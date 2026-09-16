# Contributing to nekomata

Thanks for helping build native hot-reload for C/C++. This file is the
short version of how to work in this repository; `AGENTS.md` covers the
same ground for AI coding agents.

## Build & test

The default contributor loop is:

```sh
bash scripts/configure.sh debug
bash scripts/build.sh debug
bash scripts/test.sh debug
```

Before declaring a change complete, run the full local check:

```sh
bash scripts/check.sh debug
```

`configure.sh` accepts a preset followed by ordinary CMake options;
`build.sh` and `test.sh` forward their remaining arguments to the build tool
and CTest. The shell entrypoints invoke `tools/envsetup.py` automatically.
That script only creates `.tools/venv` and installs or verifies the versions
in `tools/requirements.txt`: CMake 3.31.10, Ninja 1.13.2 and clang-format
22.1.8. Configuration, builds, tests and formatting remain in `scripts/*.sh`.

Python ≥ 3.8 with `venv` and a C++20 compiler (GCC ≥ 11 / Clang ≥ 14 /
MSVC 2022) are the bootstrap requirements. Initial tool installation needs
network access; native CMake presets remain usable with preinstalled CMake
≥ 3.21 and Ninja in restricted environments.

The complete option and dependency matrix is in the
[README build section](README.md#building-from-source). For TUI work, enable
the optional module explicitly and run its tests:

```sh
bash scripts/configure.sh debug -DNEKOMATA_TUI=ON
bash scripts/build.sh debug
bash scripts/test.sh debug -R '^neko\.tui\.'
```

The kernel builds on Linux, macOS and Windows. Live reload and the ELF/DWARF
suites require Linux; the TUI has Linux, macOS and Windows build coverage.
DWARF support can be disabled with
`-DNEKOMATA_ENABLE_DWARF_INSPECTION=OFF`, for example on restricted networks.

## Conventions

- **Naming**: C++ standard-library style, `snake_case` everywhere, types
  included (`symbol_provider`, `std::string_view`-like).
- **Commits**: Conventional Commits; wrap identifiers in backticks.
- **No internal roadmap codenames** in commit messages, code comments or
  user-facing strings. Say "not supported yet", "planned", or name the
  feature. The published roadmap lives in `docs/roadmap.md` — that
  is the only place phase vocabulary appears.
- **Formatting**: pinned clang-format 22 (`bash scripts/format.sh --check`, or
  `--fix`); CI enforces it.
- **Comments**: write concise prose that explains intent, constraints, or
  invariants. Do not simulate headings with repeated punctuation or numbered
  ruler banners such as `// ---- 1. ... ----`; use a normal sentence and
  code structure instead.

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
  core/                # shared public types and forward declarations
  runtime/             # backend and reload-planning extension interfaces
  tui/                 # optional TUI API
  platforms/elf.hpp    # public Linux backend factory
src/
  core/                # compiled core implementation
  runtime/             # compiled runtime implementation
  platforms/           # platform implementations and private headers
  tui/                 # optional TUI implementation
tests/                 # unit, platform, transaction and acceptance suites
examples/              # runnable consumers
scripts/               # configure, build, test and formatting entrypoints
tools/                 # environment setup and development-only utilities
```

Use `<neko/session.hpp>` and `<neko/platforms/elf.hpp>` for the Linux reload
entry point, and `<neko/log.hpp>` for diagnostics. With
`-DNEKOMATA_TUI=ON`, `<neko/tui/tui.hpp>` exposes the optional TUI API.
`<neko/neko.hpp>` remains the convenience include, but is not an amalgamated
single-file distribution and still requires linking the library. CMake target
names remain `nekomata::neko`, `nekomata::backends::elf` and, when enabled,
`nekomata::tui`; directory names do not change those aliases or the
`neko::elf` namespace. The supported product surface is these library APIs;
programs under `tools/` are development utilities, not a public driver CLI.

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

Suites live under `tests/` by verification level (`neko.<level>.<module>.<case>`
test names, matching CTest labels): `unit/`, `contract/`, `integration/`,
`e2e/`, `tooling/`, `fuzz/`, plus `support/` for the shared registration
helpers and doctest.

| Directory | Covers |
|---|---|
| `tests/unit/runtime/`, `tests/unit/base/` | kernel: generation transactions, managed session API, stream cursors, depfile planning, digests |
| `tests/unit/protocol/` | descriptor and immutable-generation codecs, validation, integrity |
| `tests/unit/backends/elf/`, `tests/unit/backends/dwarf/` | offline ELF/DWARF parsing and metadata |
| `tests/unit/tui/` | TUI layout and input behavior when `NEKOMATA_TUI=ON` |
| `tests/contract/` | public-header hygiene and the API surface (`smoke`, `public_api`) |
| `tests/integration/` | publisher acceptance and embedded-descriptor discovery |
| `tests/e2e/reload/managed/` | managed ELF end-to-end reload, state continuity, rejection recovery and unwatch/resume |
| `examples/hello_reload/` | end-to-end reload: logic swap and state continuity |
| `tests/e2e/reload/multi_tu/`, `tests/e2e/reload/atomicity/` | complete multi-object generations, all-or-nothing commit and recovery |
| `tests/e2e/reload/duplicate_names/` | translation-unit identity for same-named local symbols |
| `tests/e2e/reload/rejections/` | loud rejection, process survival and old-code continuity |
| `tests/e2e/reload/cross_tu/` | cross-object calls inside one generation, including brand-new symbols |
| `tests/e2e/reload/soak/` | multi-generation reload soak: state continuity and executable-slot accounting |
| `tests/e2e/adapters/cmake/` | the CMake adapter end to end, per generator and group form |
| `tests/tooling/` | harness fault injection, CLI contracts, envsetup, sanitizer canary |
| `tests/fuzz/` | libFuzzer on the ELF parser; corpus doubles as regression inputs |

## CI

PRs run Linux GCC/Clang live-reload builds with DWARF on/off and release
coverage, plus the TUI on Linux and macOS, kernel/TUI portability on Windows,
sanitizers, fuzz smoke tests, pinned clang-format 22 and clang-tidy. Actual
reload execution is Linux-only; portability legs do not claim backend
coverage. Keep the PR suite fast; anything long-form belongs in the nightly
lane, not in front of every pull request.
