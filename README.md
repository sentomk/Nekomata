<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="assets/nekomata-dark.svg">
    <source media="(prefers-color-scheme: light)" srcset="assets/nekomata-light.svg">
    <img alt="Nekomata — native hot-reload for C/C++" src="assets/nekomata-light.svg" width="420">
  </picture>
</p>

# Nekomata

Nekomata is an open-source, **true-native hot-reload tool for C/C++**. It
recompiles changed translation units while your program runs, relocates the
fresh machine code against the live process's real addresses, and redirects
function entry points:

- **no restarts** — the process keeps running, the next call takes the new code
- **no refactor** — no plugins, no interface indirection, no library splits
- **state preserved** — globals and statics keep their values across reloads

[![CI](https://github.com/sentomk/Nekomata/actions/workflows/ci.yml/badge.svg)](https://github.com/sentomk/Nekomata/actions/workflows/ci.yml)

## Status

The core mechanism works end to end on Linux/ELF. What follows is the honest
boundary: what Nekomata does today, and what it refuses rather than what nobody
has tried yet.

| Capability | Today |
|---|---|
| Hot reload | one translation unit at a time, `-O0`, Linux/ELF |
| State preservation | globals and statics keep their values across reloads |
| Multiple functions per reload | yes — applied all-or-nothing; a failed attempt rolls back |
| PIE binaries | yes, when hot objects are built `-fpie` (GOT-style `-fpic` is not supported yet) |
| Cross-TU references | not supported yet — a reloaded function cannot call into another unit |
| New globals, changed global layout | not supported yet — refused with a diagnostic |
| Optimized builds (`-O2`) | not supported yet — an inlined function has no body of its own |
| Platforms | Linux/ELF; the kernel itself builds on macOS, without a backend |
| Compilers | GCC and Clang ≥ 14 as the source of reloads |

Threading is the caller's business for now: reloads happen at a quiescent point
between `update()` calls, on one thread.

## Try it (Linux)

```sh
cmake --preset debug && cmake --build --preset debug
bash build/debug/examples/hello_reload/run_demo.sh
```

The demo edits `tick()` from `++g_counter` to `g_counter += 10` while the
process runs, and asserts the counter continues from its old value: state
survives the swap, no restart. Expected transcript:
`examples/hello_reload/expected_output.txt`.

## Inspect a binary (Linux, read-only)

```sh
./build/debug/tools/nekomata/nekomata inspect <binary>
```

`inspect` reads an existing binary without running it, attaching to a process,
or modifying the file: compilation units, function names, declaration
locations, code ranges, and how each DWARF function associates with the ELF
symbol table. Exit code zero means the inspection finished, not that every
function is safe to patch.

The details — what is matched, what is refused, the resource budgets, and the
libdwarf dependency — are in [docs/inspect.md](docs/inspect.md).

## When you should NOT use Nekomata

Being honest about the boundary is part of the design:

- **Restarts cost seconds?** Use mold/lld + ccache + incremental builds — faster
  and more reproducible. Nekomata sells *state preservation*, not raw speed.
- **Stateless, rolling-restart services?** Hot reload is a non-need.
- **Compliance-forbid self-modifying code** (finance/aviation/medical)? Skip.
  Nekomata is a development-time tool, not a production hot-patcher.
- **Verification & release builds** must stay reproducible and clean — hot reload
  serves iteration only and never enters shipped artifacts.

## Building from source

Requirements: CMake ≥ 3.21, Ninja, a C++20 compiler (GCC ≥ 11 / Clang ≥ 14 /
MSVC 2022).

```sh
cmake --preset debug          # configure (Ninja, build/debug)
cmake --build --preset debug  # build
ctest --preset debug          # test
./build/debug/tools/nekomata/nekomata --version
```

Presets: `debug`, `release`, `asan` (ASan + UBSan), `tidy` (clang-tidy).

Static analysis runs on the kernel, enabled backends and CLI targets; tests and
examples are built and executed but not analyzed. CI pins clang-tidy 18 and
treats enabled diagnostics as errors:

```sh
CC=clang-18 CXX=clang++-18 cmake --preset tidy \
  -DNEKOMATA_CLANG_TIDY_EXECUTABLE=clang-tidy-18
cmake --build --preset tidy
```

Formatting is enforced in CI:

```sh
bash scripts/format.sh --check   # or --fix
```

The Linux inspector needs a C compiler for libdwarf 2.3.2, and network access
on first configuration unless its source directory is supplied:

```sh
cmake --preset debug \
  -DFETCHCONTENT_SOURCE_DIR_LIBDWARF=/absolute/path/to/libdwarf-code-2.3.2
```

`-DNEKOMATA_ENABLE_DWARF_INSPECTION=OFF` builds the runtime and its tests
without that dependency. The TUI is optional and off by default; it fetches
[Glyph](https://github.com/sentomk/Glyph) and is enabled with
`-DNEKOMATA_TUI=ON`.

Platform notes:

- **Linux** — the primary target; every backend lives here.
- **macOS** — kernel-only build, verified in CI, by design.
- **Windows** — the kernel and the TUI build and run under CI; there is no PE/PDB
  backend yet.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for conventions, the testing doctrine,
and the suite map; [AGENTS.md](AGENTS.md) carries the same rules for AI coding
agents. The planned work — including what is deliberately out of scope — is in
[docs/roadmap.md](docs/roadmap.md).

## License

Nekomata is licensed under the [MIT License](LICENSE).

## Credits & prior art

- [Live++](https://liveplusplus.tech/) — a commercial hot-reload tool for C/C++,
  with an implementation blog series worth reading:
  [Introduction](https://liveplusplus.tech/blog/posts/2026-01-26-introduction.html),
  [Phase 0](https://liveplusplus.tech/blog/posts/2026-02-09-phase_0_motivation_goals.html),
  [Phase 1](https://liveplusplus.tech/blog/posts/2026-02-23-phase_1_build_information.html).
- [RuntimeCompiledCPlusPlus](https://github.com/RuntimeCompiledCPlusPlus/RuntimeCompiledCPlusPlus) —
  pioneered runtime recompilation; source of the dependency-graph idea.
- [crosire/blink](https://github.com/crosire/blink),
  [fungos/cr](https://github.com/fungos/cr),
  [ddovod/jet-live](https://github.com/ddovod/jet-live) — the surveyed open-source
  projects exploring native hot reload.
- [Microsoft Detours](https://github.com/microsoft/Detours) — the classic
  reference for function interception and trampolines.
- [LLVM ORC](https://llvm.org/docs/ORCv2.html) — kindred machinery for runtime
  relocation and memory management.
