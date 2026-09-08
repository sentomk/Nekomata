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
  feature. The published roadmap lives in `README.md` and `docs/` — that
  is the only place phases are vocabulary.
- **Formatting**: `clang-format` (`.clang-format`); CI enforces it.

## Testing doctrine

Four layers, each with a different contract:

| Layer | Contract | Examples |
|---|---|---|
| Test suites | encoded bugs never return | `hello_reload`, `rejections` |
| Runtime invariants | known bug classes never stay silent | entry-prologue guards, anchor consistency, span checks |
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

## CI

PRs run the build matrix (GCC/Clang, DWARF on/off, release),
sanitizers, clang-format and clang-tidy — including a real hot reload
on every leg. Keep the PR suite fast; anything long-form belongs in the
nightly lane, not in front of every pull request.
