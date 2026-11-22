#!/usr/bin/env python3
# Copyright 2026 Aethernet Inc. Licensed under the Apache License, Version 2.0.
"""Decode raw tele files with their matching debug-build map into JSON lines.

This tool has no magic/checksum/build autodetection. Keep data from different
spaces/builds separate, and select the correct map explicitly.
Input is concatenated telemetry blocks; no interrupted-write recovery is performed.
"""

from __future__ import annotations

import argparse
import os
import json
from pathlib import Path
import struct
import sys


MAX_U64 = (1 << 64) - 1
CODECS = {
    "tag": (1, (239, 4095), 65535),
    "stream": (1, (15, 1023), 65535),
    "metric": (1, (200, 5000, 100_000_000), 2_182_612_529_211_367_680),
    "blob": (1, (250, 1500), None),
    "time": (1, (250, 511), 511),
}
FIELDS = [
    {"bit": 1, "name": "time_us", "type": "delta_time_code"},
    {"bit": 2, "name": "duration_us", "type": "duration_code_u8"},
    {"bit": 8, "name": "payload", "type": "blob_length_bytes"},
]


class DecodeError(ValueError):
    pass


class _Missing(Exception):
    pass


def _encode_packed(value: int, base: int, thresholds: tuple[int, ...]) -> bytes:
    """Canonical ae-numeric TieredInt representation (also bounds its range)."""
    out = bytearray(base << len(thresholds))
    size = base
    for tier in range(len(thresholds) - 1, -1, -1):
        threshold = thresholds[tier]
        if value > threshold:
            width = base << tier
            radix = 1 << (width * 8)
            quotient, low = divmod(value - threshold - 1, radix)
            value = quotient + threshold + 1
            out[width:width * 2] = low.to_bytes(width, "little")
            size = max(size, width * 2)
    if value >= 1 << (base * 8):
        raise DecodeError("packed integer exceeds its representable range")
    out[:base] = value.to_bytes(base, "little")
    return bytes(out[:size])


class _RecordReader:
    def __init__(self, data: bytes, size: int, cursor: int = 0):
        self.data, self.size, self.cursor = data, size, cursor

    def take(self, count: int) -> bytes:
        if count > self.size - self.cursor:
            raise DecodeError("record is incomplete")
        if count > len(self.data) - self.cursor:
            raise _Missing
        start = self.cursor
        self.cursor += count
        return self.data[start:self.cursor]

    def number(self, count: int) -> int:
        return int.from_bytes(self.take(count), "little")

    def packed(self, codec: str) -> int:
        base, thresholds, limit = CODECS[codec]
        start = self.cursor
        value = self.number(base)
        for tier, threshold in enumerate(thresholds):
            if value <= threshold:
                break
            width = base << tier
            low = self.number(width)
            value = ((value - threshold - 1) << (width * 8)) + threshold + 1 + low
        if limit is not None and value > limit:
            raise DecodeError(f"{codec} packed value {value} exceeds {limit}")
        if self.data[start:self.cursor] != _encode_packed(value, base, thresholds):
            raise DecodeError(f"noncanonical {codec} packed integer")
        return value


def _table(metadata: dict | None, name: str, length: int) -> list[int]:
    values = metadata.get(name) if metadata is not None else None
    if (not isinstance(values, list) or len(values) != length or
            any(type(value) is not int or value < 0 or value > 60_000_000
                for value in values) or
            values[0] != 0 or values[1] != 1 or values[-1] != 60_000_000 or
            any(a > b for a, b in zip(values, values[1:]))):
        raise DecodeError(f"matching map with exact {name} lookup table is required")
    return values


def decode(data: bytes, metadata: dict | None = None, times: dict | None = None,
           *, include_wire: bool = False, wire_stats: dict | None = None) -> tuple[list[dict], int, bool]:
    """Decode complete blocks; each writer's time base is stored in its header.

    Record wire sizes exclude headers and absolute-time reset commands. State
    and wire statistics are committed only after the entire input validates.
    """
    reader = _RecordReader(data, len(data))
    working_times = {} if times is None else times.copy()
    records = []
    stats = dict(block_count=0, writer_block_count=0, log_block_header_bytes=0,
                 metric_block_header_bytes=0, time_anchor_bytes=0)
    time_values = duration_values = None
    while reader.cursor < len(data):
        block_start = reader.cursor
        kind = reader.number(1)
        if kind > 1:
            raise DecodeError("invalid block kind (old flat captures require their old decoder)")
        writers = reader.packed("stream")
        if not writers or writers > len(data) - reader.cursor:
            raise DecodeError("invalid writer count")
        flags = reader.number(1) if kind == 0 else 0
        if flags & ~11:
            raise DecodeError("invalid block field layout")
        header_key = "log_block_header_bytes" if kind == 0 else "metric_block_header_bytes"
        stats[header_key] += reader.cursor - block_start
        stats["block_count"] += 1
        seen = set()
        for _ in range(writers):
            writer_start = reader.cursor
            stream = reader.packed("stream")
            size, count = reader.packed("blob"), reader.packed("metric")
            if stream in seen or not size or not count or count > size:
                raise DecodeError("invalid or duplicate writer body")
            seen.add(stream)
            timestamp = reader.number(8) if flags & 1 else 0
            stats[header_key] += reader.cursor - writer_start
            stats["writer_block_count"] += 1
            end = reader.cursor + size
            if end > len(data):
                raise DecodeError("writer body is incomplete")
            body = _RecordReader(data, end, reader.cursor)
            decoded_count = 0
            while decoded_count < count:
                start = body.cursor
                payload_bytes = payload_length_bytes = 0
                index = body.packed("tag")
                if index == 0:
                    if not flags & 1:
                        raise DecodeError("absolute time command in an untimed block")
                    timestamp = body.number(8)
                    stats["time_anchor_bytes"] += body.cursor - start
                    continue
                record = {"index": index, "stream": stream, "fields": flags,
                          "kind": "log" if kind == 0 else "metric"}
                if kind == 1:
                    metric = dict(zip(("count", "samples", "sum", "min", "max"),
                                      (body.packed("metric") for _ in range(5))))
                    if metric["samples"] and metric["min"] > metric["max"]:
                        raise DecodeError("invalid metric min/max")
                    record["metric"] = metric
                else:
                    if flags & 1:
                        code = body.packed("time")
                        if time_values is None:
                            time_values = _table(metadata, "time_values_us", 512)
                        timestamp += time_values[code]
                        if timestamp > MAX_U64:
                            raise DecodeError("timestamp delta overflows u64")
                        record["time_us"] = timestamp
                    if flags & 2:
                        code = body.number(1)
                        record["duration_code"] = code
                        if code in (0, 254, 255):
                            record["duration_state"] = {0: "below_min", 254: "above_max",
                                                        255: "unknown"}[code]
                        else:
                            record["duration_state"] = "value"
                            if code in (1, 253):
                                record["duration_us"] = 1 if code == 1 else 60_000_000
                            else:
                                if duration_values is None:
                                    duration_values = _table(metadata, "duration_values_us", 254)
                                record["duration_us"] = duration_values[code]
                    if flags & 8:
                        length_start = body.cursor
                        payload_bytes = body.packed("blob")
                        payload_length_bytes = body.cursor - length_start
                        record["payload_hex"] = body.take(payload_bytes).hex()
                if include_wire:
                    record["wire"] = {"offset": start, "bytes": body.cursor - start,
                                      "payload_bytes": payload_bytes,
                                      "payload_length_bytes": payload_length_bytes}
                records.append(record)
                decoded_count += 1
            if body.cursor != end:
                raise DecodeError("writer length/record count mismatch")
            reader.cursor = end
            if flags & 1:
                working_times[stream] = timestamp
    if times is not None:
        times.clear()
        times.update(working_times)
    if wire_stats is not None:
        for key, value in stats.items():
            wire_stats[key] = wire_stats.get(key, 0) + value
    return records, len(data), False


def merge_records(records: list[dict]) -> list[dict]:
    """Merge writer heads, preserving physical append order across process restarts.

    Supply captures chronologically, without overlapping persisted bytes. Start,
    stop, pause and resume are ordinary timestamped events, not framing markers.
    """
    groups: dict[int, list[dict]] = {}
    for record in records:
        groups.setdefault(record["stream"], []).append(record)
    keys = sorted(groups)
    positions = dict.fromkeys(keys, 0)
    merged = []
    while len(merged) < len(records):
        selected = None
        for key in keys:
            position = positions[key]
            if position == len(groups[key]):
                continue
            current = groups[key][position]
            if selected is None:
                selected = key
                continue
            previous = groups[selected][positions[selected]]
            if ("time_us" in current and "time_us" in previous
                    and current["time_us"] != previous["time_us"]):
                earlier = current["time_us"] < previous["time_us"]
            else:
                earlier = current["stream"] < previous["stream"]
            if earlier:
                selected = key
        assert selected is not None
        merged.append(groups[selected][positions[selected]])
        positions[selected] += 1
    return merged


def load_map(path: Path) -> dict:
    metadata = json.loads(path.read_text(encoding="utf-8"))
    if (metadata.get("byte_order") != "little" or
            metadata.get("layout") != "thread_blocks" or
            metadata.get("absolute_time_tag") != 0):
        raise DecodeError("map does not describe the thread-block telemetry layout")
    if metadata.get("fields") != FIELDS:
        raise DecodeError("map does not describe the current raw event fields")
    expected_codecs = {
        name: dict({"base_bytes": base, "thresholds": list(thresholds)},
                   **({"max_value": limit} if limit is not None else {}))
        for name, (base, thresholds, limit) in CODECS.items()
    }
    if metadata.get("numeric_codecs") != expected_codecs:
        raise DecodeError("map does not describe the current numeric codecs")
    _table(metadata, "time_values_us", 512)
    _table(metadata, "duration_values_us", 254)
    tags = metadata.get("tags", [])
    seen = set()
    for tag in tags:
        if type(tag["index"]) is not int or not 1 <= tag["index"] <= 65535:
            raise DecodeError(f"invalid map index {tag['index']}")
        if tag["index"] in seen:
            raise DecodeError(f"duplicate map index {tag['index']}")
        seen.add(tag["index"])
    by_index = {tag["index"]: tag for tag in tags}
    observed = set()
    if metadata.get("callsite_capacity_exhausted", False):
        raise DecodeError("map callsite registration overflowed")
    for site in metadata.get("observed_callsites", []):
        index = site["index"]
        if index in observed or index not in by_index:
            raise DecodeError(f"non-unique or undeclared callsite index {index}")
        observed.add(index)
        if any(site[key] != by_index[index][key] for key in ("name", "text", "module", "level")):
            raise DecodeError(f"callsite metadata conflicts with tag {index}")
    return metadata


def restore_metrics(records: list[dict]) -> dict[int, dict]:
    """Sum acknowledged disjoint metric batches; never infer restart groups."""
    result: dict[int, dict] = {}
    maximum = CODECS["metric"][2]
    for record in records:
        if "metric" not in record:
            continue
        source = record["metric"]
        target = result.setdefault(record["index"], dict.fromkeys(
            ("count", "samples", "sum", "min", "max"), 0))
        target["count"] = min(maximum, target["count"] + source["count"])
        target["sum"] = min(maximum, target["sum"] + source["sum"])
        if source["samples"]:
            if not target["samples"]:
                target["min"], target["max"] = source["min"], source["max"]
            else:
                target["min"] = min(target["min"], source["min"])
                target["max"] = max(target["max"], source["max"])
        target["samples"] = min(maximum, target["samples"] + source["samples"])
    return result


def annotate(record: dict, metadata: dict, tags: dict[int, dict]) -> dict:
    result = dict(record)
    result["space"] = metadata.get("space", "")
    result["application_build"] = metadata.get("application_build", "")
    tag = tags.get(record["index"])
    if tag is None:
        raise DecodeError(f"index {record['index']} is absent from the selected build map")
    # Static labels live only in the selected build map.
    result["tag_name"] = tag.get("name", "")
    result["description"] = tag.get("text", "")
    result["level"] = tag["level"]
    result["module"] = tag["module"]
    result["declaration"] = {"file": tag.get("file", ""), "line": tag.get("line", 0)}
    sites = [{"file": site["file"], "line": site["line"]}
             for site in metadata.get("observed_callsites", [])
             if site["index"] == record["index"]]
    if len(sites) > 1:
        raise DecodeError(f"ambiguous callsite for tag {record['index']}")
    # Never label a tag declaration as an observed logging callsite.
    result["source"] = sites[0] if sites else None
    return result


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--map", type=Path, required=True, dest="map_path")
    parser.add_argument("--output", type=Path, help="JSONL destination (default stdout)")
    parser.add_argument("--metrics", action="store_true",
                        help="restore totals by summing acknowledged metric batches")
    parser.add_argument("files", type=Path, nargs="+",
                        help="non-overlapping raw files from one space/build, in append order")
    args = parser.parse_args(argv)
    try:
        metadata = load_map(args.map_path)
        tags = {tag["index"]: tag for tag in metadata["tags"]}
        records = []
        seen_paths = set()
        times = {}
        for path in args.files:
            canonical = os.path.normcase(str(path.resolve()))
            if canonical in seen_paths:
                raise DecodeError(f"capture file supplied twice: {path}")
            seen_paths.add(canonical)
            decoded, valid_bytes, truncated = decode(path.read_bytes(), metadata, times)
            records.extend(decoded)
        if not args.metrics and any("time_us" not in record for record in records):
            print("Some timestamps are disabled: only per-writer order is recoverable; "
                  "cross-writer ties use stream ID.", file=sys.stderr)
        ordered = merge_records(records)
        if args.metrics:
            result = []
            for index, metric in sorted(restore_metrics(ordered).items()):
                if index not in tags:
                    raise DecodeError(f"index {index} is absent from the selected build map")
                result.append({"index": index, "metric": metric,
                               "space": metadata.get("space", ""),
                               "application_build": metadata.get("application_build", ""),
                               "tag_name": tags[index].get("name", "")})
        else:
            result = [annotate(record, metadata, tags) for record in ordered]
        text = "".join(json.dumps(record, ensure_ascii=False) + "\n" for record in result)
        if args.output:
            args.output.write_text(text, encoding="utf-8")
        else:
            sys.stdout.write(text)
        return 0
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"raw tele decode failed: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
