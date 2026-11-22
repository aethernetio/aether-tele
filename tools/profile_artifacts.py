#!/usr/bin/env python3
"""Freeze real emitted callsite frequencies for deterministic next-build IDs."""
import argparse
from collections import Counter, defaultdict
import json
from pathlib import Path
import sys
from telemetry_artifact import canonical, decode_artifact, read, sha256


def profile(files: list[Path], catalogs: Path) -> dict:
    counts = defaultdict(Counter)
    versions = defaultdict(dict)
    inputs, products = [], set()
    paths = [path.resolve() for path in files]
    if len(paths) != len(set(paths)):
        raise ValueError("artifact supplied twice")
    for path in paths:
        data = path.read_bytes()
        report, rows = decode_artifact(data, catalogs)
        manifest, _ = read(data)
        for space in manifest["namespaces"]:
            commit = manifest["build"]["repositories"][space["repository"]]["commit"]
            versions[space["name"]].setdefault(commit, Counter())
        products.add(report["product"])
        inputs.append(dict(sha256=sha256(data), build_id=report["build"]["build_id"]))
        for row in rows:
            if row["kind"] != "log":
                continue
            if not row.get("site_id"):
                raise ValueError("frequency optimization requires source-identified catalogs")
            counts[row["space"]][row["site_id"]] += 1
            versions[row["space"]][row["source_commit"]][row["site_id"]] += 1
    return dict(profile_version=1, wire_layout="thread_blocks", products=sorted(products),
                inputs=sorted(inputs, key=lambda item: (item["build_id"], item["sha256"])),
                counts={space: dict(sorted(values.items())) for space, values in sorted(counts.items())},
                counts_by_commit={space: {commit: dict(sorted(values.items())) for commit, values in sorted(commits.items())}
                                  for space, commits in sorted(versions.items())})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("files", type=Path, nargs="+")
    parser.add_argument("--catalogs", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        result = profile(args.files, args.catalogs)
        if args.output.resolve() in {path.resolve() for path in args.files}:
            raise ValueError("output must not overwrite an input artifact")
        args.output.write_bytes(canonical(result))
        print(f"profiled {sum(sum(values.values()) for values in result['counts'].values())} logs")
        return 0
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"telemetry profiling failed: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
