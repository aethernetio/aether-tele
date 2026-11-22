#!/usr/bin/env python3
"""Pack accepted namespace block bytes with their frozen build descriptor."""
import argparse
import json
from pathlib import Path
import sys
from telemetry_artifact import pack


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--space", action="append", default=[], metavar="NAME=RAW_FILE")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        spaces = {}
        for item in args.space:
            name, path = item.split("=", 1)
            if name in spaces:
                raise ValueError(f"duplicate namespace {name}")
            spaces[name] = Path(path).read_bytes()
        encoded = pack(json.loads(args.build.read_text(encoding="utf-8")), spaces)
        if args.output.resolve() == args.build.resolve():
            raise ValueError("output must not overwrite the build descriptor")
        args.output.write_bytes(encoded)
        return 0
    except (OSError, ValueError, KeyError) as error:
        print(f"artifact packing failed: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
