# aether-tele

C++20 telemetry. The existing `TeleSink` / `ITrap` API and the serialization
of `StatisticsTrap` remain available. The new `aether-tele/raw.h` layer records
plain data; it does not change Aether's persistent `TeleStatistics` object or
the cloud protocol.

The public headers require C++20 and C++ exceptions. The exported CMake targets
enable exception support for consumers, including projects whose toolchain
otherwise disables it. The raw library does not require RTTI.

## Raw telemetry

Each `Space<Identity, compiled_options, Clock>` has an independent runtime mask.
Each writer owns a `Stream<Space>`:
its fixed RAM arena, metrics, sequence and callbacks. The stream must not be
copied, shared between writers, or destroyed while a scope still references it.
Different streams may share a space; `SetRuntime` publishes one atomic options
snapshot to concurrent writers. Use distinct stream numbers for live writers.
Read stream counters/metrics only after that writer is quiescent.
`Drain()` and `DrainMetrics()` are also owner-writer operations: a separate
timer/IO thread must not drain that same live stream. Join/quiesce its writer
first, or request that the owner performs the drain. `Space::SetRuntime()` is
the explicitly supported concurrent control operation.

```cpp
#include "aether-tele/raw.h"
namespace capture {
using namespace ae::tele::raw;
struct Identity {};
using Telemetry = Space<Identity>;
AE_RAW_TAG(Telemetry, kReceived, 1, 2, 0, "packet received");
AE_RAW_TAG(Telemetry, kWork, 20, 2, 0, "process packet");
AE_RAW_TAG(Telemetry, kStart, 2, 2, 0, "start");
AE_RAW_TAG(Telemetry, kStop, 3, 2, 0, "stop");
AE_RAW_TAG(Telemetry, kPause, 4, 2, 0, "pause");
AE_RAW_TAG(Telemetry, kResume, 5, 2, 0, "resume");
constexpr Tag<Telemetry> tags[]{kReceived, kWork, kStart, kStop, kPause, kResume};
static_assert(UniqueIndices(tags));

// Return true only after accepting the whole span. Filesystem/flash IO is user
// code; context can point to a file, flash journal, or a synchronous queue.
bool Save(void* context, std::span<std::uint8_t const> bytes);
void Run(void* destination) {
  Telemetry space;
  Stream<Telemetry, 4096> stream(space, 0, Save, destination);
  AE_RAW_LOG(stream, kStart);
  AE_RAW_LOG(stream, kReceived);
  {
    AE_RAW_SCOPE(stream, kWork);
    // Process the packet. Each tag has one logging callsite.
  }
  space.SetRuntime({true, false, true, true, kTime | kDuration});
  AE_RAW_LOG(stream, kPause);
  auto enabled = space.Runtime();
  space.SetRuntime({false, false, false, false, 0});
  // Application may sleep here, after draining the pause marker if needed.
  space.SetRuntime(enabled);
  AE_RAW_LOG(stream, kResume);
  AE_RAW_LOG(stream, kStop);
  // Handle false: the pending bytes have not been acknowledged.
  bool logs_saved = stream.Drain();
  bool metrics_saved = stream.DrainMetrics();
  (void)logs_saved; (void)metrics_saved;
}
}
```

Use separate prefix wrappers for consumers, for example
`#define APP_LOG(S,T) AE_RAW_LOG(S,T)`. Tags from another space are compile-time
errors, even if their index and configuration match. Log/payload macros
evaluate the stream once; tags must be static declarations. Payload spans are
copied before the logging call returns. Scope objects cannot be moved/copied.

`Options` independently controls logs, console, counts, timings, and the fields
time/duration/payload. Runtime settings are
intersected with the compile-time ceiling: they cannot enable compiled-out
features. Disabled payload expressions are not evaluated. Console output
requires an explicitly supplied callback, and does not fill the persistence
buffer when logs are disabled. A console callback can call `Decode` and
`PrintRecord` from the host-only `raw/decode.h`; the printer respects omitted
fields. Fully disabled scopes do not even require clock methods.

Profiling is **flat and inclusive**. A scope counts its entry and updates
count/samples/sum/min/max on its owning stream. Average is `sum / samples`.
There is no parent tree and no subtraction of nested durations. A scope emits
one entry with a reserved duration byte. Its destructor patches that byte only
while the original block remains mutable; it emits no completion record.
An instant log uses the unknown duration code when that field is enabled.
Nested scopes and instant logs with the same field mask share a block. A scope snapshots runtime settings at entry; changes affect
new events/scopes; existing scopes retain their entry snapshot. Durations
use a monotonic clock; default timestamps use wall-clock microseconds.

## Drain and restart

Before a whole record would overflow RAM, the stream synchronously calls its
drain callback on the writer thread. There are no internal locks around user
code. Before invoking the callback the block is frozen and every outstanding duration
ticket is invalidated, including on failure. Open scopes retain code 255 forever
in that drained block; their exact RAM statistics still update on exit.
Success releases the bytes. Failure/exception retains the exact bytes
for retry, and drops the new event once; `dropped()` reports lost records.
Previously failed metric batches are retried before later log IO, preserving
per-writer append order even when new events arrive after a failed drain.
An oversized record cannot modify or flush the pending data. Reentrant logging
or draining from a callback is rejected. There is no destructor IO: explicitly
drain while callback/context are still alive.

The callback span is borrowed only for the call: this initial API is
**synchronous**, not asynchronous ownership transfer. An asynchronous user queue
must copy the span into its own storage before acknowledging. Do not use blocking
storage callbacks in interrupts; route interrupt data to a suitable owner first.
The fixed console scratch arena is separate from pending log data. No allocator
is used by construction, event recording, metrics or drain themselves.
The entire incoming record is captured in scratch before invoking drain, so a
payload may safely refer to existing pending bytes and callback mutation cannot
alter the incoming payload. Payload bytes stay opaque. Static tag descriptions,
names, module and level live in the debug map; map text must be valid UTF-8.

The callback must implement all-or-retry acceptance. If a file write partially
succeeds, roll it back/truncate to the previous boundary before returning false;
blindly appending the same span again duplicates data. Implement the durability
barrier required by the destination (a stream flush alone is not a power-loss
guarantee). Different writer callbacks sharing a file must serialize complete
batches. Never interleave bytes from two callbacks.

Blocks concatenate directly in a file/flash region. Each contains one or more
records with a common field mask, writer ID and sequence base. After restart,
`Decode` returns rows only from complete validated blocks plus `valid_bytes`;
an interrupted final block reports `kTruncated` and contributes no rows.
Truncate that entire block **before appending**. Malformed data reports
`kMalformed` and must not be silently skipped. There is deliberately no checksum
and no guarantee of detecting arbitrary bit corruption.

Start, stop, pause and resume are ordinary application-defined tags, recorded
with `kTime` enabled. There are no reserved lifecycle indices or special
lifecycle record kinds. A missing stop marker does not prevent reading or
appending after an abrupt exit. Pause/resume boundaries are inferred from
their ordinary events; the application controls collection with `SetRuntime`.

Writer sequences may restart at zero after reopening. `Reconstruct` preserves
each writer's physical append order, including counter resets, then merges
available writer heads by timestamp. Supply capture files in append order.
Counter resets and clock regressions are diagnostics, not errors. Without
timestamps only per-writer append order is known. Cross-thread causality cannot
be inferred from equal/disabled timestamps. The returned views borrow the input
file bytes. Supply each persisted byte range exactly once: repeated counters
are legitimate after restarting, so replaying the same file twice cannot be
unambiguously distinguished from genuine data and is not deduplicated.
Only a structurally valid interrupted tail is repairable: already
visible invalid flags, kinds and lengths report `kMalformed`.

`DrainMetrics()` first drains logs, then writes incremental flat metric batches
even when logs are compiled/runtime-disabled. RAM `MetricFor()` totals are not
reset, but only not-yet-acknowledged contributions are sent. A repeated drain
without updates emits nothing. A failed batch keeps identical bytes and sequence
for retry; updates made after failure are queued separately until that retry is
acknowledged. `RestoreMetrics` sums accepted batches from all writers and across
restarts without reconstructing process boundaries or depending on stop markers.
Unacknowledged RAM data is naturally lost on abrupt termination.

To continue RAM statistics after restarting, initialize each new stream from its
persisted metric totals **before** recording, entering an active scope, or
draining. `Stream::RestoreMetrics(std::span<MetricTotal const>)` is an
allocation-free, one-shot startup operation: it validates all entries before
changing anything and returns false on duplicate indices, invalid min/max,
insufficient metric capacity, or an already started/initialized stream. A failed
initialization leaves the stream available for retry. Imported totals appear in
`MetricFor()`, including while collection is disabled; only subsequent updates
enter outgoing deltas. Draining immediately after restore writes no metric data.

## Native file drain

Host builds provide an optional `aether::tele_file` target. Link it and include
`aether-tele/raw/file.h` to use a ready synchronous callback:

```cpp
#include "aether-tele/raw.h"
#include "aether-tele/raw/file.h"

ae::tele::raw::FileDrain file;
if (!file.Open("capture.raw")) {
  // Inspect file.status(); do not start writers on failure.
  return;
}
capture::Telemetry space;
ae::tele::raw::Stream<capture::Telemetry, 4096> stream(
    space, 0, ae::tele::raw::FileDrain::Callback, &file);
ae::tele::raw::FileStatus startup;
if (!file.RestoreMetrics(stream, &startup)) {
  // Startup failed: inspect startup and do not start this writer.
  return;
}
AE_RAW_LOG(stream, capture::kStart);
if (!stream.Drain() || !stream.DrainMetrics()) {
  // Pending data remains in the stream. Inspect file.status() before retrying.
  return;
}
file.Close();
```

`FileDrain::RestoreMetrics(stream)` reads accepted metric batches from the open
file under its lock and loads only that stream ID's history. It validates the
entire file and checks capacity before initializing the stream. Read/capacity
failure does not partially import data or modify the file. Restore every stream
before starting its owner thread. The file mutex protects storage IO, not
concurrent access to the same Stream: its restore/drain/update/read operations
require one owner or an externally quiescent writer. Use the optional
`RestoreMetrics(stream, &outcome)` diagnostic when other streams can be draining;
the shared `status()` may already describe another operation after return.
Keep writer IDs stable across restarts and
unique among live writers. History from multiple writers remains separate in
their RAM totals; `RestoreMetrics(decoded.records)` still provides combined
totals for reporting. Restoring those combined totals into every writer would
multiply history in RAM and must be avoided.

Use the same tag index meanings when reopening a file. If a new build reassigns
indices, use a separate capture or explicitly remap the old totals before calling
the core stream restore API. The file contains no namespace or build identifier;
keep independent spaces/build mappings in their own files.

Only metric batches accepted by `DrainMetrics()` are recoverable statistics;
ordinary logs saved by `Drain()` are not replayed as metric updates. If a drain
accepts one tag then fails on another, restarting loads only the accepted tag.
Unsaved RAM changes are lost on process death. A live retry keeps its frozen
delta unchanged and writes newer changes separately, so restored history and
successful retries are never emitted twice. No stop marker is required.

One shared `FileDrain` serializes entire batches from multiple owner streams.
Keep it alive until all writers have finished their final drains and stopped
using the callback. The file is held exclusively for writing until `Close()`.
POSIX locks are advisory; other writers must cooperate. Do not unlink or replace
a file while its adapter is open. The adapter checks unexpected size changes,
but cannot detect external same-size overwrites.

An append validates the whole batch before writing, completes short writes, and
returns success only after `fsync` (POSIX) or `FlushFileBuffers` (Windows).
On write/sync failure it truncates to the previous boundary and synchronizes
that rollback before allowing a retry. If rollback or interrupted-tail repair
cannot be synchronized, the adapter enters permanent `kPoisoned` state and
refuses further appends, including after `Close()`. Reconcile the file externally
before creating another adapter. `Append(bytes, &outcome)` captures the call's
diagnostic under the lock; shared `status()` reports the latest operation.

`Open()` scans blocks with bounded memory, preserves all complete blocks, and
truncates only a structurally valid incomplete final block before appending.
`recovery()` reports the original/valid sizes and removed byte count. Set
`FileOptions::repair_incomplete_tail=false` to inspect without repair. Malformed
data is refused without truncation. Blocks exceeding `max_record_bytes`
(1,967,580 bytes by default) are refused, rather than treated as an interrupted write;
use a consistent limit when reopening a capture.

POSIX open also synchronizes the existing parent directory. Windows has no
portable equivalent in this adapter: file contents/length are flushed, but
durability of a newly created filename across power loss is outside this
guarantee (`recovery().directory_sync_supported` is false). Storage hardware
and filesystem guarantees still apply. A crash during append can leave complete
blocks before the callback acknowledges the batch; reopening preserves that
complete block prefix. Without transaction markers the raw format cannot provide
exactly-once acknowledgement across process death.

This component may allocate and block during filesystem IO. It is separate from
the allocation-free raw core and can be omitted with
`-DAE_TELE_BUILD_FILE_DRAIN=OFF` (the default for cross/embedded builds).
No file metadata, sessions, checksums or alternate record formats are added.

## Debug maps and index reassignment

Build with `AE_TELE_RAW_DEBUG=1`, include `raw/metadata.h`, and explicitly export
the complete tag table to a JSON file at application startup/shutdown:

```cpp
std::ofstream map("my-build.application.json");
ae::tele::raw::ExportMap<capture::Telemetry>(
    map, "application", "my-build", capture::tags);
```

The export includes declared indices, source file/line, text, and the full binary
layout, including field sizes/order. Each tag index identifies one logging
callsite in its space/build. Duplicate declared indices, conflicting tag metadata
and multiple observed callsites for one index are rejected before writing a map.
Put tag declarations at meaningful CPP checkpoints. Debug macros also register
**observed** callsites; these supplement the complete declaration table, not a
promise to discover unexecuted code. Export again after execution to include
observed sites. The decoder reports the unique observed `source` by index; if a
site was never observed it leaves `source` null and retains the tag's separate
`declaration`, rather than guessing a callsite. Debug registration is bounded and
allocation-free; exhaustion refuses map export. JSON export is a host operation
and may allocate.

Indices may change freely between application builds. Archive each build's map
with its raw files, separately per space/build. There is no on-wire build hash
or schema discriminator: the caller must select the matching map. A wrong map
with overlapping indices cannot be detected automatically. Indices are limited to 0..65535 and use **1, 2 or 4 bytes**. Assign frequent
tags indices 0..239, less frequent tags 240..4095, and the rest 4096..65535.
Rank a captured workload for a future build:

```text
python tools/rank_raw_tags.py --map my-build.application.json capture.raw
python tools/decode_raw.py --map my-build.application.json capture.raw
python tools/decode_raw.py --map my-build.application.json --metrics capture.raw
```

Ranking produces a proposal and never edits source or reinterprets old captures.
Payloads are opaque bytes; the map documents framing, and application-specific
payload interpretation belongs to the tag's documented schema.

Level, module, tag text, source file and source line are recovered by unique
index from the map and occupy no event bytes. Source locations and tag
declarations remain in the map. There is no scalar value
field or value macro. Removed field bits are invalid and are never reassigned:
captures containing removed fields and obsolete maps are refused. Use a new
capture after upgrading from the experimental layout with those fields.

## Compact binary layout

The wire codecs use the pinned `aethernet-numeric` revision
`9d79bd79188420533bab07c437d617aedfaf6b1a`. Existing public packed integer
aliases used by Aether's protocol are unchanged. Link `aether::tele_raw` for the
raw headers and numeric include requirements.

A block starts with its total size (`TieredInt<uint8_t,250,1500>`), control (u8), packed
stream ID, packed first sequence, and an absolute u64 microsecond time base if
time is present. Low control bits select logs (0) or metrics (1), field bits
are shifted left by two, and bit 7 selects exact wide u64 numbers. Other bits
are invalid. Writer sequence increments per row and is inferred within a block.
There is no padding, magic, format version, checksum or session field.

| Value | Numeric type | Wire bytes |
|---|---|---|
| Tag index | `TieredInt<uint8_t,239,4095>`; bound 65535 | 1 / 2 / 4 |
| Stream ID | `TieredInt<uint8_t,15,1023>`; bound 65535 | 1 / 2 / 4 |
| Sequence, exact metric sum/min/max | `TieredInt<uint16_t,32767,2000000000>` | 2 / 4 / 8 |
| Metric count/samples | `TieredInt<uint8_t,247,2047,16000000>` | 1 / 2 / 4 / 8 |
| Block size (including its size prefix) | `TieredInt<uint8_t,250,1500>` | 1 / 2 / 4 |
| Blob length | `TieredInt<uint8_t,250,1500>` | 1 / 2 / 4 |
| Time delta code | `TieredInt<uint8_t,250,511>` | 1 / 2 |
| Scope duration code | reserved byte | 1 |

Blob lengths 0..250 use one byte, 251..1500 use two, and 1501..1,967,580
use four. The maximum is about 1.88 MiB. The same ranges apply to total block
size. A blob must also leave room for its length, tag and the block header.
The size prefix includes itself. On growth the writer chooses the smallest
stable width; for example, a 249-byte body makes a 250-byte block, while a
250-byte body needs a two-byte prefix and makes a 252-byte block.

Every log row starts with its tag. Enabled fields then follow in order: time
code, duration byte, blob length and blob bytes. No size or kind is repeated per
row. A field-mask change, capacity boundary, clock regression, or a time gap over
60 seconds starts a new block. The new block anchors its first timestamp exactly.

Time uses `Exponential<FixedPoint<uint32_t,64.0>, TimeCodec, 1e-6,60.0,511>`.
Code 0 represents zero delta. The encoder chooses the greatest reconstructed
value no larger than the actual delta from the **previous reconstructed time**;
rounding errors therefore do not accumulate across many events. Deltas through
6535 microseconds fit one byte, larger deltas through 60 seconds use two.

Duration uses the same fixed-point runtime and exponential range with 253
numeric codes. Encoding selects the nearest decoded microsecond value. Its
maximum relative error for integer-microsecond input in this range is about
5.56%; endpoints are exact. Bounds are checked before quantization:

| Duration byte | Meaning |
|---:|---|
| 0 | below 1 microsecond; exact duration unspecified |
| 1 | 1 microsecond |
| 2..252 | approximate exponential duration |
| 253 | 60 seconds |
| 254 | above 60 seconds; exact duration unspecified |
| 255 | unknown/uninitialized, including a scope still open when drained |

Exact time/duration lookup tables are exported in each debug map. Python uses
those tables, including numeric's fixed-point rounding, instead of recreating
them with floating-point math.

For frequent tags the following are row sizes in **both RAM and files**. Add one
block header per group: at least 5 bytes without time, 13 with time; larger
size prefixes, stream IDs and sequences can increase those to 17/25 bytes.

| Enabled fields | Frequent tag row | Full tag/length range |
|---|---:|---:|
| tag only | 1 B | 1..4 B |
| tag + time | 2..3 B | 2..6 B |
| tag + duration | 2 B | 2..5 B |
| tag + time + duration | 3..4 B | 3..7 B |
| tag + time + duration + blob of N bytes | N+4..8 B | N+4..11 B |

The raw stream preallocates two arenas of `Capacity + 3` bytes (pending block and incoming
scratch). Three extra bytes per arena keep the body and duration slots at stable
RAM offsets when the prefix grows; these bytes are never drained. `Capacity`
limits the complete encoded block size. The stream also has a fixed metric
table and constant control state. Disabling fields
increases rows per arena; it does not resize the object. Duration tickets hold
only owner/generation/offset and never retain a buffer pointer across drain.

Metric blocks contain a tag and exact count, samples, sum, min, max values.
They start at 14 bytes per block; they never use approximate duration codes.
Values outside a configured packed range switch the whole metric block to
checked exact u64 fields, preserving the complete statistics API range.
Persisted data contains no pointers. This layout replaces the earlier
experimental fixed-width layout; start a new capture and retain its matching map.

## Existing Aether API

`TeleSink<ConfigProvider>`, `ITrap` ownership and the existing stored statistics
remain unchanged. `NamespacedTeleSink<Identity, ConfigProvider>` adds independent
instances for existing consumers sharing one configuration. Explicit
`AE_TELE_INFO_TO(SinkType, tag, ...)` (and DEBUG/WARNING/ERROR variants), plus
`AE_TELE_ENV_TO`, let library wrappers bind a qualified sink without mutating
global `TELE_SINK`. Existing default macros retain their signatures, caller
context, RAII lifetime and original eager evaluation of formatting arguments.
The new raw macros skip disabled payload expressions.

`SetTrap()` and `GetTrap()` publish/acquire owning snapshots safely between
threads. Each legacy event keeps one snapshot for its count, log and eventual
duration, even if the sink is rebound during the scope. The historical
`trap()` reference getter is retained for compatibility and requires trap
replacement to be quiescent; concurrent code must use `GetTrap()` instead.
Statistics mutation, environment updates and merging now synchronize both
participating traps; the reference getters and serialization still require a
quiescent trap. Public `ProxyTrap::first/second` pointers are historical mutable
fields: changing them directly while logging is unsupported. Publish a new
complete proxy through `SetTrap()` instead of mutating a live proxy's children.

The raw core depends on numeric and can be used without the legacy headers;
include host decoder/metadata only where needed.

## Raw front-end and object integration

Include `aether-tele/raw/sink.h` and link `aether::tele` to route the familiar
`AE_TELE_INFO_TO`, DEBUG/WARNING/ERROR and environment macros through
`RawTeleSink<Identity, ConfigProvider, Capacity, MetricCapacity>`. `Identity::kName`
selects an independent space. Aether and aether-objects bind separate identities
(`aether` and `objects`) to a shared backend, so tag indices and runtime masks do
not collide. Existing `AE_TELE_*` argument expressions remain eagerly evaluated,
and retain caller `__func__` and scope lifetime. The raw front-end formats a blob
only when its compiled and runtime options allow payload collection. Its payload
is formatted UTF-8 text, identified by `payload_encoding` in the exported map.
The lower-level `AE_RAW_*` macros retain lazy disabled-payload evaluation.

`raw::IBackend` owns accepted data. Its `Append(space, bytes)` callback must accept
the whole immutable span or return false without committing any bytes.
`RestoreMetrics(space, writer, output, count)` supplies acknowledged totals before
a writer records new deltas. Optional `Console`, `WriteEnvData` and `Snapshot`
operations support human output, environment metadata and migration. Backends
are held through `shared_ptr`; the default `MemoryBackend` is a bootstrap RAM
journal. A configuration provider may supply `MakeBackend()` instead.

Call `BindWriter(0)` for the main writer and give background workers stable,
distinct IDs. Automatic IDs start at one and skip live claims. Thread exit does
not perform IO: streams remain reachable for `DrainAll()`. Reusing an ended
writer's ID first drains its pending contributions, then restores its accepted
history. `DrainCurrent()` flushes the caller; `DrainAll()` safely checkpoints
all registered writers while other threads continue recording. `MetricFor(index)`
queries canonical accepted history for the caller's writer ID and adds only
unacknowledged deltas from its current and retired streams; late completions,
failed retries and saturated totals are therefore accounted for exactly once.
The query takes the binding gate exclusively to stabilize registration and
acknowledgement while forming that snapshot. Per-writer locks
cover event entry, duration completion and draining without locking the user
scope body. The raw `Stream` API itself retains its one-owner contract.

`SetBackend(target, migrate)` takes the binding gate exclusively, drains the old
writers, invokes the optional migration callback with the old shared backend,
and publishes the new binding only after success. For a bootstrap RAM journal:

```cpp
bool bound = Sink::Instance().SetBackend(target, [&](auto const& early) {
  return target->Append(Sink::kName, early->Snapshot(Sink::kName));
});
```

The callback must itself be all-or-retry. Failure preserves the old binding and
any unacknowledged bytes. Existing scopes retain their writer and runtime
snapshot; late duration deltas are flushed to the new target when those retired
scopes complete. Migration and normal event entry/completion share the binding
gate, preventing old-buffer events from being omitted during rebinding.
Backend callbacks and custom formatting cannot recursively record, drain, bind
or replace another raw front-end on that thread; those attempts are rejected
before writer locks. `failures()` reports front-end failures.

Aether's persistent `TeleStatistics` owns an archive of the accepted blocks and
environment metadata, with runtime backend ownership outside reflection. The
normal domain save/load path commits and restores that archive. A RAM journal
acknowledgement alone is not a filesystem durability barrier: object graph IO
occurs at application checkpoints, outside recording callbacks. Raw-core
construction and destruction never implicitly save an object graph.

The Aether integration provides a generated stable callsite catalog. Generic
front-ends can supply `Identity::ResolveIndex(file, line, tag)` for explicit or
catalog-based indices. The fallback hashes the callsite and rejects detected
collisions instead of silently sharing an index. `ExportMap(out, build)` exports
observed callsites with owned file/line, tag labels and full format text when
`AE_TELE_RAW_DEBUG=1`; it does not discover unexecuted sites. Application build
catalogs can provide the complete declaration table.

## Validation

```text
cmake -S . -B build -DAE_TELE_BUILD_TESTS=ON -DAE_TELE_INSTALL=OFF
cmake --build build --config Release --parallel 4
ctest --test-dir build -C Release --output-on-failure
```

Tests cover compile/runtime disabling, fields and console, no-heap recording,
namespace ownership, both legacy include orders, flat nested scopes, failed
drain retries, reentrant callbacks, malformed and interrupted records, four
writers, live runtime changes, raw numeric limits, metric restore, independent
restart processes, and two builds with different indices/maps. GNU/Clang tests
can enable `-DAE_TELE_RAW_TSAN=ON` for ThreadSanitizer.
Native file tests also exercise short/failed writes, rollback and sync failures,
permanent poisoning, exclusive writer ownership, four streams sharing one file,
and separate-process recovery without a stop event or destructor cleanup.

For a real consumer-header/API test, configure with
`-DAE_TELE_AETHER_SOURCE=/path/to/aether-client-cpp`; this builds and runs against
that checkout's `aether/tele.h`, module/tag headers, configuration and concrete
statistics alias without downloading its unrelated crypto dependencies. This
does not claim a complete Aether client build or a cloud smoke test.

Test builds also export `test-raw-tele.map.json` and
`test-raw-reindexed.map.json` next to their executables, showing two different
index tables and the same compact binary schema.
