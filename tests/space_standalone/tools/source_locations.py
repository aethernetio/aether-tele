#!/usr/bin/env python3
# Copyright 2026 Aethernet Inc.
"""Locate each space tag macro invocation and verify uniqueness."""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

MACRO_RE = re.compile(
    r"\b(?:NET|APP)_TELE_(?:INFO|WARNING|ERROR|DEBUG)\s*\(\s*"
    r"[^,]+,\s*(?:demo::(?:network|application)::)?(k[A-Za-z0-9_]+)"
)
TABLE_BEGIN = "TELE_INDEX_TABLE_BEGIN"
TABLE_END = "TELE_INDEX_TABLE_END"
TAG_X_RE = re.compile(r"X\(([A-Za-z_][A-Za-z0-9_]*)\s*,\s*(\d+)\s*,")


def header_for_space(space: str, root: Path) -> Path:
    if space == "network":
        return root / "include" / "demo" / "network_space.h"
    if space == "application":
        return root / "include" / "demo" / "application_space.h"
    raise SystemExit(f"unknown space {space}")


def read_table_tags(header: Path) -> list[str]:
    text = header.read_text(encoding="utf-8")
    begin = text.find(TABLE_BEGIN)
    end = text.find(TABLE_END)
    if begin < 0 or end < 0 or end <= begin:
        raise SystemExit(f"index table markers missing in {header}")
    return [m.group(1) for m in TAG_X_RE.finditer(text[begin:end])]


def scan_sources(src_root: Path) -> dict[str, list[tuple[str, int]]]:
    found: dict[str, list[tuple[str, int]]] = {}
    skip = {
        "unit_tests.cpp",
        "main.cpp",
        "heap_probe.cpp",
        "stack_probe.cpp",
    }
    for path in sorted(src_root.rglob("*")):
        if path.suffix not in {".cpp", ".cc", ".h", ".hpp"}:
            continue
        if path.name in skip:
            continue
        for i, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            code = line.split("//", 1)[0]
            for match in MACRO_RE.finditer(code):
                symbol = match.group(1)
                tag = symbol[1:] if symbol.startswith("k") else symbol
                found.setdefault(tag, []).append((path.as_posix(), i))
    return found


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--space", required=True, choices=("network", "application"))
    parser.add_argument("--root", default=".")
    parser.add_argument("--json")
    parser.add_argument("--decode-json")
    parser.add_argument("--exe")
    parser.add_argument("--blob")
    parser.add_argument("--print-lines", action="store_true")
    parser.add_argument("--limit", type=int, default=12)
    args = parser.parse_args()

    root = Path(args.root).resolve()
    tags = read_table_tags(header_for_space(args.space, root))
    found = scan_sources(root / "src")
    errors = []
    locations: dict[str, tuple[str, int]] = {}
    for tag in tags:
        spots = found.get(tag, [])
        if len(spots) != 1:
            errors.append(f"{tag}: expected 1 invocation, found {len(spots)} {spots}")
        else:
            rel = spots[0][0]
            try:
                rel = str(Path(rel).resolve().relative_to(root)).replace("\\", "/")
            except ValueError:
                rel = spots[0][0]
            locations[tag] = (rel, spots[0][1])
    if errors:
        print("source location check failed:", file=sys.stderr)
        for err in errors:
            print("  " + err, file=sys.stderr)
        return 1

    decoded = None
    if args.exe and args.blob:
        proc = subprocess.run(
            [args.exe, "--phase=decode", "--space", args.space, args.blob],
            capture_output=True,
            text=True,
            check=False,
        )
        if proc.returncode != 0:
            print(proc.stdout + proc.stderr, file=sys.stderr)
            return proc.returncode
        decoded = json.loads(proc.stdout)
    elif args.decode_json:
        decoded = json.loads(Path(args.decode_json).read_text(encoding="utf-8"))
    elif args.json:
        decoded = json.loads(Path(args.json).read_text(encoding="utf-8"))

    if decoded is not None:
        unknown = 0
        events = decoded.get("events") or []
        rows = events if events else decoded.get("metrics") or []
        shown = 0
        for event in rows:
            name = event.get("name", "")
            loc = locations.get(name)
            if loc is None:
                unknown += 1
                loc_s = "UNKNOWN"
            else:
                loc_s = f"{loc[0]}:{loc[1]}"
            if events:
                line = (
                    f"+{event.get('rel_time', 0)} ticks | stream="
                    f"{event.get('stream', '')} | {name} | "
                    f"{event.get('module', '')} | {event.get('severity', '')} | "
                    f"{loc_s}"
                )
            else:
                line = f"{name} | {loc_s} | count={event.get('count', 0)}"
            if shown < args.limit:
                print(line)
            shown += 1
        if events and shown > args.limit:
            print(f"... {shown} events, showing first {args.limit}")
        if unknown:
            print(f"ERROR: {unknown} events without a unique call site", file=sys.stderr)
            return 1
    if args.print_lines:
        for tag, (rel, line) in locations.items():
            print(f"{tag} | {rel}:{line}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
