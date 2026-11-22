#!/usr/bin/env python3
# Copyright 2026 Aethernet Inc. Licensed under the Apache License, Version 2.0.
"""Decode one space/build's capture and measure actual binary bytes per log.

Only parameter contents are subtracted: their length prefixes remain overhead.
Time anchors and metric batches are reported separately and amortized as well.
"""

from __future__ import annotations

import argparse
from collections import Counter
import json
from pathlib import Path
import sys

from decode_raw import DecodeError, annotate, decode, load_map, restore_metrics


def analyze(data: bytes, metadata: dict) -> tuple[dict, list[dict]]:
    framing = {}
    rows, used, truncated = decode(data, metadata, include_wire=True, wire_stats=framing)
    if used != len(data) or truncated:
        raise DecodeError("capture was not completely decoded")
    tags = {tag["index"]: tag for tag in metadata["tags"]}
    # Validate the selected map for metrics too, not just visible log rows.
    decoded = [annotate(row, metadata, tags) for row in rows]
    logs = [row for row in rows if row["kind"] == "log"]
    metrics = [row for row in rows if row["kind"] == "metric"]
    log_bytes = sum(row["wire"]["bytes"] for row in logs)
    metric_record_bytes = sum(row["wire"]["bytes"] for row in metrics)
    metric_bytes = metric_record_bytes + framing["metric_block_header_bytes"]
    payload_bytes = sum(row["wire"]["payload_bytes"] for row in logs)
    prefixes = sum(row["wire"]["payload_length_bytes"] for row in logs)
    anchors = framing["time_anchor_bytes"]
    log_headers = framing["log_block_header_bytes"]
    if len(data) != log_bytes + metric_bytes + anchors + log_headers:
        raise DecodeError("block/record byte accounting does not reconcile")
    overhead = log_bytes - payload_bytes
    count = len(logs)
    per_log = lambda value: value / count if count else None
    histogram = Counter(row["wire"]["bytes"] - row["wire"]["payload_bytes"]
                        for row in logs)
    by_writer = {}
    for writer in sorted({row["stream"] for row in rows}):
        writer_rows = [row for row in rows if row["stream"] == writer]
        by_writer[str(writer)] = {
            "log_records": sum(row["kind"] == "log" for row in writer_rows),
            "metrics": restore_metrics(writer_rows),
        }
    return {
        "capture_bytes": len(data),
        "log_records": count,
        "metric_records": len(metrics),
        "log_record_bytes": log_bytes,
        "payload_bytes": payload_bytes,
        "payload_length_bytes": prefixes,
        "log_bytes_excluding_payload": overhead,
        "metric_bytes": metric_bytes,
        "metric_record_bytes": metric_record_bytes,
        **framing,
        "average_log_bytes_excluding_payload": per_log(overhead),
        "average_with_time_anchors_excluding_payload": per_log(overhead + anchors),
        "average_with_log_block_headers_excluding_payload": per_log(overhead + anchors + log_headers),
        "average_capture_bytes_excluding_payload": per_log(len(data) - payload_bytes),
        "log_overhead_histogram": dict(sorted(histogram.items())),
        "metrics": restore_metrics(rows),
        "writers": by_writer,
    }, decoded


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    parser.add_argument("--map", type=Path, required=True, dest="map_path")
    parser.add_argument("--output", type=Path, help="summary JSON (default stdout)")
    parser.add_argument("--records", type=Path, help="decoded logs and metrics as JSONL in append order")
    args = parser.parse_args(argv)
    try:
        inputs = {args.capture.resolve(), args.map_path.resolve()}
        outputs = [path.resolve() for path in (args.output, args.records) if path]
        if any(path in inputs for path in outputs) or len(outputs) != len(set(outputs)):
            raise ValueError("output paths must be distinct from inputs and each other")
        report, rows = analyze(args.capture.read_bytes(), load_map(args.map_path))
        if args.records:
            with args.records.open("w", encoding="utf-8") as output:
                for row in rows:
                    output.write(json.dumps(row, ensure_ascii=False) + "\n")
        text = json.dumps(report, indent=2, ensure_ascii=False) + "\n"
        if args.output:
            args.output.write_text(text, encoding="utf-8")
        else:
            sys.stdout.write(text)
        return 0
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"raw tele analysis failed: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
