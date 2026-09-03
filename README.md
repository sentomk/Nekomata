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

## Status: pre-alpha (Phase 1 in progress)

Nekomata cannot hot-reload anything yet. To set expectations honestly:

| Capability | Today | Target |
|---|---|---|
| Hot reload | not yet | running process, no restart |
| Platforms | kernel builds everywhere; backends pending | Linux/ELF first, then Windows/PE |
| Compilers (as reload source) | — | Clang ≥ 14, then MSVC |
| Optimization (`-O2`) builds | — | late phase (inline handling) |
| Class layout migration | — | late phase (Phase 5) |

## What "true-native" means

Four acceptance criteria, from the [design proposal](docs/proposal.md):

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

A platform-neutral kernel behind four interfaces; everything platform-specific
is a pluggable backend in its own directory with its own tests.

```
              +------------------------------------------+
   ChangedSet |  kernel (platform-neutral, include/neko)  |
 ------------->  patch_planner   what must be recompiled? |
              |  symbol_provider where is everything?     |
              |  code_substituter write + redirect code   |
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

## Roadmap

| Phase | Scope | Status |
|---|---|---|
| 1 · Single-function prototype | Linux/ELF, `-O0`, one TU, fixed-moment swap | 🚧 scaffolding |
| 2 · Real-world usable | DWARF ranges, whole-TU reloads, dependency graph, safe points | ⏳ |
| 3 · Windows | PE/PDB (DIA), MSVC + `/hotpatch` | ⏳ |
| 4 · Optimized builds | `-O2` inline units (`DW_TAG_inlined_subroutine`), COMDAT folding | ⏳ high risk |
| 5 · Class layout migration | object migration + vtable updates | ⏳ hardest |

Details and acceptance criteria per phase: [docs/proposal.md](docs/proposal.md).

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

Presets: `debug`, `release`, `asan` (ASan + UBSan).

Platform notes:

- **Linux** — the primary target; all backends will live here (Phase 1–2).
- **macOS / other** — kernel-only build, verified in CI, by design.
- **Windows** — arrives with Phase 3.

## Documentation

- [docs/proposal.md](docs/proposal.md) — the full design proposal: motivation,
  architecture, mechanisms, phased roadmap, risks, FAQ.
- [docs/prerequisite-knowledge.md](docs/prerequisite-knowledge.md) — the
  onboarding guide for the underlying domains (ELF, x86-64 patching, DWARF).

## Contributing

The project is in its design/scaffolding phase; interfaces are explicitly
marked as drafts that Phase 1 will validate. Reading the proposal first is the
best way in — open an issue or discussion for anything from API shape to
backend design.

Naming follows the C++ standard library style: `snake_case` everywhere, types
included (like `std::string_view`). Commit messages use Conventional Commits,
with identifiers wrapped in backticks.

## License

To be decided (MIT or Apache-2.0) before the first external contribution —
tracked as an open item in the proposal.

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
  landscape this proposal positions against (see proposal, Motivation).
- [Microsoft Detours](https://github.com/microsoft/Detours) — the classic
  reference for function interception and trampolines.
- [LLVM ORC](https://llvm.org/docs/ORCv2.html) — kindred machinery for runtime
  relocation and memory management.
tml) — kindred machinery for runtime
  relocation and memory management.
