#!/usr/bin/env python3
"""Complete blocks decode independently across drains and process restarts."""
import struct
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from decode_raw import decode


def block(writer, time):
    return bytes([0, 1, 1, writer, 2, 1]) + struct.pack("<Q", time) + b"\x07\x01"


metadata = {"time_values_us": list(range(511)) + [60_000_000]}
times = {}
chunks = [block(3, 1000), block(4, 5000), block(3, 1001), block(3, 50)]
incremental = []
for chunk in chunks:
    rows, used, truncated = decode(chunk, metadata, times)
    assert used == len(chunk) and not truncated
    incremental.extend(rows)
assert [row["time_us"] for row in incremental] == [1001, 5001, 1002, 51]
assert decode(b"".join(chunks), metadata)[0] == incremental
