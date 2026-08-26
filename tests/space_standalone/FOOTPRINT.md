# Footprint measurements

Platform: Windows 10 (build 26200), AMD64, little-endian.
Compiler: MSVC 19.50.35717 (`dumpbin` 14.50.35717.0), C++20, `/O2 /Zc:preprocessor`.
Build: `cmake --build build-space --config Release --target space_fp_*`.
ESP-IDF was not present (`IDF_PATH` unset). No ESP32 numbers were invented.
See `esp32/README.md` for the optional IDF target.

Section sizes below are **raw PE section sizes** from `dumpbin /HEADERS`
plus on-disk executable size. PE files pad sections to file alignment, so
`dumpbin /SUMMARY` virtual sizes are larger than raw `.text`.

## Code / rodata / data

| Binary | on-disk | size of code | `.text` raw | `.rdata` raw | `.data` raw | `.bss` (uninit) | Δ disk vs baseline | Δ `.text` vs baseline |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `space_fp_baseline` (empty `main`) | 9728 | 0xC00 (3072) | 0xC00 (3072) | 0xE00 (3584) | 0x200 | 0 | 0 | 0 |
| `space_fp_compile_out` | 16384 | 0x1400 (5120) | 0x1400 (5120) | 0x2000 (8192) | 0x200 | 0 | +6656 | +2048 |
| `space_fp_one_space` | 24064 | 0x3000 (12288) | 0x3000 (12288) | 0x2000 | 0x200 | 0 | +14336 | +9216 |
| `space_fp_two_spaces` | 28160 | 0x3C00 (15360) | 0x3C00 (15360) | 0x2400 (9216) | 0x200 | 0 | +18432 | +12288 |

Commands:

```
dumpbin /HEADERS space_fp_baseline.exe
dumpbin /HEADERS space_fp_compile_out.exe
dumpbin /HEADERS space_fp_one_space.exe
dumpbin /HEADERS space_fp_two_spaces.exe
```

`AE_TELE_SPACE_COMPILE_OUT` still links the library and emit TUs; macros do
not construct `Event` or read the clock.

Python check of the compile-out binary (`PktRx`, `network_emit.cpp`,
`demo::network`, `kPoll`, `NetError`) found **no hits**. Host decoder builds
define `AE_TELE_SPACE_HOST` and do contain names.

## Stack

MSVC has no `-fstack-usage`. CMake option `AE_TELE_SPACE_STACK_USAGE` exists
for GCC/Clang and was **not** run on this toolchain.

`space_stack_probe` under `/O2` reported `event_span_bytes=0` and
`flush_span_bytes=0`: the capture helper did not observe a distinct hot-path
frame. The `Event` object is a stream pointer plus `TimeType` (16 bytes
typical). Log buffers are **not** on the stack: `TeleStorage` allocates
`kMaxStreams * kStreamCapacity` (8 × 65536 = 524288 bytes) once in the
constructor. Decoder stack was not measured separately (host `std::vector`
path).

## Heap (`space_heap_probe`, scoped `operator new`)

| Region | alloc calls | notes |
| --- | ---: | --- |
| storage construction | 1 | `unique_ptr` log arena, 524288 bytes |
| 1000 `EmitPoll` events | **0** | acceptance criterion met |
| `SerializeBlob` (flush) | 16 | `std::vector` growth for the ATS1 blob |
| peak live | 552641 | includes the arena + blob |
| total bytes | 552641 | |

Restart loading and host decode also allocate (file I/O / `std::vector`);
they are not the embedded event path.

## ThreadSanitizer

Not run: the available toolchain is MSVC. CMake option `AE_TELE_SPACE_TSAN`
exists for GCC/Clang.

## Exponential codec (measured on this build)

`network` uses `ExponentialCodec` / `ae::Exponential` with min=1 tick,
boundary=4e9 ticks, `BoundaryCode=1049834`. Round-trip of 10 ticks decoded
as 16. Max relative error on values ≤ 1e6 ticks: **0.6384**. Absolute codes
round-trip. Application packed deltas remain exact.
