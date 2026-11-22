#!/usr/bin/env python3
# Copyright 2026 Aethernet Inc. Licensed under the Apache License, Version 2.0.
"""Build explicit production catalogs and deterministic frequency-ranked C++ IDs.

Source roots come from the descriptor, never from a recursive examples/tests
search. The actual AeroAdmin Chat product is allowed its explicit examples path.
"""
from __future__ import annotations

import argparse
import copy
import fnmatch
import json
from pathlib import Path
import re
import subprocess
import sys

from telemetry_artifact import PRODUCTS, SHARED_SPACES, canonical, profile_counts, sha256
from decode_raw import CODECS, _encode_packed

LEVELS = {"INFO": 1, "WARNING": 2, "ERROR": 4, "DEBUG": 8}
DEFAULT_MACROS = ["AE_TELE", "AETHER_TELE", "APPTRAVERSE_TELE", "PRODUCT_TELE", "AEROCHAT_TELE"]
SOURCE_EXTENSIONS = {".h", ".hpp", ".cc", ".cpp", ".cxx"}


def write_if_changed(path: Path, data: bytes) -> None:
    if not path.is_file() or path.read_bytes() != data:
        path.write_bytes(data)


def mask_cpp(source: str) -> str:
    """Preserve positions/newlines while hiding comments, strings and directives."""
    chars, cursor = list(source), 0
    def erase(start, end):
        for index in range(start, end):
            if chars[index] != "\n":
                chars[index] = " "
    while cursor < len(source):
        start = cursor
        if source.startswith("//", cursor):
            end = source.find("\n", cursor)
            cursor = len(source) if end < 0 else end
        elif source.startswith("/*", cursor):
            end = source.find("*/", cursor + 2)
            if end < 0:
                raise ValueError("unterminated C++ comment")
            cursor = end + 2
        elif source.startswith('R"', cursor):
            opening = source.find("(", cursor + 2)
            if opening < 0:
                raise ValueError("unterminated C++ raw string")
            delimiter = source[cursor + 2:opening]
            close = ")" + delimiter + '"'
            end = source.find(close, opening + 1)
            if end < 0:
                raise ValueError("unterminated C++ raw string")
            cursor = end + len(close)
        elif (source[cursor] == "'" and cursor > 0 and cursor + 1 < len(source) and
              source[cursor - 1].isalnum() and source[cursor + 1].isalnum()):
            cursor += 1  # C++ digit separator, not the start of a character literal.
            continue
        elif source[cursor] in ('"', "'"):
            quote = source[cursor]
            cursor += 1
            while cursor < len(source):
                if source[cursor] == "\\":
                    cursor += 2
                elif source[cursor] == quote:
                    cursor += 1
                    break
                else:
                    cursor += 1
        else:
            cursor += 1
            continue
        erase(start, cursor)
    cursor, directive = 0, False
    for line in source.splitlines(keepends=True):
        if re.match(r"\s*#", line) or directive:
            erase(cursor, cursor + len(line))
            directive = line.rstrip().endswith("\\")
        cursor += len(line)
    return "".join(chars)


def scan(source: str, file: str, repository: str, macros=None) -> list[dict]:
    masked = mask_cpp(source)
    names = "|".join(re.escape(name) for name in [*(macros or DEFAULT_MACROS), "BW_LOG"])
    pattern = re.compile(r"\b(?P<macro>(?:" + names + r")D?_(?P<level>DEBUG|INFO|WARNING|ERROR)(?:_TO)?|AE_LOG_(?:MACRO|COUNT)_TAGGED)\s*\(")
    sites = []
    for match in pattern.finditer(masked):
        cursor, depth, arg_start, args = match.end(), 1, match.end(), []
        while cursor < len(masked) and depth:
            char = masked[cursor]
            if char in "([{":
                depth += 1
            elif char in ")]}":
                depth -= 1
            if (char == "," and depth == 1) or depth == 0:
                args.append(source[arg_start:cursor].strip())
                arg_start = cursor + 1
            cursor += 1
        if depth:
            raise ValueError(f"unterminated telemetry call in {file}")
        line = source.count("\n", 0, match.start()) + 1
        last_line = source.count("\n", 0, cursor) + 1
        macro = match["macro"]
        tagged = "TELED_" not in macro and not macro.startswith("BW_LOG_")
        if macro.endswith("_TO"):
            args = args[1:]
        tag = args[0] if tagged and args else "kLog"
        description = args[1] if tagged and len(args) > 1 else args[0] if not tagged and args else ""
        site_id = f"{repository}:{file}:{line}:{macro}"
        sites.append(dict(site_id=site_id, file=file, line=line, last_line=last_line,
                          macro=macro, declared_tag=tag, text=description,
                          level=LEVELS[match["level"]] if match["level"] else 8,
                          count_only=macro == "AE_LOG_COUNT_TAGGED"))
    for previous, current in zip(sites, sites[1:]):
        if previous["last_line"] >= current["line"]:
            raise ValueError(f"telemetry callsites must occupy separate lines: {file}:{current['line']}")
    return sites


def sources(root: Path, spec: dict) -> list[Path]:
    includes = spec.get("include")
    if not isinstance(includes, list) or not includes:
        raise ValueError("every namespace requires explicit source includes")
    found = set()
    excluded = spec.get("exclude", ["**/tests/**", "**/test/**", "**/build*/**"])
    for entry in includes:
        candidate = root / entry
        if candidate.is_dir():
            selected = candidate.rglob("*")
        elif candidate.is_file():
            selected = [candidate]
        else:
            selected = root.glob(entry)
        for path in selected:
            if path.suffix not in SOURCE_EXTENSIONS or not path.is_file():
                continue
            relative = path.resolve().relative_to(root.resolve()).as_posix()
            if any(part in {"tests", "test", "examples"} or part.startswith("build")
                   for part in Path(relative).parts[:-1]) and not spec.get("allow_examples", False):
                continue
            if any(fnmatch.fnmatch(relative, pattern) or fnmatch.fnmatch("/" + relative, pattern)
                   for pattern in excluded):
                continue
            found.add(path.resolve())
    if not found:
        raise ValueError(f"no production sources selected for {spec.get('name')}")
    return sorted(found)


def revision(root: Path, selected: list[Path]) -> dict:
    commit = subprocess.check_output(["git", "-C", str(root), "rev-parse", "HEAD"], text=True).strip()
    status = subprocess.check_output(["git", "-C", str(root), "status", "--porcelain", "--untracked-files=normal"])
    tree = bytearray(subprocess.check_output(["git", "-C", str(root), "diff", "--binary", "HEAD"],
                                             stderr=subprocess.PIPE))
    untracked = subprocess.check_output(["git", "-C", str(root), "ls-files", "--others", "--exclude-standard", "-z"])
    for relative in sorted(part for part in untracked.split(b"\0") if part):
        path = root / relative.decode("utf-8")
        if path.is_file():
            tree.extend(relative + b"\0" + path.read_bytes() + b"\0")
    for path in sorted(set(selected)):
        tree.extend(path.relative_to(root).as_posix().encode() + b"\0" + path.read_bytes() + b"\0")
    return dict(commit=commit, dirty=bool(status.strip()), tree_sha256=sha256(bytes(tree)))


def rank_sites(sites: list[dict], counts: dict | None = None) -> list[dict]:
    counts = {} if counts is None else counts
    unknown = set(counts) - {site["site_id"] for site in sites}
    if unknown:
        raise ValueError(f"frequency profile refers to unknown callsites: {sorted(unknown)[:3]}")
    for value in counts.values():
        if type(value) is not int or value < 0:
            raise ValueError("frequency counts must be nonnegative integers")
    if len(sites) > 65535:
        raise ValueError("namespace exceeds tag range")
    baseline = sorted(sites, key=lambda site: (site["file"], site["line"], site["macro"]))
    ranked = sorted(sites, key=lambda site: (-counts.get(site["site_id"], 0), site["file"], site["line"], site["macro"]))
    base, thresholds, _ = CODECS["tag"]
    def cost(ordered):
        return sum(counts.get(site["site_id"], 0) * len(_encode_packed(index, base, thresholds))
                   for index, site in enumerate(ordered, 1))
    # Reassigning IDs changes saved-state compatibility. Only pay that cost
    # when the observed log profile actually reduces encoded tag bytes.
    ordered = ranked if cost(ranked) < cost(baseline) else baseline
    return [dict(site, index=index, events=counts.get(site["site_id"], 0))
            for index, site in enumerate(ordered, 1)]


def semantic_schema(name: str, schema: dict, tags: list[dict]) -> str:
    # Commit IDs, paths, line numbers, profiling counts and declaration order do
    # not change how saved bytes are interpreted. Numeric IDs and their meaning
    # do, including the parameter representation selected by this backend.
    wire = {key: value for key, value in schema.items() if key not in {"schema_source", "instructions"}}
    identities = [dict(index=tag["index"], module=tag["module"], level=tag["level"],
                       declared_tag=tag["declared_tag_expression"], text=tag["text"],
                       parameter_schema="utf8_formatted_message") for tag in sorted(tags, key=lambda tag: tag["index"])]
    return sha256(canonical(dict(space=name, wire=wire, tags=identities)))


def module_tags(files: list[Path]) -> dict[str, int]:
    modules, tags = {}, {"kLog": 0xfffffffe}
    declarations = []
    for path in files:
        source = mask_cpp(path.read_text(encoding="utf-8-sig"))
        for match in re.finditer(r'\bAE_TELE_MODULE\s*\(\s*(\w+)\s*,\s*(0x[0-9a-fA-F]+|\d+)\s*,', source):
            number = int(match[2], 0) if match[2].startswith("0x") else int(match[2])
            if match[1] in modules and modules[match[1]] != number:
                raise ValueError(f"ambiguous telemetry module {match[1]}")
            modules[match[1]] = number
        declarations.extend(re.findall(r'\bAE_TAG\s*\(\s*(\w+)\s*,\s*(\w+)\s*\)', source))
    for tag, module in declarations:
        if module in modules:
            if tag in tags and tags[tag] != modules[module]:
                raise ValueError(f"ambiguous telemetry tag module {tag}")
            tags[tag] = modules[module]
    return tags


def _header(manifest: dict, all_sites: list[dict]) -> str:
    entries = []
    for site in sorted(all_sites, key=lambda site: (site["space"], site["file"], site["line"])):
        entries.append("  Site{" + ",".join([json.dumps(site["space"]), json.dumps(site["file"]),
                                              str(site["line"]), str(site["last_line"]), str(site["index"])]) + "},")
    catalogs = "\n".join("  Catalog{" + ",".join(json.dumps(space[key]) for key in ("name", "repository", "catalog_sha256", "schema_sha256")) + "},"
                          for space in manifest["namespaces"])
    return '''#pragma once
#include <array>
#include <cstdint>
#include <string_view>
namespace ae::telemetry_catalog {
struct Site { std::string_view space,file; std::uint32_t first,last,index; };
struct Catalog { std::string_view space,repository,digest,schema; };
inline constexpr std::array<Site,''' + str(len(entries)) + "> kSites{{\n" + "\n".join(entries) + '''
}};
inline constexpr std::array<Catalog,''' + str(len(manifest["namespaces"])) + "> kCatalogs{{\n" + catalogs + '''
}};
inline constexpr std::string_view kProduct = ''' + json.dumps(manifest["product"]) + ''';
inline constexpr std::string_view kBuildId = ''' + json.dumps(manifest["build"]["build_id"]) + ''';
inline constexpr std::string_view kBuildJson = ''' + json.dumps(canonical(manifest["build"]).decode(), ensure_ascii=True) + ''';
inline constexpr std::string_view kManifestJson = ''' + json.dumps(canonical(manifest).decode(), ensure_ascii=True) + ''';
inline bool FileMatches(std::string_view full, std::string_view relative) noexcept {
  if (full.size() < relative.size()) return false;
  auto suffix = full.substr(full.size() - relative.size());
  for (std::size_t i = 0; i != suffix.size(); ++i)
    if ((suffix[i] == '\\\\' ? '/' : suffix[i]) != relative[i]) return false;
  return full.size() == relative.size() || full[full.size()-relative.size()-1] == '/' ||
         full[full.size()-relative.size()-1] == '\\\\';
}
inline std::uint32_t ResolveIndex(std::string_view space, std::string_view file, std::uint32_t line) noexcept {
  for (auto const& site : kSites)
    if (site.space == space && line >= site.first && line <= site.last && FileMatches(file, site.file))
      return site.index;
  return 0;
}
inline std::string_view CatalogId(std::string_view space) noexcept {
  for (auto const& catalog : kCatalogs) if (catalog.space == space) return catalog.digest;
  return {};
}
inline std::string_view SchemaId(std::string_view space) noexcept {
  for (auto const& catalog : kCatalogs) if (catalog.space == space) return catalog.schema;
  return {};
}
} // namespace ae::telemetry_catalog
'''


def build(descriptor_path: Path, output: Path, profile_path: Path | None = None,
          wire_map: Path | None = None, repository_roots: dict[str, Path] | None = None) -> dict:
    descriptor = json.loads(descriptor_path.read_text(encoding="utf-8"))
    product = descriptor["product"]
    if product not in PRODUCTS | {"sdk"}:
        raise ValueError("unknown production product")
    profile = json.loads(profile_path.read_text()) if profile_path else {}
    if profile and (profile.get("profile_version") != 1 or profile.get("wire_layout") != "thread_blocks"):
        raise ValueError("unsupported frequency profile")
    schema = json.loads((wire_map or Path(__file__).with_name("thread_blocks_wire.json")).read_text())
    roots = {name: (descriptor_path.parent / spec["root"]).resolve()
             for name, spec in descriptor["repositories"].items()}
    roots.update({name: path.resolve() for name, path in (repository_roots or {}).items()})
    selected, scanned = {name: [] for name in roots}, []
    names = set()
    for spec in descriptor["namespaces"]:
        if spec["name"] in names:
            raise ValueError("duplicate namespace")
        allowed = {"aether"} if product == "sdk" else SHARED_SPACES | {"product." + product}
        if spec["name"] not in allowed:
            raise ValueError("unknown or foreign-product namespace")
        names.add(spec["name"])
        root = roots[spec["repository"]]
        files = sources(root, spec)
        selected[spec["repository"]].extend(files)
        tag_modules = module_tags(files)
        sites = []
        macros = spec.get("macros")
        if spec["name"] == "aether":
            macros = list(dict.fromkeys([*(macros or DEFAULT_MACROS), "AE_TELE"]))
        for path in files:
            sites.extend(scan(path.read_text(encoding="utf-8-sig"), path.relative_to(root).as_posix(),
                              spec["repository"], macros))
        for site in sites:
            site["module"] = spec.get("module", tag_modules.get(site["declared_tag"].split("::")[-1]))
        if spec.get("legacy_tag_header"):
            declarations = (root / spec["legacy_tag_header"]).read_text()
            legacy = {match[1]: (int(match[2]), match[3]) for match in re.finditer(
                r'inline\s+constexpr\s+tele::Tag\s+(\w+)\s*\{\s*(\d+)\s*,\s*kModule\s*,\s*"([^"]+)"', declarations)}
            for site in sites:
                symbol = site["declared_tag"].split("::")[-1]
                if symbol not in legacy:
                    raise ValueError(f"unknown declared legacy tag {symbol}")
                site.update(legacy_index=legacy[symbol][0], runtime_tag_name=legacy[symbol][1],
                            site_id=f"{spec['repository']}:tag:{symbol}")
        scanned.append((spec, sites))
    revisions = {name: revision(root, selected[name]) for name, root in roots.items()}
    ranked = []
    for spec, sites in scanned:
        sites = rank_sites(sites, profile_counts(profile, spec["name"], revisions[spec["repository"]]["commit"]))
        if spec.get("preserve_indices"):
            for site in sites:
                site["index"] = site["legacy_index"]
            sites.sort(key=lambda site: site["index"])
            if len({site["index"] for site in sites}) != len(sites):
                raise ValueError("legacy tags must identify unique callsites")
        ranked.append((spec, sites))
    scanned = ranked
    build_info = dict(repositories=revisions, configuration=descriptor.get("configuration", {}),
                      profile_sha256=sha256(canonical(profile)), wire_layout="thread_blocks")
    build_info["build_id"] = sha256(canonical(dict(product=product, build=build_info,
                                                  sites=[sites for _, sites in scanned])))
    manifest = dict(product=product, build=build_info, namespaces=[])
    output.mkdir(parents=True, exist_ok=True)
    catalogs_dir = output / "catalogs"
    catalogs_dir.mkdir(exist_ok=True)
    all_sites = []
    for spec, sites in scanned:
        mapping = copy.deepcopy(schema)
        namespace_build = sha256(canonical(dict(name=spec["name"], source=revisions[spec["repository"]],
                                                sites=sites, module=spec.get("module"), schema=schema)))
        mapping.update(space=spec["name"], application_build=namespace_build,
                       source_revision=revisions[spec["repository"]], payload_encoding="utf8",
                       observed_callsites=[], callsite_capacity_exhausted=False)
        mapping.update(catalog_id=namespace_build, sites=sites,
                       indices_preserved=bool(spec.get("preserve_indices")))
        # Source identity is explicit. Module numbers are only supplied when the
        # descriptor knows them; null is preferable to inventing runtime labels.
        mapping["tags"] = [dict(index=site["index"], name=site.get("runtime_tag_name", site["site_id"]), text=site["text"],
                                file=site["file"], line=site["line"], module=site["module"],
                                level=site["level"], site_id=site["site_id"],
                                declared_tag_expression=site["declared_tag"],
                                macro=site["macro"], last_line=site["last_line"], count_only=site["count_only"])
                           for site in sites]
        mapping["schema_sha256"] = semantic_schema(spec["name"], schema, mapping["tags"])
        encoded = canonical(mapping)
        digest = sha256(encoded)
        write_if_changed(catalogs_dir / (digest + ".json"), encoded)
        manifest["namespaces"].append(dict(name=spec["name"], repository=spec["repository"],
                                            catalog_sha256=digest, schema_sha256=mapping["schema_sha256"]))
        all_sites.extend(dict(site, space=spec["name"]) for site in sites)
    write_if_changed(output / "build.json", canonical(manifest))
    write_if_changed(output / "telemetry_catalog.h", _header(manifest, all_sites).encode("utf-8"))
    write_if_changed(output / "sites.json", canonical(dict(product=product, build=build_info, sites=all_sites)))
    def cmake_quote(path):
        return str(path).replace("\\", "/").replace('"', '\\"').replace(";", "\\;")
    dependencies = [descriptor_path, Path(__file__).resolve(), Path(__file__).with_name("thread_blocks_wire.json"),
                    Path(__file__).with_name("telemetry_artifact.py"), Path(__file__).with_name("decode_raw.py"),
                    Path(__file__).with_name("analyze_raw.py")]
    dependencies.extend(path for values in selected.values() for path in values)
    for root in roots.values():
        for git_name in ("HEAD", "index"):
            git_path = subprocess.check_output(["git", "-C", str(root), "rev-parse", "--git-path", git_name], text=True).strip()
            candidate = Path(git_path)
            if not candidate.is_absolute():
                candidate = root / candidate
            if candidate.is_file():
                dependencies.append(candidate.resolve())
        reference = subprocess.run(["git", "-C", str(root), "symbolic-ref", "-q", "HEAD"], capture_output=True, text=True)
        if reference.returncode == 0:
            git_path = subprocess.check_output(["git", "-C", str(root), "rev-parse", "--git-path", reference.stdout.strip()], text=True).strip()
            candidate = Path(git_path)
            if not candidate.is_absolute():
                candidate = root / candidate
            if candidate.is_file():
                dependencies.append(candidate.resolve())
    if profile_path:
        dependencies.append(profile_path.resolve())
    write_if_changed(output / "catalog.inputs.cmake", (
        "set(AE_TELEMETRY_CATALOG_INPUTS\n" +
        "".join('  "' + cmake_quote(path) + '"\n' for path in sorted(set(dependencies))) + ")\n").encode())
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--descriptor", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--profile", type=Path)
    parser.add_argument("--wire-map", type=Path)
    parser.add_argument("--repository", action="append", default=[], metavar="NAME=PATH",
                        help="explicit dependency revision root, without adding a namespace")
    args = parser.parse_args()
    try:
        repositories = {}
        for entry in args.repository:
            name, separator, path = entry.partition("=")
            if not separator or not name or not path or name in repositories:
                raise ValueError("repository roots must be unique NAME=PATH values")
            repositories[name] = Path(path)
        manifest = build(args.descriptor.resolve(), args.output_dir.resolve(), args.profile, args.wire_map, repositories)
        print(manifest["build"]["build_id"])
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        print(f"catalog generation failed: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
