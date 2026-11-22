#!/usr/bin/env python3
"""Offline product telemetry parsing using exact local catalog hashes."""
import argparse
import json
from pathlib import Path
import sys
from telemetry_artifact import PRODUCTS, decode_artifact


def main(argv=None, product=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("files", type=Path, nargs="+")
    parser.add_argument("--catalogs", type=Path, required=True)
    if product is None:
        parser.add_argument("--product", choices=sorted(PRODUCTS), required=True)
    parser.add_argument("--output", type=Path, help="decoded JSONL, default stdout")
    parser.add_argument("--summary", type=Path)
    args = parser.parse_args(argv)
    product = product or args.product
    try:
        paths = [path.resolve() for path in args.files]
        if len(paths) != len(set(paths)):
            raise ValueError("artifact supplied twice")
        outputs = [path.resolve() for path in (args.output, args.summary) if path]
        if len(outputs) != len(set(outputs)) or any(path in paths for path in outputs):
            raise ValueError("outputs must be distinct from inputs and each other")
        reports, decoded = [], []
        for path in paths:
            report, rows = decode_artifact(path.read_bytes(), args.catalogs, product)
            report["artifact"] = str(path)
            reports.append(report)
            for row in rows:
                row["artifact"] = str(path)
            decoded.extend(rows)
        text = "".join(json.dumps(row, ensure_ascii=False) + "\n" for row in decoded)
        if args.output:
            args.output.write_text(text, encoding="utf-8")
        else:
            sys.stdout.write(text)
        if args.summary:
            args.summary.write_text(json.dumps(reports, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        return 0
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"offline telemetry parse failed: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
