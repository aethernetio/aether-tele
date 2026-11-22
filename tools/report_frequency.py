#!/usr/bin/env python3
# Copyright 2026 Aethernet Inc. Licensed under the Apache License, Version 2.0.
"""Compare frozen observed frequencies against baseline and next-build maps.

The savings are for tag bytes in log records. Parameter bytes, metric packets,
block headers and keeper envelopes are deliberately accounted elsewhere by the
artifact decoder's actual binary byte report.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys

from decode_raw import CODECS, DecodeError, _encode_packed, load_map
from telemetry_artifact import canonical, profile_counts, sha256


def tag_bytes(index: int) -> int:
    base, thresholds, maximum = CODECS["tag"]
    if type(index) is not int or not 1 <= index <= maximum:
        raise DecodeError("invalid catalog tag index")
    return len(_encode_packed(index, base, thresholds))


def catalog_sites(build_path: Path) -> tuple[dict, dict]:
    manifest = json.loads(build_path.read_text(encoding="utf-8"))
    spaces = {}
    for space in manifest["namespaces"]:
        path = build_path.parent / "catalogs" / (space["catalog_sha256"] + ".json")
        if sha256(path.read_bytes()) != space["catalog_sha256"]:
            raise DecodeError("catalog file hash mismatch")
        mapping = load_map(path)
        if mapping["space"] != space["name"]:
            raise DecodeError("catalog namespace mismatch")
        sites = {tag["site_id"]: tag for tag in mapping["tags"]}
        if len(sites) != len(mapping["tags"]):
            raise DecodeError("catalog has duplicate source identities")
        spaces[space["name"]] = sites
    return manifest, spaces


def compare(profile: dict, baseline: Path, ranked: Path) -> dict:
    if profile.get("profile_version") != 1 or profile.get("wire_layout") != "thread_blocks":
        raise DecodeError("unsupported frequency profile")
    before, old_spaces = catalog_sites(baseline)
    after, new_spaces = catalog_sites(ranked)
    if before["product"] != after["product"] or old_spaces.keys() != new_spaces.keys():
        raise DecodeError("frequency comparison requires the same product and namespaces")
    reports = {}
    repositories = {space["name"]: space["repository"] for space in before["namespaces"]}
    for name, old_sites in old_spaces.items():
        new_sites = new_spaces[name]
        if old_sites.keys() != new_sites.keys():
            raise DecodeError(f"source callsites changed in {name}; rerun the baseline scenario")
        counts = profile_counts(profile, name, before["build"]["repositories"][repositories[name]]["commit"])
        if set(counts) - old_sites.keys():
            raise DecodeError(f"profile contains unknown {name} callsites")
        entries, old_bytes, new_bytes, log_count = [], 0, 0, 0
        tier_before, tier_after = {1: 0, 2: 0, 4: 0}, {1: 0, 2: 0, 4: 0}
        for site_id, count in counts.items():
            if type(count) is not int or count < 0:
                raise DecodeError("invalid frequency count")
            old, new = old_sites[site_id], new_sites[site_id]
            old_width, new_width = tag_bytes(old["index"]), tag_bytes(new["index"])
            old_bytes += old_width * count
            new_bytes += new_width * count
            log_count += count
            tier_before[old_width] += count
            tier_after[new_width] += count
            entries.append(dict(site_id=site_id, events=count, old_index=old["index"],
                                new_index=new["index"], old_tag_bytes=old_width,
                                new_tag_bytes=new_width, saved_bytes=count * (old_width - new_width)))
        reports[name] = dict(log_records=log_count, catalog_sites=len(old_sites),
                             old_tag_bytes=old_bytes, new_tag_bytes=new_bytes,
                             saved_tag_bytes=old_bytes - new_bytes,
                             old_tag_width_events=tier_before, new_tag_width_events=tier_after,
                             most_frequent=sorted(entries, key=lambda site: (-site["events"], site["site_id"])),
                             indices_preserved=all(old_sites[site]["index"] == new_sites[site]["index"] for site in old_sites))
    logs = sum(report["log_records"] for report in reports.values())
    old_bytes = sum(report["old_tag_bytes"] for report in reports.values())
    new_bytes = sum(report["new_tag_bytes"] for report in reports.values())
    return dict(product=before["product"], baseline_build_id=before["build"]["build_id"],
                ranked_build_id=after["build"]["build_id"], log_records=logs,
                old_tag_bytes=old_bytes, new_tag_bytes=new_bytes, saved_tag_bytes=old_bytes - new_bytes,
                average_old_tag_bytes=old_bytes / logs if logs else None,
                average_new_tag_bytes=new_bytes / logs if logs else None, namespaces=reports)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", type=Path, required=True)
    parser.add_argument("--baseline", type=Path, required=True, help="baseline generated build.json")
    parser.add_argument("--ranked", type=Path, required=True, help="next-build generated build.json")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    try:
        if args.output and args.output.resolve() in {args.profile.resolve(), args.baseline.resolve(), args.ranked.resolve()}:
            raise ValueError("output must not overwrite an input")
        result = compare(json.loads(args.profile.read_text()), args.baseline, args.ranked)
        if args.output:
            args.output.write_bytes(canonical(result))
        else:
            print(json.dumps(result, indent=2))
        return 0
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"frequency comparison failed: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
