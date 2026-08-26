# Space telemetry standalone

Independent CMake project that uses `aether-tele` as an external library.
It does not depend on `aether-client-cpp`.

Two spaces, `demo::network` and `demo::application`, share no `namespace_id`.
Each has its own tag table, checksum, clock, storage, streams, codecs, log
buffer, metrics, and sink. The same numeric tag index may exist in both.

## Architecture

Hot path writes into a per-stream fixed buffer (`Stream` + `TeleStorage`).
There is no global lock per event. Metrics are stored per stream and merged
when a blob is serialized. Names, files, levels, and modules are host-only
(`AE_TELE_SPACE_HOST`) and never appear in the blob or the embedded runtime.

`space/console.h` is a cold/debug printer. Do not include it in embedded
translation units.

## Public API

* One X-macro table of direct indices per space (`TELE_INDEX_TABLE_*`).
* `AE_SPACE_TELE_INFO(stream, tag, payload...)` — the stream handle is explicit.
* `NET_TELE_*` / `APP_TELE_*` wrappers. No `TELE_SINK`, no `__COUNTER__`.
* RAII `Event`: constructor writes tag/payload/delta; destructor writes duration
  only (`noexcept`). Payload is not serialized again in the destructor.
* Disabled macros (`GetTeleConfig` all false, or `AE_TELE_SPACE_COMPILE_OUT`)
  do not construct `Event` and do not read the clock.

## Wire format (`ATS1`, version 2)

Little-endian packed integers from `ae-numeric` (`TieredInt` 1/2/4/8 bytes;
there is no 3-byte tier).

| Field | Size |
| --- | --- |
| magic | `ATS1` |
| schema checksum | `uint32` LE |
| wire version | 1 byte (`2`) |
| flags | 1 byte (bit0 = library version present) |
| sequence | packed |
| base_unix | `uint32` LE seconds (overflow 2106-02-07 06:28:15 UTC) |
| stream_section_size | packed |
| metrics_section_size | packed |
| library_version | optional packed length + bytes (`LIBRARY_VERSION`) |
| streams | count, then per stream: marker, log size, log bytes |
| metrics | count, then index/count/sum/min/max |

Log record: packed index, optional delta, optional packed payload, optional
duration. Stream marker is written once in the stream section, not per event.
There is no namespace, name, module, severity, file, line, function, or OS
thread id on the wire.

`uint32_t` Unix `base_time` wraps after 2106-02-07; that wrap is tested.

## Time

Deltas are per-stream relative to the previous event on that stream. The
decoder reconstructs each stream timeline and merges by `rel_time`, then
marker.

### `network`

* Policy: `ExponentialCodec` over existing `ae::Exponential`.
* Exponential runtime: `FixedPoint<uint32_t, 4e9>` ticks.
* Wire: `PackedU64` exponential codes.
* `kDeltaMinMagnitude = 1` tick.
* `kDeltaBoundaryMagnitude = 4e9` ticks (~4000 s at 1 µs/tick).
* `kDeltaBoundaryCode = PackedU64::kMaxBoundaryCode`.
* Zero is code 0. Values above the boundary saturate.
* Mapping is approximate (library exponential). Exact **codes** round-trip.
  Reconstructed ticks for 10 µs are typically nearby (about 16 in this
  configuration). Relative error on values ≤ 1e6 ticks was measured below
  0.75 with the 4e9 FixedPoint work type. Boundary codes saturate near
  `uint32` max.

The default ae-numeric work-type picker only reaches Max=65536, so network
uses `WideExponentialMathPolicy` with a 4e9 work type. That is a math-policy
parameter, not a second packed-integer implementation.

### `application`

* Policy: packed fixed-point ticks (`PackedIntCodec<PackedU64>`), exact.
* `kTicksPerSecond = 1000`.

Restart starts a new blob with a new `base_unix`. Deltas do not continue
across processes.

## Concurrency

Streams `Main`, `Poller`, `Network`, `Storage`, `Worker0`. Each thread writes
through its own `Stream` handle into a preallocated slot. ThreadSanitizer:
`-DAE_TELE_SPACE_TSAN=ON` on GCC/Clang.

## Restart

`--phase=write` commits batches plus a truncated tail. `--phase=resume` in a
**new process** reads committed batches, ignores the tail, and writes a blob
with a new `base_unix` and continued sequence.

## Decoder

Host decoder checks magic, wire version, and schema checksum, then refuses
on mismatch. Source locations are recovered from the schema table plus unique
macro call sites in the matching source revision — not from the blob.

```
python tools/source_locations.py --space network --root . --print-lines
python tools/source_locations.py --space network --root . --exe ./space_standalone --blob out/network.blob
```

A tag used from more than one call site is an error.

## Reindex

The reindex script is **not** part of the ordinary build and does not generate
production headers.

```
python tools/reindex.py --space network --root . --report out/network.json
python tools/reindex_revision.py --root . --lib-root ../.. --report out/network.json --build-dir <revA-build>
```

Revision B must not decode a revision A blob, and vice versa.

## Build and run

From `aether-tele`:

```
cmake -S . -B build-space -DAE_TELE_BUILD_TESTS=ON
cmake --build build-space --config Release --target test-tele space_standalone space_heap_probe space_stack_probe space_fp_baseline space_fp_compile_out space_fp_one_space space_fp_two_spaces
ctest --test-dir build-space --build-config Release --output-on-failure
```

Phases:

```
space_standalone --phase=unit
space_standalone --phase=write --dir=out
space_standalone --phase=resume --dir=out
space_standalone --phase=decode --space network out/network.blob
space_standalone --phase=decode --space network --text out/network.blob
space_standalone --phase=threads
space_standalone --phase=stress
```

See `FOOTPRINT.md` for measured code/stack/heap. ESP-IDF is optional; see
`esp32/README.md`.
