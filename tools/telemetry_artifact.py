#!/usr/bin/env python3
# Copyright 2026 Aethernet Inc. Licensed under the Apache License, Version 2.0.
"""Portable keeper artifacts and strict, entirely offline catalog resolution."""
from __future__ import annotations

import copy
import hashlib
import json
from pathlib import Path
import re
import struct

from analyze_raw import analyze
from decode_raw import DecodeError, load_map

MAGIC = b"AETLOG01"
MAX_MANIFEST = 1024 * 1024
PRODUCTS = {"apptraverse-app", "aeroadmin-chat"}
SHARED_SPACES = {"objects", "aether", "apptraverse"}


def sha256(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def canonical(value) -> bytes:
    return json.dumps(value, ensure_ascii=False, sort_keys=True,
                      separators=(",", ":")).encode("utf-8")


def profile_counts(profile: dict, space: str, commit: str) -> dict:
    versions = profile.get("counts_by_commit", {}).get(space)
    if versions is not None:
        if not isinstance(versions, dict) or commit not in versions:
            raise DecodeError(f"frequency profile has no {space} source revision {commit}")
        return versions[commit]
    return profile.get("counts", {}).get(space, {})


def _digest(value, field):
    if not isinstance(value, str) or not re.fullmatch(r"[0-9a-f]{64}", value):
        raise DecodeError(f"invalid {field}")


def _commit(value, field):
    if not isinstance(value, str) or not re.fullmatch(r"[0-9a-f]{40}|[0-9a-f]{64}", value):
        raise DecodeError(f"invalid {field}")


def validate_manifest(manifest: dict, payload: bytes) -> None:
    if (type(manifest.get("artifact_version")) is not int or
            manifest["artifact_version"] != 1 or manifest.get("wire_layout") != "thread_blocks"):
        raise DecodeError("unsupported telemetry artifact version/layout")
    product = manifest.get("product")
    if product not in PRODUCTS:
        raise DecodeError("unknown product")
    build = manifest.get("build")
    if not isinstance(build, dict):
        raise DecodeError("build descriptor is required")
    _digest(build.get("build_id"), "build ID")
    repositories = build.get("repositories")
    if not isinstance(repositories, dict) or not repositories:
        raise DecodeError("source revision manifest is required")
    for name, revision in repositories.items():
        if not isinstance(name, str) or not isinstance(revision, dict):
            raise DecodeError("invalid repository descriptor")
        _commit(revision.get("commit"), "source commit")
        if type(revision.get("dirty")) is not bool:
            raise DecodeError("source dirty state must be explicit")
        _digest(revision.get("tree_sha256"), "source tree digest")
    spaces = manifest.get("namespaces")
    if not isinstance(spaces, list) or not spaces or len(spaces) > 4:
        raise DecodeError("artifact must contain 1..4 namespaces")
    seen, cursor = set(), 0
    for space in spaces:
        if not isinstance(space, dict):
            raise DecodeError("namespace descriptor is not an object")
        name = space.get("name")
        if name not in SHARED_SPACES | {"product." + product} or name in seen:
            raise DecodeError("duplicate, unknown or foreign-product namespace")
        seen.add(name)
        if space.get("repository") not in repositories:
            raise DecodeError("namespace source repository is missing")
        _digest(space.get("catalog_sha256"), "catalog digest")
        if "schema_sha256" in space:
            _digest(space["schema_sha256"], "schema digest")
        _digest(space.get("raw_sha256"), "raw digest")
        offset, length = space.get("offset"), space.get("length")
        if (type(offset) is not int or type(length) is not int or
                offset != cursor or length < 0 or length > len(payload) - cursor):
            raise DecodeError("namespace payload ranges must be complete and non-overlapping")
        if sha256(payload[offset:offset + length]) != space["raw_sha256"]:
            raise DecodeError("namespace payload hash mismatch")
        cursor += length
    if cursor != len(payload):
        raise DecodeError("artifact has trailing/unreferenced payload bytes")


def encode(manifest: dict, payload: bytes) -> bytes:
    validate_manifest(manifest, payload)
    encoded = canonical(manifest)
    if len(encoded) > MAX_MANIFEST:
        raise DecodeError("artifact manifest is too large")
    return MAGIC + struct.pack("<I", len(encoded)) + encoded + payload


def read(data: bytes) -> tuple[dict, bytes]:
    if len(data) < 12 or data[:8] != MAGIC:
        raise DecodeError("artifact magic/header missing")
    length, = struct.unpack_from("<I", data, 8)
    if not length or length > MAX_MANIFEST or length > len(data) - 12:
        raise DecodeError("artifact manifest length is invalid")
    def unique_pairs(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise DecodeError(f"duplicate JSON member {key}")
            result[key] = value
        return result
    manifest = json.loads(data[12:12 + length].decode("utf-8"), object_pairs_hook=unique_pairs)
    if not isinstance(manifest, dict):
        raise DecodeError("artifact manifest is not an object")
    payload = data[12 + length:]
    validate_manifest(manifest, payload)
    return manifest, payload


def pack(build: dict, raw_spaces: dict[str, bytes]) -> bytes:
    manifest = dict(artifact_version=1, wire_layout="thread_blocks",
                    product=build["product"], build=copy.deepcopy(build["build"]), namespaces=[])
    payload = bytearray()
    known = {space["name"]: space for space in build["namespaces"]}
    for name, raw in sorted(raw_spaces.items()):
        if name not in known:
            raise DecodeError(f"namespace {name} is absent from the build descriptor")
        space = copy.deepcopy(known[name])
        space.update(offset=len(payload), length=len(raw), raw_sha256=sha256(raw))
        manifest["namespaces"].append(space)
        payload.extend(raw)
    return encode(manifest, bytes(payload))


def decode_artifact(data: bytes, catalogs: Path, product: str | None = None) -> tuple[dict, list[dict]]:
    manifest, payload = read(data)
    if product is not None and manifest["product"] != product:
        raise DecodeError(f"artifact belongs to {manifest['product']}, expected {product}")
    reports, rows = {}, []
    for space in manifest["namespaces"]:
        digest = space["catalog_sha256"]
        path = catalogs / (digest + ".json")
        if not path.is_file():
            raise DecodeError(f"missing exact catalog {digest} for {space['name']}; source commit "
                              f"{manifest['build']['repositories'][space['repository']]['commit']}")
        if sha256(path.read_bytes()) != digest:
            raise DecodeError(f"catalog file hash mismatch: {path.name}")
        mapping = load_map(path)
        if mapping.get("space") != space["name"]:
            raise DecodeError("catalog namespace mismatch")
        if "schema_sha256" in space and mapping.get("schema_sha256") != space["schema_sha256"]:
            raise DecodeError("catalog semantic schema mismatch")
        expected = manifest["build"]["repositories"][space["repository"]]
        if mapping.get("source_revision") != expected:
            raise DecodeError("catalog source revision/dirty tree does not match the artifact")
        raw = payload[space["offset"]:space["offset"] + space["length"]]
        report, decoded = analyze(raw, mapping)
        reports[space["name"]] = report
        by_index = {tag["index"]: tag for tag in mapping["tags"]}
        for row in decoded:
            row.update(product=manifest["product"], build_id=manifest["build"]["build_id"],
                       source_commit=expected["commit"], source_dirty=expected["dirty"],
                       catalog_sha256=digest,
                       site_id=by_index[row["index"]].get("site_id"))
            if mapping.get("payload_encoding") == "utf8" and "payload_hex" in row:
                try:
                    row["message"] = bytes.fromhex(row["payload_hex"]).decode("utf-8")
                except UnicodeError:
                    row["payload_decode_error"] = "invalid UTF-8 for the selected catalog"
            rows.append(row)
    return dict(product=manifest["product"], build=manifest["build"],
                artifact_bytes=len(data), namespaces=reports), rows
