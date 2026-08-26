#!/usr/bin/env python3
# Copyright 2026 Aethernet Inc.
"""Rewrite direct tag indices from a frequency report. Dry-run by default."""

from __future__ import annotations

import argparse
import difflib
import json
import re
import sys
from pathlib import Path

TABLE_BEGIN = "TELE_INDEX_TABLE_BEGIN"
TABLE_END = "TELE_INDEX_TABLE_END"
TAG_LINE_RE = re.compile(
    r"^(?P<prefix>\s*X\()(?P<name>[A-Za-z_][A-Za-z0-9_]*)\s*,\s*"
    r"(?P<index>\d+)(?P<rest>\s*,.*)$"
)


def header_for_space(space: str, root: Path) -> Path:
    if space == "network":
        return root / "include" / "demo" / "network_space.h"
    if space == "application":
        return root / "include" / "demo" / "application_space.h"
    raise SystemExit(f"unknown space {space}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--space", required=True, choices=("network", "application"))
    parser.add_argument("--root", default=".")
    parser.add_argument("--report", required=True)
    parser.add_argument("--apply", action="store_true")
    args = parser.parse_args()

    header = header_for_space(args.space, Path(args.root).resolve())
    original = header.read_text(encoding="utf-8")
    report = json.loads(Path(args.report).read_text(encoding="utf-8"))
    counts = {m["name"]: int(m["count"]) for m in report.get("metrics", [])}
    begin = original.find(TABLE_BEGIN)
    end = original.find(TABLE_END)
    names = []
    for line in original[begin:end].splitlines():
        match = TAG_LINE_RE.match(line.rstrip())
        if match:
            names.append(match.group("name"))
    mapping = {
        name: i
        for i, name in enumerate(sorted(names, key=lambda n: (-counts.get(n, 0), n)))
    }
    new_lines = []
    for line in original[begin:end].splitlines(keepends=True):
        raw = line.rstrip("\n")
        nl = "\n" if line.endswith("\n") else ""
        match = TAG_LINE_RE.match(raw)
        if not match:
            new_lines.append(line)
            continue
        name = match.group("name")
        new_lines.append(
            f"{match.group('prefix')}{name}, {mapping[name]}{match.group('rest')}{nl}"
        )
    updated = original[:begin] + "".join(new_lines) + original[end:]
    diff = "".join(
        difflib.unified_diff(
            original.splitlines(keepends=True),
            updated.splitlines(keepends=True),
            fromfile=str(header),
            tofile=str(header),
        )
    )
    if not diff:
        print("no index changes")
        return 0
    sys.stdout.write(diff)
    if args.apply:
        header.write_text(updated, encoding="utf-8")
        print(f"applied {header}", file=sys.stderr)
    else:
        print("dry-run only; pass --apply to write", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
