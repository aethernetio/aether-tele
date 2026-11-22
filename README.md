# aether-tele

C++20 binary logging and flat profiling for applications with a limited RAM
and storage budget. Record compact data, decode it with the matching build map,
and choose how it reaches a file, flash or another destination.

- **Small indices, separate context.** Records identify a logging callsite by
  its numeric tag. The build map supplies descriptions, source locations and
  the binary layout. Indices can change between builds, so frequently used
  tags can receive the shortest encoding. Keep each build's map with its data.
- **One entry for a scope's lifetime.** Time uses exponential deltas; duration
  occupies one byte. Scope exit patches its original entry instead of emitting
  another record. If those bytes have already drained, the duration stays
  explicitly unknown. Profiling is flat and inclusive, with count and
  duration statistics rather than a parent/child tree.
- **Independent spaces and runtime controls.** Modules can use separate,
  type-safe namespaces with their own tag indices, collection masks and writer
  streams. Logs, fields, counters and timings can change at runtime within
  the features enabled at compile time.
- **Application-owned drain.** The raw core records into fixed RAM arenas
  without allocating. As an arena fills, it calls your synchronous drain
  callback. A rejected batch remains available unchanged for
  retry; the application owns storage, scheduling and durability guarantees.
- **Statistics that continue after restart.** Restore totals from acknowledged
  metric batches before recording, then accumulate new contributions without
  sending the restored history again. A stop event is unnecessary. Raw records
  continue across drains and restarts in self-contained writer blocks.
  This implementation assumes normal, complete writes; interrupted-file recovery
  is outside its scope. Unacknowledged RAM changes are not durable.

The allocation-free `raw.h` core is accompanied by a sink/trap front-end for
macro-based application logging and per-thread writer management. The existing
`TeleSink` / `ITrap` API and `StatisticsTrap` serialization remain available.

## Toolchain support

The public headers require C++20. Both exception-enabled and exception-disabled
consumers are supported, and the CMake targets preserve the consumer's policy.
With exceptions enabled, callback exceptions are contained and failed drains
remain available for retry. Without exceptions, callbacks must report failures
through their return values; allocation failures follow the toolchain's fatal
failure policy. The raw library does not require RTTI. Backends can optionally
expose a process-local `TypeToken()` for checked concrete-type access without
RTTI; the default token is null.

The allocation-free recording and lazy-payload guarantees below apply to the
low-level raw core. The sink/trap front-end maintains runtime state: its macros
currently access the sink singleton and evaluate arguments even when event
collection is disabled. Disabling console output also does not promise removal
of all console-related code from the binary.

## Raw telemetry

Each `Space<Identity, compiled_options, Clock>` has an independent runtime mask.
Each writer owns a `Stream<Space>`:
its fixed RAM arena, metrics and callbacks. The stream must not be
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
while the original RAM bytes remain mutable; it emits no completion record.
An instant log uses the unknown duration code when that field is enabled.
Records are appended consecutively, including when their field masks differ. A scope snapshots runtime settings at entry; changes affect
new events/scopes; existing scopes retain their entry snapshot. Durations
use a monotonic clock; default timestamps use wall-clock microseconds.

## Drain and restart

Before a whole record would overflow RAM, the stream synchronously calls its
drain callback on the writer thread. There are no internal locks around user
code. Before invoking the callback the pending bytes are frozen and every outstanding duration
ticket is invalidated, including on failure. Open scopes retain code 255 forever
in those drained bytes; their exact RAM statistics still update on exit.
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

Complete telemetry blocks concatenate directly in a file/flash region. A block
contains its kind (logs or metrics) and writer count. Each writer section has
one ID, body byte length and record count. Log layout is shared by the block,
with an absolute timestamp base per timed writer section. Individual logs have
only their tag and configured data: **no control byte, writer ID, sequence or
wide flag**. Tag zero inside a timed writer section resets that writer's time
base, followed by an absolute u64 timestamp; it never repeats the writer ID.
Every drain is independently decodable, without loading a previous log line.

A `Stream` owns one writer, so its synchronous callback emits a block with
writer count one. `wire::WriteBlockPrefix` and `WriteWriterHeader` also support
multi-writer blocks; both decoders validate and read these. Sharing a file does
not require waiting for other writers before acknowledging one writer's batch.
The block log-field layout is described once in its header, also documented by
the build map. Compile-time configuration limits available fields. The existing
optional runtime field controls remain supported: changing layout finishes the
old block instead of adding masks to every record. Open scopes keep their
original field snapshots, and drained durations stay explicitly unknown.

Start, stop, pause and resume remain ordinary application-defined tags.
A missing stop marker does not prevent reading or appending after an abrupt
exit. `Reconstruct` preserves each writer's physical append order across process
restarts and then merges available writer heads by timestamp. There is no
persisted sequence number. The C++ `DecodedRecord::sequence` member is retained
only as a decoder-derived append ordinal for source compatibility, and is not
printed or serialized. Supply capture files in append order. Without timestamps
only per-writer append order is known; equal timestamps do not establish
cross-thread causality. Repeated input ranges are not deduplicated. Returned
record views borrow the input bytes.

`DrainMetrics()` first drains logs, then writes incremental flat metric batches
even when logs are compiled/runtime-disabled. RAM `MetricFor()` totals are not
reset, but only not-yet-acknowledged contributions are sent. A repeated drain
without updates emits nothing. A failed batch keeps identical block bytes
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

The native adapter serializes callback spans and appends their raw bytes.
`Open()` reads a normal complete file and rebuilds per-writer time state;
`RestoreMetrics()` imports acknowledged totals without writing them again.
There is no scanning for block boundaries, tail repair or truncation recovery.
Callbacks are synchronous; stop their writers before closing the file.

This component may allocate and block during filesystem IO. It is separate from
the allocation-free raw core and can be omitted with
`-DAE_TELE_BUILD_FILE_DRAIN=OFF` (the default for cross/embedded builds).
No sessions, checksums or additional file envelope is added.

## Multithreaded process restart test and byte analysis

The host fixture `test-threaded-restart` uses one raw `Stream` per worker and
one shared durable `FileDrain`. Its Python driver starts **five separate
application processes**, each with four concurrent workers writing 1000 scoped
logs to the same append-only capture. Worker creation order changes between
boots; logical writer IDs 1..4 remain stable. Both runtime and native thread IDs,
process IDs and restored counts are included in the report. Each restart checks
the old file prefix, log payloads/order, exact per-writer and
combined metrics, and that restoring/draining history does not write it again.
One boot exits with `_Exit` after successful checkpoints, without destructors or
an application stop marker. This tests acknowledged writes, not termination
during a write or recovery of unacknowledged RAM.

From a native checkout (append `.exe` on Windows; multi-configuration generators
also put the binary under `Release/`):

```text
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DAE_TELE_BUILD_TESTS=ON -DAE_TELE_INSTALL=OFF
cmake --build build --config Release --target test-threaded-restart
python tests/raw/threaded_restart.py --exe build/tests/test-threaded-restart --output-dir restart-results
python tools/analyze_raw.py restart-results/capture.raw --map restart-results/map.json --records restart-results/decoded.jsonl --output restart-results/summary.json
```

`--output-dir` must name a new directory; omitting it uses temporary files.
The driver accepts `--boots`, `--workers`, `--events` and `--payload-bytes`.
It also runs through CTest as `test-threaded-restart` when Python and the native
file target are enabled. Golden byte-accounting tests run as `test-raw-analysis`.

The deterministic workload advances timestamps by 1000 us per log and scope
durations by 5 us. It measures encoded size, not real-world event timing or IO
throughput. With defaults it writes 20,000 logs and restores count/samples 20,000,
sum 100,000 us, min/max 5 us. Each log contains a 12-byte parameter blob:
little-endian u32 boot, writer and item number.

The analyzer decodes both log and metric blocks, emits optional JSONL in
physical append order, and partitions every capture byte. It reports averages
excluding only the **contents** of parameter blobs:

- `average_log_bytes_excluding_payload`: record bodies only, including blob-length
  prefixes. Defaults give **4 bytes/log**: tag 1 + time 1 + duration 1 + blob length 1.
- `average_with_log_block_headers_excluding_payload`: also amortizes log-block
  headers and any in-body time resets; defaults give **4.136 bytes/log**.
- `average_capture_bytes_excluding_payload`: also amortizes metric blocks;
  defaults give **4.232 bytes/log** with one checkpoint every 127 events plus
  the final checkpoint.

The default 20,000-log capture is 324,640 bytes, including 240,000 bytes of
parameters, 80,000 bytes of log overhead, 2,720 bytes of log-block headers and
1,920 bytes of metric blocks. These are decoded byte counts, not a fixed-size
assumption. Wider tags, time deltas and blob lengths increase record size;
writer ID width affects headers only. No-log captures return `null` averages.
Build-map JSON and decoded JSONL are external artifacts, excluded from capture size.

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
with overlapping indices cannot be detected automatically. Ordinary indices are limited to 1..65535; index 0 is the absolute-time command. They use **1, 2 or 4 bytes**. Assign frequent
tags indices 1..239, less frequent tags 240..4095, and the rest 4096..65535.
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

The wire codecs use the pinned `aethernet-numeric` revision from CMake.
Link `aether::tele_raw` for raw headers and numeric include requirements.
The **thread_blocks** format replaces the experimental flat-record layout;
use a new capture and its matching map. Old maps are rejected, and old capture
bytes must not be appended to a new capture.

All integers are little endian. A telemetry block is:

```text
kind:u8 | writer_count:StreamCodec | log_fields:u8 (logs only)
  repeated writer_count times:
    writer:StreamCodec | body_bytes:BlobCodec | records:MetricCodec
    base_time_us:u64 (timed logs only)
    body[body_bytes]
```

Each block must have at least one writer, every writer ID must be unique within
that block, and each nonempty body must match its declared byte length and
record count. Record counts exclude in-body absolute-time reset commands.
Both readers reject malformed/truncated bodies and cross-writer length overruns.
No magic, build hash, checksum or on-wire format version is added.

```text
log record:    TAG | time_delta? | duration:u8? | blob_length? | blob_bytes?
metric record: TAG | count:MetricCodec | samples:MetricCodec
                  | sum_us:MetricCodec | min_us:MetricCodec | max_us:MetricCodec
absolute time: 0   | absolute_time_us:u64     (timed log bodies only)
```

Every metric value independently uses
`TieredInt<uint8_t, 200, 5000, 100000000>`:

| Value | Bytes |
|---|---:|
| 0..200 | 1 |
| 201..5,000 | 2 |
| 5,001..100,000,000 | 4 |
| 100,000,001..2,182,612,529,211,367,680 | 8 |

There is no wide fallback. RAM statistics, accepted-history aggregation and
outgoing deltas saturate at `raw::kMetricMax`, the codec's upper bound. Startup
restoration atomically rejects values above that bound. Timestamps remain u64.

Time uses `Exponential<FixedPoint<uint32_t,64.0>, TimeCodec, 1e-6,60.0,511>`.
The encoder chooses a value no larger than the delta from the previous decoded
time; rounding errors do not accumulate across events. Deltas through 6535 us
fit one byte, larger deltas through 60 seconds use two. A time regression,
runtime epoch change or larger gap emits an in-body absolute-time reset.

Duration uses the same fixed-point/exponential range with 253 numeric codes.
The encoder selects the nearest decoded value. Its maximum relative error for
integer-microsecond input is about 5.56%; endpoints are exact.

| Duration byte | Meaning |
|---:|---|
| 0 | below 1 us |
| 1 | exactly 1 us |
| 2..252 | approximate duration |
| 253 | exactly 60 seconds |
| 254 | above 60 seconds |
| 255 | unknown, including a scope still open when drained |

Exact time/duration tables are exported in the debug map. For one-byte tags,
time deltas and blob lengths, record overhead excluding parameter contents is:

| Fields | Bytes/log, excluding block headers |
|---|---:|
| Tag only | 1 |
| Time | 2 |
| Time + duration | 3 |
| Time + duration + parameter-length prefix | 4 |

The stream owns a Capacity-byte body arena with a fixed 32-byte reserved header
prefix, a Capacity+32 scratch arena and its metric table. No heap allocation is
introduced in the raw core. Header widths can grow without moving body bytes or
invalidating live duration tickets. Each drain freezes the full block and
invalidates tickets, including on failure; successful retries cannot patch
already acknowledged bytes. Capacity must fit the BlobCodec body-length range.

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

Aether's persistent `TeleStatistics` owns an archive of the accepted flat records and
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
