# Nekomata

> Native hot-reload for C/C++ — code that lives long enough grows a second tail.
> No restarts, no refactor.

Nekomata is an open-source, **true-native hot-reload tool for C/C++** — the same
category of capability as [Live++](https://liveplusplus.tech/), built in the
open. It recompiles changed translation units while your program runs,
relocates the fresh machine code against the live process's real addresses, and
redirects function entry points:

- **no restarts** — the process keeps running, the next call takes the new code
- **no refactor** — no plugins, no interface indirection, no library splits
- **state preserved** — globals and statics keep their values across reloads

[![CI](https://github.com/sentomk/Nekomata/actions/workflows/ci.yml/badge.svg)](https://github.com/sentomk/Nekomata/actions/workflows/ci.yml)

## Status: pre-alpha (Phase 1 prototype working)

The core mechanism is proven end-to-end on Linux/ELF: edit a function, drop a
fresh object file, and the running process takes the new code on the next
call — with globals and statics preserved. To set expectations honestly:

| Capability | Today | Target |
|---|---|---|
| Hot reload | ✅ single TU, `-O0`, Linux/ELF (see the demo below) | whole programs, real projects |
| Platforms | Linux/ELF (kernel builds everywhere) | then Windows/PE |
| Compilers (as reload source) | GCC, Clang ≥ 14 | MSVC (Phase 3) |
| PIE binaries | — (`-no-pie` for now) | Phase 2 |
| Optimization (`-O2`) builds | — | late phase (inline handling) |
| Class layout migration | — | late phase (Phase 5) |

### Try it (Linux)

```sh
cmake --preset debug && cmake --build --preset debug
bash build/debug/examples/hello_reload/run_demo.sh
```

The demo edits `tick()` from `++g_counter` to `g_counter += 10` while the
process runs, and asserts the counter continues from its old value:
state survives the swap, no restart. Expected transcript:
`examples/hello_reload/expected_output.txt`.

## What "true-native" means

Four acceptance criteria guide the project:

1. **Zero code changes** — no plugin/interface/function-pointer rewrites.
2. **In-process replacement** — reload without restarting; next call runs new code.
3. **State preservation** — globals/statics stay consistent across reloads.
4. **Multi-platform, multi-compiler** — at least Linux/ELF + Windows/PE, Clang and MSVC.

## How a reload works

```
edit .cpp
  -> compiler frontend emits fresh .o (relocations against a zero base)
  -> symbol backend locates functions (address + size) in the live process
  -> relocations are fixed up against real runtime addresses (a runtime mini-link)
  -> binary backend writes the new body into executable-reserved pages
  -> the old function entry is overwritten with a 5-byte `jmp rel32`
  -> the next call takes the new code
```

## Architecture

A platform-neutral kernel sits behind five interfaces. Everything
platform-specific is a pluggable backend in its own directory with its own
tests.

```
              +------------------------------------------+
   ChangedSet |  kernel (platform-neutral, include/neko)  |
 ------------->  patch_planner    what must be recompiled? |
              |  symbol_provider where is everything?     |
              |  object_loader    mini-link the fresh .o   |
              |  code_substituter exec memory + redirect  |
              |  state_manager    keep state alive        |
              +--------------------+---------------------+
                                   |
        +--------------------------+--------------------------+
        v                          v                          v
   Linux backends             Windows backends           future backends
   Clang / DWARF / ELF        MSVC / PDB(DIA) / PE       (ARM, …)
   in-process agent           in-process agent
   (mmap/mprotect)            (VirtualProtect)
```

## Repository layout

Nekomata is a compiled library, not a header-only library. Common public APIs
have short entry points; extension APIs are grouped by module. Implementation
files and private headers live under `src/`:

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

## Roadmap

| Phase | Scope | Status |
|---|---|---|
| 1 · Single-function prototype | Linux/ELF, `-O0`, one TU, fixed-moment swap | ✅ done |
| 2 · Real-world usable | DWARF ranges, whole-TU reloads, dependency graph, safe points, PIE | ⏳ next |
| 3 · Windows | PE/PDB (DIA), MSVC + `/hotpatch` | ⏳ |
| 4 · Optimized builds | `-O2` inline units (`DW_TAG_inlined_subroutine`), COMDAT folding | ⏳ high risk |
| 5 · Class layout migration | object migration + vtable updates | ⏳ hardest |

## When you should NOT use nekomata

Being honest about the boundary is part of the design:

- **Restarts cost seconds?** Use mold/lld + ccache + incremental builds — faster
  and more reproducible. nekomata sells *state preservation*, not raw speed.
- **Stateless, rolling-restart services?** Hot reload is a non-need.
- **Compliance-forbid self-modifying code** (finance/aviation/medical)? Skip.
  nekomata is a development-time tool, not a production hot-patcher.
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

CI uses clang-format 18 with two-space indentation. The same script checks or
formats all tracked and new non-ignored C/C++ sources, headers, and header
templates, including backends and examples; vendored code is excluded:

```sh
bash scripts/format.sh --check
bash scripts/format.sh --fix
```

Set `CLANG_FORMAT` if the version-18 executable has a different name or path.

Static analysis runs on the kernel, enabled backends, and CLI targets, including
their project headers. Tests and examples are still built and executed but are
not analyzed. CI pins clang-tidy 18 and treats enabled diagnostics as errors:

```sh
CC=clang-18 CXX=clang++-18 cmake --preset tidy \
  -DNEKOMATA_CLANG_TIDY_EXECUTABLE=clang-tidy-18
cmake --build --preset tidy
ctest --preset tidy
```

The `tidy` build directory is separate from normal builds. To repeat analysis
without source changes, use `cmake --build --preset tidy --clean-first`.

Platform notes:

- **Linux** — the primary target; all backends will live here (Phase 1–2).
- **macOS / other** — kernel-only build, verified in CI, by design.
- **Windows** — arrives with Phase 3.

## Contributing

The project is pre-alpha and its interfaces are still evolving. Start with
this README and the runnable examples, and open an issue or discussion for
anything from API shape to backend design.

Naming follows the C++ standard library style: `snake_case` everywhere, types
included (like `std::string_view`). Commit messages use Conventional Commits,
with identifiers wrapped in backticks.

## License

Nekomata is licensed under the [MIT License](LICENSE).

## Credits & prior art

- [Live++](https://liveplusplus.tech/) (Molecular Matters GmbH) — the mature
  commercial reference this project benchmarks against. Stefan Reinalter's
  ongoing implementation blog series: [Introduction](https://liveplusplus.tech/blog/posts/2026-01-26-introduction.html),
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
