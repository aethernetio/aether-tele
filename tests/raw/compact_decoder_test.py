#!/usr/bin/env python3
"""Flat golden records: no sizes, versions or block headers."""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from decode_raw import decode


def anchor(writer, time):
    return bytes([0, writer]) + struct.pack("<Q", time)


def event(writer, sequence, timed=False):
    return bytes([7, 4 if timed else 0, writer]) + struct.pack("<H", sequence) + (b"\x01" if timed else b"")


# No envelope: an anchor and a no-fields log concatenate directly.
data = anchor(3, 1000) + event(3, 0)
rows, used, truncated = decode(data)
assert len(rows) == 1 and rows[0]["index"] == 7
assert used == len(data) and not truncated and rows[0]["fields"] == 0

# Codes 0 and 1 have exact values independent of the exponential table.
metadata = {"time_values_us": list(range(511)) + [60_000_000]}
times = {}
chunks = [anchor(3, 1000) + event(3, 0, True),
          anchor(4, 5000) + event(4, 0, True),
          event(3, 1, True),  # drain continuation has no new anchor
          anchor(3, 50) + event(3, 0, True)]  # restart without stop
incremental = []
for chunk in chunks:
    rows, used, truncated = decode(chunk, metadata, times)
    assert used == len(chunk) and not truncated
    incremental.extend(rows)
assert [row["time_us"] for row in incremental] == [1001, 5001, 1002, 51]
assert decode(b"".join(chunks), metadata)[0] == incremental
