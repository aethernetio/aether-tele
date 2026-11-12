# Copyright 2026 Aethernet Inc. Licensed under the Apache License, Version 2.0.
"""Independent compact-wire fixtures for the host Python decoder."""
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from decode_raw import (CODECS, DecodeError, _BlockReader, _encode_packed,
                        decode, merge_records, restore_metrics)


def packed(name, value):
    base, thresholds, _ = CODECS[name]
    return _encode_packed(value, base, thresholds)


def block(rows, flags=0, stream=1, sequence=0, metric=False, wide=False,
          base_time=0):
    body = bytes([(1 if metric else 0) | (flags << 2) | (128 if wide else 0)])
    body += packed("stream", stream)
    body += struct.pack("<Q", sequence) if wide else packed("sequence", sequence)
    if flags & 1:
        body += struct.pack("<Q", base_time)
    body += rows
    for width in (1, 2, 4):
        prefix = packed("block_size", len(body) + width)
        if len(prefix) == width:
            return prefix + body
    raise ValueError("block is too large")


def tables():
    return {"time_values_us": [0] + [1] * 510 + [60_000_000],
            "duration_values_us": [0] + [1] * 252 + [60_000_000]}


class CompactDecoderTest(unittest.TestCase):
    def test_block_size_prefixes_and_torn_headers(self):
        for size, prefix in ((250, b"\xfa"), (251, b"\xfb\x00"),
                             (1500, b"\xff\xe1"), (1501, b"\xff\xe2\x00\x00")):
            body = b"\x00\x01\x00\x00" + b"\x07" * (size - len(prefix) - 4)
            data = prefix + body
            with self.subTest(size=size):
                rows, valid, truncated = decode(data)
                self.assertEqual(len(rows), len(body) - 4)
                self.assertEqual(valid, size)
                self.assertFalse(truncated)
                for cut in range(1, size):
                    rows, valid, truncated = decode(data[:cut])
                    self.assertTrue(truncated)
                    self.assertEqual((rows, valid), ([], 0))
        for size in range(6):
            with self.subTest(invalid=size), self.assertRaises(DecodeError):
                decode(bytes([size]))

    def test_blob_approved_wire_vectors(self):
        # Fixed bytes lock the approved 250/1500 tiers independently of the
        # decoder's codec descriptors and the encoder used by other fixtures.
        for value, expected in ((0, b"\x00"), (250, b"\xfa"),
                                (251, b"\xfb\x00"),
                                (1500, b"\xff\xe1"),
                                (1501, b"\xff\xe2\x00\x00"),
                                (1_967_580, b"\xff\xff\xff\xff")):
            with self.subTest(value=value):
                self.assertEqual(packed("blob", value), expected)
                reader = _BlockReader(expected, len(expected))
                self.assertEqual(reader.packed("blob"), value)
                self.assertEqual(reader.cursor, len(expected))

    def test_packed_tier_boundaries(self):
        for name, (base, thresholds, limit) in CODECS.items():
            values = {0, 1}
            for boundary in thresholds:
                values.update((boundary - 1, boundary, boundary + 1))
            if limit is not None:
                values = {value for value in values if value <= limit}
                values.add(limit)
            for value in values:
                with self.subTest(codec=name, value=value):
                    wire = packed(name, value)
                    reader = _BlockReader(wire, len(wire))
                    self.assertEqual(reader.packed(name), value)
                    self.assertEqual(reader.cursor, len(wire))

    def test_atomic_torn_block(self):
        prefix = block(packed("tag", 9))
        following = block(packed("tag", 1) + packed("tag", 50000), sequence=1)
        complete = prefix + following
        self.assertEqual(len(decode(complete)[0]), 3)
        for kept in range(1, len(following)):
            with self.subTest(kept=kept):
                records, valid, truncated = decode(prefix + following[:kept])
                self.assertTrue(truncated)
                self.assertEqual(valid, len(prefix))
                self.assertEqual([row["index"] for row in records], [9])

    def test_complete_incomplete_row_is_malformed(self):
        row = packed("tag", 50000)[:-1]
        with self.assertRaises(DecodeError):
            decode(block(row))
        with self.assertRaises(DecodeError):
            decode(block(b""))

    def test_duration_states_and_payload(self):
        rows = b"".join(packed("tag", index) + bytes([code]) +
                        packed("blob", 2) + b"ab"
                        for index, code in enumerate((0, 1, 253, 254, 255)))
        records, _, _ = decode(block(rows, flags=2 | 8))
        self.assertEqual([record["duration_state"] for record in records],
                         ["below_min", "value", "value", "above_max", "unknown"])
        self.assertEqual([record.get("duration_us") for record in records],
                         [None, 1, 60_000_000, None, None])
        self.assertTrue(all(record["payload_hex"] == "6162" for record in records))
        self.assertEqual([record["sequence"] for record in records], list(range(5)))

    def test_exact_table_time_and_duration(self):
        data = block(packed("tag", 0) + packed("time", 511) + bytes([252]),
                     flags=1 | 2, base_time=500)
        with self.assertRaises(DecodeError):
            decode(data)
        records, _, _ = decode(data, tables())
        self.assertEqual(records[0]["time_us"], 60_000_500)
        self.assertEqual(records[0]["duration_us"], 1)
        with self.assertRaises(DecodeError):
            decode(block(packed("tag", 0) + packed("time", 1),
                         flags=1, base_time=(1 << 64) - 1), tables())

    def test_exact_metrics_and_wide_values(self):
        small = [248, 3, 32768, 1, 50000]
        rows = packed("tag", 33) + b"".join(packed(codec, value)
                for codec, value in zip(("count", "count", "sum", "sum", "sum"), small))
        huge = [(1 << 64) - 1, 1, (1 << 64) - 1, (1 << 64) - 1, (1 << 64) - 1]
        data = block(rows, metric=True) + block(
            packed("tag", 33) + struct.pack("<QQQQQ", *huge),
            metric=True, wide=True, sequence=(1 << 64) - 1)
        records, valid, truncated = decode(data)
        self.assertFalse(truncated)
        self.assertEqual(valid, len(data))
        self.assertEqual(list(records[1]["metric"].values()), huge)
        total = restore_metrics(records)[33]
        self.assertEqual(total["count"], (1 << 64) - 1)
        self.assertEqual(total["samples"], 4)
        self.assertEqual(total["min"], 1)

    def test_invalid_control_ranges_and_sequence_overflow(self):
        for control in (2, 3, 16, 64, 5):
            invalid = bytearray(block(b"\0"))
            invalid[1] = control
            with self.subTest(control=control), self.assertRaises(DecodeError):
                decode(invalid)
        with self.assertRaises(DecodeError):
            decode(block(b"\0\0", wide=True, sequence=(1 << 64) - 1))
        with self.assertRaises(DecodeError):
            decode(block(packed("tag", 65536)))

    def test_merge_preserves_restart_order(self):
        records = decode(block(b"\0\1", stream=1, sequence=5) +
                         block(b"\2", stream=1, sequence=0) +
                         block(b"\3", stream=2, sequence=0))[0]
        self.assertEqual([row["index"] for row in merge_records(records)], [0, 1, 2, 3])


if __name__ == "__main__":
    unittest.main()
