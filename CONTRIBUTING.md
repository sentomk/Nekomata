# Contributing to nekomata

Thanks for helping build native hot-reload for C/C++. This file is the
short version of how to work in this repository; `AGENTS.md` covers the
same ground for AI coding agents.

## Build & test

```sh
cmake --preset debug && cmake --build --preset debug   # Ninja, build/debug
ctest --preset debug                                   # run everything
```

Requirements: CMake ≥ 3.21, Ninja, C++20 (GCC ≥ 11 / Clang ≥ 14). The
kernel builds on every platform; the ELF/DWARF platforms and their test
suites build on Linux only. DWARF support is optional
(`-DNEKO_DWARF=OFF` builds without it, e.g. on restricted networks).

## Conventions

- **Naming**: C++ standard-library style, `snake_case` everywhere, types
  included (`symbol_provider`, `std::string_view`-like).
- **Commits**: Conventional Commits; wrap identifiers in backticks.
- **No internal roadmap codenames** in commit messages, code comments or
  user-facing strings. Say "not supported yet", "planned", or name the
  feature. The published roadmap lives in `docs/roadmap.md` — that
  is the only place phase vocabulary appears.
- **Formatting**: `clang-format` (`.clang-format`); CI enforces it.

## Layout, headers and file names

Nekomata is a compiled library, not a header-only library. Common public APIs
have short entry points; extension APIs are grouped by module:

```text
include/neko/
  neko.hpp             # convenience umbrella for core + runtime
  fwd.hpp              # public forward-declaration umbrella
  log.hpp              # diagnostic API declarations
  session.hpp          # reload session API
  version.hpp          # configured by CMake into the build include directory
  core/                # shared public types and fwd.hpp
  runtime/             # public backend extension interfaces and fwd.hpp
  platforms/elf.hpp    # public Linux backend factory
src/
  core/                # compiled core implementation
  runtime/             # compiled runtime implementation
  platforms/           # platform implementations and private headers
tests/
  headers/             # standalone public-header and forward-declaration checks
  rejections/          # runtime rejection tests
  vendor/              # third-party test dependencies
examples/              # runnable consumers
tools/                 # command-line programs
```

Use `<neko/session.hpp>` and `<neko/platforms/elf.hpp>` for the Linux
reload entry point, and `<neko/log.hpp>` for diagnostics. `<neko/neko.hpp>`
remains the convenience include, but is
not an amalgamated single-file distribution and still requires linking the
library. CMake target names remain `nekomata::neko` and
`nekomata::backends::elf`; directory names do not change those target aliases
or the `neko::elf` namespace.

The common headers declare the API directly instead of forwarding to private
implementation headers. For example, logging is implemented in
`src/core/log.cpp`; terminal detection and formatting code are not included by
consumers. The public `core/` and `runtime/` headers describe shared types and
extension contracts, not the private ELF parser, loader, or code-page machinery.
Private source directories need not mirror the public API layout.

Module `fwd.hpp` files hold forward declarations and lightweight type aliases.
`<neko/fwd.hpp>` aggregates those public declarations.
Use them when only names, pointers, or references are needed; include the
defining header when a complete type is required. Do not duplicate declarations
at call sites or create empty forward headers for factory-only directories.
Public headers must be self-contained and must not include anything from
`src/`. Implementation include paths must not leak through public CMake usage
requirements. With tests enabled, the normal build compiles every public header
independently, including the configured version header. A separate consumer
test also links and exercises the short public entry points.

New C++ sources use `.cpp`, C++ headers use `.hpp`, and generated header
templates use `.hpp.in`. Reserve `.h` for headers that can be included from
both C and C++, and `.c` for C implementations. A future C API belongs under
`include/neko/c/`, with its C++ bridge under `src/c/`; no C API is exposed yet.
Such headers must guard `extern "C"` with `#ifdef __cplusplus`, expose C-compatible
types, and keep C++ exceptions, containers, and ownership details behind the
boundary. When that API is introduced, compile its headers as both C and C++
and test a C consumer linked to the bridge; file extensions or `extern "C"`
alone do not establish ABI stability.

## Testing doctrine

Four layers, each with a different contract:

| Layer | Contract | Examples |
|---|---|---|
| Test suites | encoded bugs never return | `hello_reload`, `rejections` |
| Runtime invariants | known bug classes never stay silent | entry-prologue guards, anchor consistency, guard pages, span checks |
| Fuzz / stress | probabilistic discovery | condition exploration, randomized reload sequences |
| Review + issues | everything else | — |

Two rules follow from this:

1. **Discovery comes from invariants and diversity, not runtime volume.**
   Running a suite longer catches nothing its conditions don't contain;
   add *conditions* (geometries, code shapes, compilers), not minutes.
2. **Every bug dies into a test.** A fix without a regression test is
   unfinished work. Place it in the suite that matches the failure mode
   (`tests/rejections/` for loud rejections, the demo script for
   reload-semantics bugs, platform suites for parsing).

## Test suite map

| Directory | Covers |
|---|---|
| `tests/smoke.cpp` | kernel interfaces, builds everywhere |
| `examples/hello_reload/` | end-to-end reload: logic swap + state continuity, twice per run |
| `tests/rejections/` | every boundary rejects loudly; process survives; old code keeps running |
| `tests/elf/`, `tests/dwarf/` | offline ELF/DWARF inspection |
| `tests/harness/`, `tests/headers/`, `tests/public_api.cpp` | test infrastructure, header hygiene, API shape |
| `tests/soak/` | multi-generation reload soak: state continuity + arena accounting |
| `tests/fuzz/` | libFuzzer on the ELF parser; corpus doubles as regression inputs |

## CI

PRs run the build matrix (GCC/Clang, DWARF on/off, release),
sanitizers, clang-format and clang-tidy — including a real hot reload
on every leg. Keep the PR suite fast; anything long-form belongs in the
nightly lane, not in front of every pull request.
