#!/usr/bin/env python3
"""Continue compact telemetry statistics through independent process lifetimes."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile


sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
from decode_raw import _BlockReader


def blocks(data):
    offset = 0
    while offset < len(data):
        size = _BlockReader(data[offset:offset + 4], 4).packed("block_size")
        assert size >= 6 and offset + size <= len(data)
        yield data[offset:offset + size]
        offset += size


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--exe', required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='tele-compact-restart-') as directory:
        root = Path(directory)
        capture = root / 'capture.raw'
        mapping = root / 'capture.map.json'
        subprocess.run([args.exe, '--map', str(mapping)], check=True, timeout=15)
        accepted = b''
        for boot in range(5):
            if boot:
                # An interrupted next append may contain a valid header but only
                # part of its rows. Repair must not change earlier blocks.
                tail = list(blocks(accepted))[-1]
                with capture.open('ab') as stream:
                    stream.write(tail[:-1])
            subprocess.run([args.exe, '--boot', str(capture), str(boot)], check=True, timeout=15)
            contents = capture.read_bytes()
            assert contents.startswith(accepted)
            assert len(list(blocks(contents))) == (boot + 1) * 4
            subprocess.run([args.exe, '--verify', str(capture), str(boot + 1)], check=True, timeout=15)
            assert capture.read_bytes() == contents, 'restore and empty drain must not re-emit history'
            accepted = contents
        decoder = Path(__file__).resolve().parents[2] / 'tools' / 'decode_raw.py'
        result = subprocess.run([sys.executable, str(decoder), '--map', str(mapping),
                                 '--metrics', str(capture)], check=True,
                                capture_output=True, text=True, timeout=15)
        metrics = [json.loads(line) for line in result.stdout.splitlines() if line.strip()]
        metric = next(row['metric'] for row in metrics if row['index'] == 7)
        assert metric == {'count': 15, 'samples': 15, 'sum': 570, 'min': 5, 'max': 104}
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
