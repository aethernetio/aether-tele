#!/usr/bin/env python3
"""Decode two builds with different tag indices using their exact debug maps."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from decode_raw import _BlockReader, decode, load_map


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", required=True)
    parser.add_argument("--reindexed-exe", required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="tele-compact-maps-") as directory:
        root = Path(directory)
        decoded = []
        captures = []
        indices = []
        ranker = Path(__file__).resolve().parents[2] / "tools" / "rank_raw_tags.py"
        for build, exe in enumerate((args.exe, args.reindexed_exe)):
            mapping = root / f"{build}.json"
            capture = root / f"{build}.raw"
            for option, path in (("--map", mapping), ("--fixture", capture)):
                subprocess.run([exe, option, str(path)], check=True, timeout=15)
            metadata = load_map(mapping)
            data = capture.read_bytes()
            rows, valid, truncated = decode(data, metadata)
            assert not truncated and valid == len(data) and len(rows) == 5
            assert rows[0]["payload_hex"] == bytes([0, 128, 255]).hex()
            assert rows[0]["duration_us"] == 10
            assert rows[0]["time_us"] == 1000
            assert rows[3]["payload_hex"] == (b"\xa5" * 251).hex()
            assert rows[4]["payload_hex"] == (b"\xa5" * 1501).hex()
            offset, widths = 0, []
            while offset < len(data):
                prefix = _BlockReader(data[offset:offset + 4], 4)
                size = prefix.packed("block_size")
                widths.append(prefix.cursor)
                offset += size
            assert offset == len(data) and widths == [1, 2, 4]
            indices.append(rows[0]["index"])
            decoded.append([(row["time_us"], row["duration_us"], row["payload_hex"]) for row in rows])
            captures.append(data)
            result = subprocess.run([sys.executable, str(ranker), "--map", str(mapping), str(capture)],
                                    check=True, capture_output=True, text=True, timeout=15)
            proposal = json.loads(result.stdout)["proposed_indices"]
            assert proposal[0]["old_index"] == rows[0]["index"]
            assert proposal[0]["new_index"] == 0 and proposal[0]["events"] == 4
        assert indices == [7, 2000]
        assert captures[0] != captures[1]
        assert len(captures[1]) == len(captures[0]) + 4
        assert decoded[0] == decoded[1]
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
