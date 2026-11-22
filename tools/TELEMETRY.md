# Offline product telemetry

There are two production products: `apptraverse-app` and `aeroadmin-chat`.
Each has independent `objects`, `aether`, `apptraverse` and `product.<name>`
namespaces. IDs are local to a namespace. Generic examples and tests are excluded
from production catalogs. AeroAdmin Chat is explicitly selected as a product;
its location under the AeroAdmin `examples/` directory does not make its source
an example catalog.

## Build artifacts

The product's checked-in `telemetry/catalog.json` names repository roots and
source includes. Configure the target with `TelemetryCatalog.cmake`, or run:

```sh
python tools/build_catalog.py --descriptor /source/product/telemetry/catalog.json --output-dir /artifacts/build
```

Archive the entire output directory with the binary. It contains `build.json`,
`telemetry_catalog.h`, source callsites in `sites.json` and immutable exact maps
under `catalogs/<sha256>.json`. Keep the catalog directories from all released
builds in a local archive. The parser never fetches Git or uses a map from the
current checkout as a substitute.

`build.json` records repository commit IDs, explicit dirty flags, source tree
digests, the supplied build configuration, frozen frequency profile digest and
the exact map hashes. The CMake helper also records the resolved raw telemetry
and numeric dependency repositories without creating additional namespaces.
A dirty binary is never presented as an unmodified commit.
The source descriptor's `configuration` object can contain additional build
options required by the product release process. Reproducing a release also
requires archiving its normal compiler and dependency build inputs.

The generated C++ header exposes `ae::telemetry_catalog::ResolveIndex(space,
file,line)`, `CatalogId(space)`, `SchemaId(space)`, `kCatalogs`, `kProduct`, `kBuildId`, `kBuildJson`
and `kManifestJson`. The SDK wrapper generator preserves its existing
`ae::aether_tele_internal` interface and uses the same namespace maps. Therefore
the shared Aether catalog has the same hash in both products when its source
revision, schema and profile match.

`schema_sha256` is separate from the exact provenance/catalog hash. It describes
numeric tag meanings, their level/module/parameter representation and wire
codecs. Commit changes, source paths, line shifts and unchanged frequency ranks
do not change that fingerprint. Reassigning a numeric tag to a different event
does. Saved state can therefore survive a compatible build while every keeper
artifact still names its exact original catalog.

## Keeper envelope

The keeper receives and persists these exact bytes:

```text
"AETLOG01" | uint32_le(manifest byte length) | UTF-8 JSON manifest | raw payload
```

The manifest has `artifact_version: 1`, `wire_layout: "thread_blocks"`,
`product`, `build` and `namespaces`. Each namespace contains `name`,
`repository`, `catalog_sha256`, `offset`, `length` and `raw_sha256`. Offsets are
relative to the start of the raw payload. Ranges are contiguous, ordered and
cover the payload exactly. The sender's immutable batch ID and source UID belong
to the transport protocol and keeper storage path; they do not change these
artifact bytes. The maximum manifest size is 1 MiB.

For raw captures taken by a product scenario, create the same envelope locally:

```sh
python tools/pack_artifact.py --build /artifacts/build/build.json --space objects=objects.raw --space apptraverse=apptraverse.raw --space product.apptraverse-app=product.raw --output capture.aetlog
```

Only include namespaces actually present in that capture. An empty/offline
Aether namespace does not imply that SDK telemetry was exercised.

## Separate offline parsers

```sh
python tools/decode_apptraverse_app.py capture.aetlog --catalogs /artifacts/catalogs --output records.jsonl --summary bytes.json
python tools/decode_aeroadmin_chat.py capture.aetlog --catalogs /artifacts/catalogs --output records.jsonl --summary bytes.json
```

Each entry point rejects the other product. Both use the same decoder and select
each namespace map from the artifact's exact SHA256. Missing or modified maps,
unknown numeric schemas, source revision mismatches, invalid ranges, unknown
tags and truncated bytes cause an error. Files from different compatible builds
can be passed together; each uses its own archived maps. Parsing needs no
network access.

The byte report separates log records, parameter contents, parameter length
prefixes, time anchors, thread block framing and metric batches. The principal
`average_log_bytes_excluding_payload` subtracts only parameter contents.
`average_with_log_block_headers_excluding_payload` additionally amortizes time
anchors and log block framing. Keeper JSON and transport overhead are separate
from raw log record costs.

## Freeze observed frequencies for the next build

```sh
python tools/profile_artifacts.py baseline.aetlog --catalogs /artifacts/catalogs --output frequency.json
python tools/build_catalog.py --descriptor /source/product/telemetry/catalog.json --profile frequency.json --output-dir /artifacts/ranked
python tools/report_frequency.py --profile frequency.json --baseline /artifacts/build/build.json --ranked /artifacts/ranked/build.json --output frequency-report.json
```

The profile counts emitted log records, not metric packets or source macro
occurrences. Inputs are recorded by artifact hash and build ID. The generator
keeps counts separated by namespace and source commit as well as reporting
aggregate totals. One frozen file can therefore serve products pinned to
different AppTraverse revisions without mixing their source callsites.
The generator
sorts by descending count with a stable file/line/macro tie break, then freezes
the mapping in generated artifacts. It never changes a live process's mapping.
It retains the default mapping when reassigning IDs would save zero observed
tag bytes, so a frequency-only reorder does not force a pointless state reset.
Tags 1–239 occupy one byte, 240–4095 two bytes, and the remaining supported tags
four bytes. Rebuild and repeat the same product scenario to verify semantic
parity and measure all actual bytes. The comparison report predicts only the
tag-byte change using the observed baseline counts; decoder summaries measure
the actual captures.

Object library tags retain their existing explicit 1–21 IDs. They already fit
in one byte, so reordering them offers no byte saving. Preserve that choice in
the descriptor with `preserve_indices: true` and `legacy_tag_header`.

Run the offline integration tests with:

```sh
python tools/test_offline_pipeline.py
```
