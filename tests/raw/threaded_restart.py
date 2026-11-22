#!/usr/bin/env python3
# Copyright 2026 Aethernet Inc. Licensed under the Apache License, Version 2.0.
"""Restart real processes; validate concurrent append, restore and byte accounting."""

import argparse
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from analyze_raw import analyze
from decode_raw import decode, load_map


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def run(args, directory):
    capture = directory / "capture.raw"
    mapping = directory / "map.json"
    previous = b""
    runs = []
    for boot in range(args.boots):
        command = [str(args.exe.resolve()), "--capture", str(capture),
                   "--map", str(mapping), "--boot", str(boot),
                   "--workers", str(args.workers), "--events", str(args.events),
                   "--payload-bytes", str(args.payload_bytes)]
        # A checkpointed exit without destructors/close/stop markers.
        abrupt = boot == args.boots // 2
        with subprocess.Popen(command + (["--abrupt-exit"] if abrupt else []),
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              text=True) as process:
            try:
                stdout, stderr = process.communicate(timeout=60)
            except subprocess.TimeoutExpired:
                process.kill()
                process.communicate()
                raise
            require(process.returncode == 0, f"boot {boot} failed: {stderr}")
            observations = [json.loads(line) for line in stdout.splitlines()]
            require(len(observations) == args.workers, "missing writer observation")
            for writer, item in enumerate(observations, 1):
                require(item["writer"] == writer and item["boot"] == boot,
                        "incorrect logical writer binding")
                require(item["restored_count"] == boot * args.events and
                        item["count"] == (boot + 1) * args.events,
                        "RAM totals did not continue across process restart")
            require(len({item["thread_id"] for item in observations}) == args.workers,
                    "workers did not run on distinct threads")
            require(len({item["native_thread_id"] for item in observations}) == args.workers,
                    "workers did not have distinct native thread IDs")
            runs.append(dict(boot=boot, pid=process.pid, abrupt_exit=abrupt,
                             writers=observations))
        contents = capture.read_bytes()
        require(contents.startswith(previous), "restart overwrote historical bytes")
        metadata = load_map(mapping)
        rows, used, truncated = decode(contents[len(previous):], metadata)
        require(used == len(contents) - len(previous) and not truncated,
                "incomplete appended capture")
        require({row["stream"] for row in rows} == set(range(1, args.workers + 1)),
                "unexpected or missing writer in capture")
        for writer in range(1, args.workers + 1):
            owned = [row for row in rows if row["stream"] == writer]
            require(all("sequence" not in row for row in owned),
                    "the decoder exposed an obsolete sequence field")
            logs = [row for row in owned if row["kind"] == "log"]
            require(len(logs) == args.events, "missing or duplicated logs")
            for item, row in enumerate(logs):
                payload = bytes.fromhex(row["payload_hex"])
                require(len(payload) == args.payload_bytes and
                        struct.unpack("<III", payload[:12]) == (boot, writer, item) and
                        payload[12:] == b"\xa5" * (args.payload_bytes - 12),
                        "payload corruption or incorrect append order")
                timestamp = 1_700_000_000_000_000 + boot * 2_000_000_000 + item * 1000
                require(0 <= timestamp - row["time_us"] < 100,
                        "timestamp did not reset/continue correctly")
                require(row.get("duration_state") == "value", "scope duration missing")
        # A fresh process restores the next boot's totals without changing data.
        verify = command.copy()
        verify[verify.index("--boot") + 1] = str(boot + 1)
        subprocess.run(verify + ["--verify-only"], check=True, timeout=60)
        require(capture.read_bytes() == contents, "restore modified the capture")
        previous = contents
        print(f"boot {boot}: {args.workers} writers, "
              f"{args.events * args.workers} new logs, {len(contents)} bytes", flush=True)

    report, _ = analyze(previous, metadata)
    total = args.boots * args.workers * args.events
    expected_metric = dict(count=total, samples=total, sum=total * 5, min=5, max=5)
    require(report["log_records"] == total and report["metrics"] == {7: expected_metric},
            "persisted aggregate metrics do not match the complete workload")
    per_writer = total // args.workers
    for entry in report["writers"].values():
        require(entry["log_records"] == per_writer and entry["metrics"] == {
            7: dict(count=per_writer, samples=per_writer, sum=per_writer * 5, min=5, max=5)},
            "one writer inherited another writer's totals")
    require(report["payload_bytes"] == total * args.payload_bytes,
            "incorrect payload accounting")
    require(report["capture_bytes"] == report["log_bytes_excluding_payload"] +
            report["payload_bytes"] + report["time_anchor_bytes"] + report["metric_bytes"] +
            report["log_block_header_bytes"],
            "wire bytes do not reconcile")
    report["test"] = dict(boots=args.boots, workers=args.workers,
                          events_per_worker_per_boot=args.events,
                          payload_bytes_per_log=args.payload_bytes,
                          clock="synthetic: 1000 us between events, 5 us scope duration",
                          checkpoint_every_events=127, runs=runs)
    (directory / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    subprocess.run([sys.executable, str(Path(__file__).resolve().parents[2] / "tools" / "analyze_raw.py"),
                    str(capture), "--map", str(mapping),
                    "--records", str(directory / "records.jsonl"),
                    "--output", str(directory / "analysis.json")], check=True, timeout=60)
    require(len((directory / "records.jsonl").read_text(encoding="utf-8").splitlines()) ==
            report["log_records"] + report["metric_records"], "JSONL record count mismatch")
    print(json.dumps({key: value for key, value in report.items()
                      if key not in ("test", "writers", "metrics")}, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--boots", type=int, default=5)
    parser.add_argument("--workers", type=int, default=4)
    parser.add_argument("--events", type=int, default=1000)
    parser.add_argument("--payload-bytes", type=int, default=12)
    parser.add_argument("--output-dir", type=Path,
                        help="new directory in which to retain raw/map/JSON artifacts")
    args = parser.parse_args()
    if not (2 <= args.boots <= 100 and 1 <= args.workers <= 32 and
            1 <= args.events <= 1_000_000 and 12 <= args.payload_bytes <= 2000):
        parser.error("boots 2..100, workers 1..32, events 1..1000000, payload-bytes 12..2000")
    if args.output_dir:
        directory = args.output_dir.resolve()
        directory.mkdir(parents=True, exist_ok=False)
        run(args, directory)
    else:
        with tempfile.TemporaryDirectory(prefix="tele-threaded-restart-") as tmp:
            run(args, Path(tmp))


if __name__ == "__main__":
    main()
