#!/usr/bin/env python3
"""Exercise compact native blocks at process-crash and ownership boundaries."""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
from decode_raw import _BlockReader, decode, restore_metrics


def metric_rows(contents):
    records, valid, truncated = decode(contents)
    assert not truncated and valid == len(contents)
    assert all(row['kind'] == 'metric' for row in records)
    return records


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--exe', required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='tele-compact-file-') as directory:
        root = Path(directory)
        path = root / 'capture.raw'
        subprocess.run([args.exe, '--write', str(path)], check=True, timeout=15)
        acknowledged = path.read_bytes()
        assert acknowledged, 'acknowledged logs survive without stop or destructors'
        block_size = _BlockReader(acknowledged[:4], 4).packed("block_size")
        assert block_size > 17
        torn = acknowledged[:17]
        with path.open('ab') as stream:
            stream.write(torn)
        subprocess.run([args.exe, '--resume', str(path)], check=True, timeout=15)
        assert path.read_bytes().startswith(acknowledged)
        subprocess.run([args.exe, '--verify', str(path)], check=True, timeout=15)

        metrics_path = root / 'metrics.raw'
        previous = b''
        for boot in range(5):
            if boot:
                with metrics_path.open('ab') as stream:
                    stream.write(torn)
            subprocess.run([args.exe, '--metrics-boot', str(metrics_path), str(boot)],
                           check=True, timeout=15)
            contents = metrics_path.read_bytes()
            assert contents.startswith(previous)
            rows = metric_rows(contents)
            assert len(rows) == (boot + 1) * 3
            boots = boot + 1
            for writer in (3, 9):
                total = restore_metrics([row for row in rows if row['stream'] == writer])[7]
                if writer == 3:
                    assert total == dict(count=2 * boots, samples=2 * boots,
                                         sum=boots * (boots + 2), min=1, max=boots + 1)
                else:
                    assert total == dict(count=boots, samples=boots,
                                         sum=100 * boots + boots * (boots - 1) // 2,
                                         min=100, max=99 + boots)
            previous = contents
        subprocess.run([args.exe, '--metrics-check', str(metrics_path), '5'],
                       check=True, timeout=15)
        assert metrics_path.read_bytes() == previous

        # A new process repairs every possible cut inside the 2/4-byte size
        # prefix, and complete prefixes whose body was never written.
        for tail in (b"\xfb", b"\xfb\x00", b"\xff", b"\xff\xe2",
                     b"\xff\xe2\x00", b"\xff\xe2\x00\x00"):
            with metrics_path.open('ab') as stream:
                stream.write(tail)
            subprocess.run([args.exe, '--metrics-check', str(metrics_path), '5'],
                           check=True, timeout=15)
            assert metrics_path.read_bytes() == previous

        # Terminate inside the storage callback, before its acknowledgement is
        # observed. Only a fully written block may contribute restored totals.
        for point in ('partial', 'full', 'synced', 'rollback'):
            crash_path = root / f'crash-{point}.raw'
            for mode in ('--metrics-crash', '--metrics-recover', '--metrics-recovered-check'):
                subprocess.run([args.exe, mode, str(crash_path), point],
                               check=True, timeout=15)
            rows = metric_rows(crash_path.read_bytes())
            full = point in ('full', 'synced')
            assert len(rows) == (3 if full else 2)
            assert rows[0]['metric'] == dict(count=1, samples=1, sum=10, min=10, max=10)
            assert rows[-1]['metric'] == dict(count=1, samples=1, sum=7, min=7, max=7)
            if full:
                assert rows[1]['metric'] == dict(count=1, samples=1, sum=5, min=5, max=5)

        ready = root / 'writer.ready'
        holder = subprocess.Popen([args.exe, '--hold', str(path), str(ready)],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, text=True)
        try:
            deadline = time.monotonic() + 8
            while not ready.exists():
                if holder.poll() is not None:
                    out, err = holder.communicate()
                    raise RuntimeError(f'file holder exited: {out}\n{err}')
                if time.monotonic() >= deadline:
                    raise TimeoutError('file holder did not signal readiness')
                time.sleep(0.01)
            subprocess.run([args.exe, '--exclusive-probe', str(path)],
                           check=True, timeout=10)
            holder.communicate('release\n', timeout=10)
            assert holder.returncode == 0
        finally:
            if holder.poll() is None:
                holder.kill()
                holder.communicate()
        subprocess.run([args.exe, '--verify', str(path)], check=True, timeout=15)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
