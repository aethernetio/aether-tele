# AGENTS.md

Guidance for coding agents working on `aether-tele`.

## Project Overview

`aether-tele` is a C++20 telemetry/logging library.

The main library target is `aether-tele`, with CMake alias `aether::tele`.
The public namespace is `ae::tele`.

The project provides a macro-heavy logging API backed by a sink/trap architecture:
- `ITrap` is the trap interface.
- `TeleSink<ConfigProvider>` coordinates telemetry/log handling.
- Traps are owned through `std::shared_ptr<ITrap>`.
- Configuration providers may use `consteval` patterns.
- `tele.h` is the umbrella public header and uses IWYU export pragmas.

## Repository Layout

- `src/aether-tele/` — public headers and implementation files.
- `src/aether-tele/traps/` — trap implementations.
- `src/aether-tele/configs/` — telemetry configuration helpers/providers.
- `src/aether-tele/env/` — environment-related helpers.
- `tests/` — Unity-based tests.
- `cmake/` — CMake helper modules, including CPM integration.
- `.github/workflows/` — CI definitions; treat CI as authoritative for release validation.
- `.clang-format` — Google-derived formatting rules.
- `.clang-tidy` — static-analysis configuration.

## Build, Test, and Install Notes

CMake is the build system.

Important project options:
- `AE_TELE_BUILD_TESTS` — enables tests.
- `AE_TELE_INSTALL` — enables install rules.

Tests use Unity and are registered with CTest under the `test-tele` target.

When changing behavior, keep local validation aligned with the CI workflow. The release CI builds in Release mode with tests enabled and install disabled.

## Style and Conventions

- Language standard: C++20.
- Formatting follows the repository `.clang-format`.
- Static-analysis expectations are captured in `.clang-tidy`.
- Public API lives under namespace `ae::tele`.
- Public headers use include guards.
- Public headers should preserve Apache 2.0 license headers.
- Preserve IWYU export pragmas in umbrella/public headers where present.
- Keep macro-based logging API compatibility unless the task explicitly requires changing it.

## Architectural Notes

- Preserve the existing sink/trap separation.
- Do not change trap ownership semantics casually; traps are shared via `std::shared_ptr<ITrap>`.
- Be careful with compile-time configuration providers and `consteval` behavior.
- Public headers may affect downstream users; avoid unnecessary public API changes.
- `tele.h` is the umbrella include and should remain consistent with public header additions/removals.

## Dependency Notes

Dependencies are managed through CPM.cmake:
- `aethernet-numeric`
- `aether-miscpp`
- `Unity` for tests

Do not vendor or manually edit fetched dependency contents.

## Edit Guidance

Prefer small, focused edits.

Before changing public headers, check:
- namespace consistency,
- include guards,
- umbrella header implications,
- ABI/API compatibility,
- license header preservation.

Before changing tests, check that they remain Unity/CTest-compatible.

Before changing CMake, check target names, aliases, options, install behavior, and CI expectations.

## Validation Expectations

For code changes, agents should ensure the relevant CMake configuration, build, and CTest flows remain valid. CI workflow definitions are the authoritative reference for release validation.

Documentation-only changes generally do not require build validation unless they alter documented commands, options, or public behavior expectations.

## Do Not Edit Generated or Third-Party Files

Avoid modifying:
- `build-*`
- build output directories
- `.cache/`
- generated artifacts
- `cmake/CPM.cmake`
- CPM package lock files
- opencode `package.json`
- opencode `package-lock.json`

If a task appears to require editing generated, cached, vendored, or third-party files, stop and ask for clarification.
