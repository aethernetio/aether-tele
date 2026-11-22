#!/usr/bin/env python3
# Copyright 2026 Aethernet Inc. Licensed under the Apache License, Version 2.0.
"""Offline product isolation, immutable catalogs and measured tag remapping."""
import copy
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

from build_catalog import build, scan
from decode_raw import CODECS, DecodeError, _encode_packed
from profile_artifacts import profile
from report_frequency import compare, tag_bytes
from telemetry_artifact import canonical, decode_artifact, encode, pack, profile_counts, read, sha256


def packed(codec, value):
    base, thresholds, _ = CODECS[codec]
    return _encode_packed(value, base, thresholds)


def raw_records(indices):
    body = b"".join(packed("tag", index) for index in indices)
    return b"\x00\x01\x00\x01" + packed("blob", len(body)) + packed("metric", len(indices)) + body


class PipelineTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="offline-telemetry-test-")
        self.root = Path(self.temporary.name)
        self.catalogs = self.root / "catalogs"
        self.catalogs.mkdir()

    def tearDown(self):
        self.temporary.cleanup()

    def fixture(self, product="apptraverse-app", commit="a" * 40, label="item"):
        revision = dict(commit=commit, dirty=False, tree_sha256="b" * 64)
        mapping = json.loads(Path(__file__).with_name("thread_blocks_wire.json").read_text())
        name = "product." + product
        mapping.update(space=name, application_build=commit, source_revision=revision,
                       tags=[dict(index=7, name=label, text=label, module=1, level=1,
                                  site_id="product:src/main.cpp:1:PRODUCT_TELE_INFO")],
                       observed_callsites=[], callsite_capacity_exhausted=False)
        encoded = canonical(mapping)
        digest = sha256(encoded)
        (self.catalogs / (digest + ".json")).write_bytes(encoded)
        descriptor = dict(product=product, build=dict(build_id="c" * 64,
                          repositories={"product": revision}),
                          namespaces=[dict(name=name, repository="product", catalog_sha256=digest)])
        return pack(descriptor, {name: raw_records([7, 7])}), descriptor, digest

    def test_separate_product_frontends_reject_foreign_product(self):
        for product, script in [("apptraverse-app", "decode_apptraverse_app.py"),
                                ("aeroadmin-chat", "decode_aeroadmin_chat.py")]:
            data, _, _ = self.fixture(product)
            capture = self.root / (product + ".aetlog")
            capture.write_bytes(data)
            output = self.root / (product + ".jsonl")
            result = subprocess.run([os.sys.executable, str(Path(__file__).with_name(script)),
                                     str(capture), "--catalogs", str(self.catalogs),
                                     "--output", str(output)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            rows = [json.loads(line) for line in output.read_text().splitlines()]
            self.assertEqual([row["product"] for row in rows], [product, product])
            other = "aeroadmin-chat" if product == "apptraverse-app" else "apptraverse-app"
            with self.assertRaises(DecodeError):
                decode_artifact(data, self.catalogs, other)

    def test_exact_catalog_and_commit_are_required(self):
        data, _, digest = self.fixture()
        path = self.catalogs / (digest + ".json")
        original = path.read_bytes()
        path.unlink()
        with self.assertRaisesRegex(DecodeError, "missing exact catalog"):
            decode_artifact(data, self.catalogs)
        path.write_bytes(original + b" ")
        with self.assertRaisesRegex(DecodeError, "hash mismatch"):
            decode_artifact(data, self.catalogs)
        path.write_bytes(original)
        manifest, payload = read(data)
        manifest["build"]["repositories"]["product"]["commit"] = "d" * 40
        with self.assertRaisesRegex(DecodeError, "source revision"):
            decode_artifact(encode(manifest, payload), self.catalogs)
        manifest["build"]["repositories"]["product"]["commit"] = "a" * 40
        manifest["build"]["repositories"]["product"]["dirty"] = True
        with self.assertRaisesRegex(DecodeError, "dirty tree"):
            decode_artifact(encode(manifest, payload), self.catalogs)

    def test_identical_indices_in_namespaces_are_independent(self):
        _, descriptor, digest = self.fixture()
        mapping = json.loads((self.catalogs / (digest + ".json")).read_bytes())
        mapping["space"] = "objects"
        mapping["tags"][0].update(name="object event", site_id="objects:tag:kSaved")
        encoded = canonical(mapping)
        objects_digest = sha256(encoded)
        (self.catalogs / (objects_digest + ".json")).write_bytes(encoded)
        descriptor["build"]["repositories"]["objects"] = mapping["source_revision"]
        descriptor["namespaces"].append(dict(name="objects", repository="objects", catalog_sha256=objects_digest))
        data = pack(descriptor, {"objects": raw_records([7]), "product.apptraverse-app": raw_records([7])})
        report, rows = decode_artifact(data, self.catalogs)
        self.assertEqual(len(report["namespaces"]), 2)
        self.assertEqual([row["space"] for row in rows], ["objects", "product.apptraverse-app"])
        self.assertEqual([row["site_id"] for row in rows], ["objects:tag:kSaved", "product:src/main.cpp:1:PRODUCT_TELE_INFO"])

    def test_unknown_numeric_codec_is_rejected_even_with_valid_hash(self):
        _, descriptor, digest = self.fixture()
        mapping = json.loads((self.catalogs / (digest + ".json")).read_bytes())
        mapping["numeric_codecs"]["tag"]["thresholds"][0] = 200
        encoded = canonical(mapping)
        digest = sha256(encoded)
        (self.catalogs / (digest + ".json")).write_bytes(encoded)
        descriptor["namespaces"][0]["catalog_sha256"] = digest
        with self.assertRaisesRegex(DecodeError, "numeric codecs"):
            decode_artifact(pack(descriptor, {"product.apptraverse-app": raw_records([7])}), self.catalogs)

    def test_versions_and_namespace_overlap_are_not_guessed(self):
        data, _, _ = self.fixture()
        manifest, payload = read(data)
        for version in (0, 2, "1", True):
            changed = copy.deepcopy(manifest)
            changed["artifact_version"] = version
            with self.assertRaises(DecodeError):
                encode(changed, payload)
        for cut in (0, 7, 8, 11, 12, len(data) - 1):
            with self.assertRaises((DecodeError, ValueError)):
                read(data[:cut])
        with self.assertRaises(DecodeError):
            read(data + b"\0")
        changed = copy.deepcopy(manifest)
        changed["namespaces"].append(changed["namespaces"][0].copy())
        with self.assertRaises(DecodeError):
            encode(changed, payload)
        changed = copy.deepcopy(manifest)
        changed["namespaces"] = [7]
        with self.assertRaises(DecodeError):
            encode(changed, payload)
        changed = copy.deepcopy(manifest)
        changed["namespaces"][0]["name"] = "product.aeroadmin-chat"
        with self.assertRaises(DecodeError):
            encode(changed, payload)

    def test_mixed_builds_resolve_their_own_map(self):
        first, _, _ = self.fixture(commit="a" * 40, label="old")
        second, _, _ = self.fixture(commit="d" * 40, label="new")
        rows = decode_artifact(first, self.catalogs)[1] + decode_artifact(second, self.catalogs)[1]
        self.assertEqual([row["description"] for row in rows], ["old", "old", "new", "new"])
        self.assertEqual({row["source_commit"] for row in rows}, {"a" * 40, "d" * 40})
        paths = [self.root / "old.aetlog", self.root / "new.aetlog"]
        paths[0].write_bytes(first)
        paths[1].write_bytes(second)
        frozen = profile(paths, self.catalogs)
        name = "product.apptraverse-app"
        self.assertEqual(sum(profile_counts(frozen, name, "a" * 40).values()), 2)
        self.assertEqual(sum(profile_counts(frozen, name, "d" * 40).values()), 2)
        self.assertEqual(sum(frozen["counts"][name].values()), 4)
        with self.assertRaisesRegex(DecodeError, "source revision"):
            profile_counts(frozen, name, "e" * 40)

    def test_comments_strings_directives_and_generic_examples_are_excluded(self):
        source = ('// PRODUCT_TELE_INFO(kTag, "fake");\n'
                  '#define FAKE PRODUCT_TELE_INFO(kTag,"fake")\n'
                  'auto raw=R"x(PRODUCT_TELE_INFO(kTag,"fake"))x";\n'
                  'PRODUCT_TELE_INFO(kTag,\n "real {}");\n')
        sites = scan(source, "src/main.cpp", "product")
        self.assertEqual(len(sites), 1)
        self.assertEqual(sites[0]["line"], 4)
        self.assertEqual(sites[0]["last_line"], 5)
        separated = scan("auto n = 1'000;\nPRODUCT_TELE_INFO(kTag, \"visible\");\n",
                         "src/numbers.cpp", "product")
        self.assertEqual(len(separated), 1)
        wrappers = scan('BW_LOG_DEBUG("Buffer capacity {}", size);\nAE_TELED_DEBUG("Session {}", session);\n',
                        "aether/write_action/buffer_write.h", "aether")
        self.assertEqual([site["declared_tag"] for site in wrappers], ["kLog", "kLog"])
        self.assertEqual([site["text"] for site in wrappers], ['"Buffer capacity {}"', '"Session {}"'])

    def repository(self):
        root = self.root / "source"
        (root / "src").mkdir(parents=True)
        source = root / "src" / "main.cpp"
        source.write_text("\n".join('PRODUCT_TELE_INFO(kTag, "event %d");' % number
                                    for number in range(240)) + "\n")
        (root / "examples").mkdir()
        (root / "examples" / "demo.cpp").write_text('PRODUCT_TELE_INFO(kTag,"excluded");\n')
        (root / "tests").mkdir()
        (root / "tests" / "test.cpp").write_text('PRODUCT_TELE_INFO(kTag,"excluded");\n')
        subprocess.run(["git", "init", "-q", str(root)], check=True)
        subprocess.run(["git", "-C", str(root), "add", "."], check=True)
        subprocess.run(["git", "-C", str(root), "-c", "user.name=Telemetry Test",
                        "-c", "user.email=telemetry@example.invalid", "-c", "commit.gpgsign=false",
                        "commit", "-qm", "fixture"], check=True)
        descriptor = self.root / "descriptor.json"
        descriptor.write_bytes(canonical(dict(product="apptraverse-app",
            repositories={"product": {"root": str(root)}},
            namespaces=[dict(name="product.apptraverse-app", repository="product", include=["."])])))
        return root, descriptor

    def test_frequency_remap_has_measured_byte_gain_and_semantic_parity(self):
        _, descriptor = self.repository()
        baseline_dir, ranked_dir = self.root / "baseline", self.root / "ranked"
        baseline = build(descriptor, baseline_dir)
        baseline_sites = json.loads((baseline_dir / "sites.json").read_text())["sites"]
        self.assertEqual(len(baseline_sites), 240)
        self.assertTrue(all(site["file"].startswith("src/") for site in baseline_sites))
        capture = self.root / "baseline.aetlog"
        capture.write_bytes(pack(baseline, {"product.apptraverse-app": raw_records([240] * 100 + [1] * 2)}))
        observed = profile([capture], baseline_dir / "catalogs")
        observed["counts"]["product.apptraverse-app"]["product:older.cpp:1:PRODUCT_TELE_INFO"] = 7
        observed["counts_by_commit"]["product.apptraverse-app"]["e" * 40] = {"product:older.cpp:1:PRODUCT_TELE_INFO": 7}
        frozen = self.root / "frequency.json"
        frozen.write_bytes(canonical(observed))
        ranked = build(descriptor, ranked_dir, frozen)
        self.assertNotEqual(baseline["namespaces"][0]["schema_sha256"], ranked["namespaces"][0]["schema_sha256"])
        ranked_sites = json.loads((ranked_dir / "sites.json").read_text())["sites"]
        new_by_site = {site["site_id"]: site["index"] for site in ranked_sites}
        old_by_index = {site["index"]: site for site in baseline_sites}
        new_indices = [new_by_site[old_by_index[index]["site_id"]] for index in [240] * 100 + [1] * 2]
        changed = pack(ranked, {"product.apptraverse-app": raw_records(new_indices)})
        before, old_rows = decode_artifact(capture.read_bytes(), baseline_dir / "catalogs")
        after, new_rows = decode_artifact(changed, ranked_dir / "catalogs")
        self.assertEqual([row["site_id"] for row in old_rows], [row["site_id"] for row in new_rows])
        old_size = before["namespaces"]["product.apptraverse-app"]["log_record_bytes"]
        new_size = after["namespaces"]["product.apptraverse-app"]["log_record_bytes"]
        self.assertEqual(old_size - new_size, 100)
        comparison = compare(observed, baseline_dir / "build.json", ranked_dir / "build.json")
        self.assertEqual(comparison["log_records"], 102)
        self.assertEqual(comparison["saved_tag_bytes"], 100)
        repeat = build(descriptor, self.root / "repeat", frozen)
        self.assertEqual(ranked, repeat)
        header_mtime = (ranked_dir / "telemetry_catalog.h").stat().st_mtime_ns
        build(descriptor, ranked_dir, frozen)
        self.assertEqual((ranked_dir / "telemetry_catalog.h").stat().st_mtime_ns, header_mtime)
        compiler = shutil.which("g++")
        if compiler:
            result = subprocess.run([compiler, "-std=c++20", "-x", "c++", "-fsyntax-only",
                                     str(ranked_dir / "telemetry_catalog.h")], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_dirty_source_is_never_identified_as_clean_commit(self):
        root, descriptor = self.repository()
        (root / "src" / "main.cpp").write_text('PRODUCT_TELE_INFO(kTag,"changed");\n')
        manifest = build(descriptor, self.root / "dirty")
        self.assertTrue(manifest["build"]["repositories"]["product"]["dirty"])

    def test_preserved_object_indices_are_not_reordered_by_frequency(self):
        root, descriptor_path = self.repository()
        (root / "src" / "main.cpp").write_text('AE_LOG_MACRO_TAGGED(kFirst, "first");\nAE_LOG_MACRO_TAGGED(kLast, "last");\n')
        (root / "src" / "telemetry.h").write_text('inline constexpr tele::Tag kFirst{1, kModule, "first"};\ninline constexpr tele::Tag kLast{21, kModule, "last"};\n')
        descriptor = json.loads(descriptor_path.read_text())
        descriptor["namespaces"] = [dict(name="objects", repository="product", include=["src/"],
                                        legacy_tag_header="src/telemetry.h", preserve_indices=True)]
        descriptor_path.write_bytes(canonical(descriptor))
        baseline_dir = self.root / "baseline"
        baseline = build(descriptor_path, baseline_dir)
        source = root / "src" / "main.cpp"
        source.write_text(source.read_text().replace('AE_LOG_MACRO_TAGGED(kLast', 'AE_LOG_COUNT_TAGGED(kLast'))
        profile_path = self.root / "profile.json"
        observed = dict(profile_version=1, wire_layout="thread_blocks", counts={"objects": {"product:tag:kLast": 100}})
        profile_path.write_bytes(canonical(observed))
        ranked_dir = self.root / "ranked"
        ranked = build(descriptor_path, ranked_dir, profile_path)
        comparison = compare(observed, baseline_dir / "build.json", ranked_dir / "build.json")
        self.assertEqual(comparison["saved_tag_bytes"], 0)
        self.assertTrue(comparison["namespaces"]["objects"]["indices_preserved"])
        self.assertEqual(baseline["namespaces"][0]["schema_sha256"], ranked["namespaces"][0]["schema_sha256"])
        sites = json.loads((ranked_dir / "sites.json").read_text())["sites"]
        self.assertTrue(next(site for site in sites if site["index"] == 21)["count_only"])
        self.assertEqual([tag_bytes(index) for index in (1, 239, 240, 4095, 4096, 65535)], [1, 1, 2, 2, 4, 4])

    def test_schema_ignores_provenance_but_tracks_encoded_meaning(self):
        root, descriptor = self.repository()
        initial = build(descriptor, self.root / "first")
        source = root / "src" / "main.cpp"
        source.write_text("\n\n" + source.read_text())
        source.rename(root / "src" / "renamed.cpp")
        relocated = build(descriptor, self.root / "relocated")
        self.assertNotEqual(initial["namespaces"][0]["catalog_sha256"], relocated["namespaces"][0]["catalog_sha256"])
        self.assertEqual(initial["namespaces"][0]["schema_sha256"], relocated["namespaces"][0]["schema_sha256"])
        first_site = json.loads((self.root / "relocated" / "sites.json").read_text())["sites"][9]
        profile_path = self.root / "profile.json"
        profile_path.write_bytes(canonical(dict(profile_version=1, wire_layout="thread_blocks",
            counts={"product.apptraverse-app": {first_site["site_id"]: 100}})))
        counted = build(descriptor, self.root / "counted", profile_path)
        self.assertNotEqual(relocated["namespaces"][0]["catalog_sha256"], counted["namespaces"][0]["catalog_sha256"])
        self.assertEqual(relocated["namespaces"][0]["schema_sha256"], counted["namespaces"][0]["schema_sha256"])
        source = root / "src" / "renamed.cpp"
        source.write_text(source.read_text().replace('"event 0"', '"different event"'))
        incompatible = build(descriptor, self.root / "incompatible")
        self.assertNotEqual(initial["namespaces"][0]["schema_sha256"], incompatible["namespaces"][0]["schema_sha256"])

    def test_dependency_only_revision_is_embedded_without_namespace(self):
        _, descriptor = self.repository()
        dependency = self.root / "dependency"
        dependency.mkdir()
        (dependency / "codec.h").write_text("// pinned codec\n")
        subprocess.run(["git", "init", "-q", str(dependency)], check=True)
        subprocess.run(["git", "-C", str(dependency), "add", "."], check=True)
        subprocess.run(["git", "-C", str(dependency), "-c", "user.name=Telemetry Test",
                        "-c", "user.email=telemetry@example.invalid", "-c", "commit.gpgsign=false",
                        "commit", "-qm", "dependency"], check=True)
        manifest = build(descriptor, self.root / "with-dependency", repository_roots={"numeric": dependency})
        self.assertIn("numeric", manifest["build"]["repositories"])
        self.assertEqual(len(manifest["namespaces"]), 1)

    def test_schema_tables_equal_native_export_when_available(self):
        reference = Path(__file__).resolve().parents[1] / "build-msvc/tests/Release/test-raw-tele.map.json"
        if not reference.is_file():
            self.skipTest("native ExportMap artifact unavailable")
        native = json.loads(reference.read_text())
        schema = json.loads(Path(__file__).with_name("thread_blocks_wire.json").read_text())
        for key in ("numeric_codecs", "fields", "time_values_us", "duration_values_us", "layout"):
            self.assertEqual(schema[key], native[key])


if __name__ == "__main__":
    unittest.main()
