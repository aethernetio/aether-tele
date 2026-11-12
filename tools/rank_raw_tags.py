#!/usr/bin/env python3
"""Propose next-build indices from recorded log frequency; never edit sources."""
import argparse
from collections import Counter
import json
import os
from pathlib import Path
import sys
from decode_raw import decode, load_map, merge_records


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--map', type=Path, required=True, dest='map_path')
    parser.add_argument('files', nargs='+', type=Path)
    args = parser.parse_args()
    mapping = load_map(args.map_path)
    tags = {tag['index']: tag for tag in mapping['tags']}
    frequency = Counter()
    records = []
    seen_paths = set()
    for path in args.files:
        canonical = os.path.normcase(str(path.resolve()))
        if canonical in seen_paths:
            raise ValueError(f'capture file supplied twice: {path}')
        seen_paths.add(canonical)
        decoded, valid_bytes, truncated = decode(path.read_bytes(), mapping)
        if truncated:
            print(f'{path}: interrupted tail ignored; valid_bytes={valid_bytes}', file=sys.stderr)
        records.extend(decoded)
    for record in merge_records(records):
        if record['index'] not in tags:
            raise ValueError('record index absent from selected build map')
        if record['kind'] == 'log':
            frequency[record['index']] += 1
    ranked = sorted(tags, key=lambda index: (-frequency[index], index))
    result = [{'old_index': index, 'new_index': rank, 'events': frequency[index],
               'name': tags[index]['name'], 'text': tags[index]['text'],
               'file': tags[index]['file'], 'line': tags[index]['line']}
              for rank, index in enumerate(ranked)]
    print(json.dumps({'source_build': mapping['application_build'],
                      'proposed_indices': result}, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f'raw tele ranking failed: {error}', file=sys.stderr)
        raise SystemExit(2)
