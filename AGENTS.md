# AGENTS.md

Guidance for AI coding agents working in this repository. Human
contributors: see `CONTRIBUTING.md` — the conventions are the same.

## What this is

nekomata is an open-source native hot-reload library for C/C++. Application
build integration produces changed object files; the library applies complete
generations and redirects function entries in a live process while preserving
program state. The product surface is the library API
(`neko::reload_session`). There is deliberately no supported driver CLI;
everything under `tools/` is development infrastructure.

## Layout

- `include/neko/{core,runtime,tui}/` — public module headers; TUI is optional
- `src/core/`, `src/runtime/` — platform-neutral kernel implementation
- `src/platforms/{elf,dwarf}/` — Linux backends; gated to Linux builds
- `src/tui/` — optional TUI library; enabled with `-DNEKOMATA_TUI=ON`
- `examples/hello_reload/` — acceptance demo, doubles as an end-to-end test
- `examples/playground/` — manual playground (bouncing ball)
- `tests/` — see the suite map in `CONTRIBUTING.md`
- `scripts/` — configure, build, test and formatting entrypoints
- `tools/` — pinned environment setup and development-only utilities

## Verify before declaring done

```sh
bash scripts/check.sh debug
```

This uses the repository-pinned CMake, Ninja and clang-format 22 toolchain.

The ELF/DWARF backends and live-reload suites require Linux. macOS and
Windows validate the platform-neutral kernel, and both build the TUI when it
is enabled. If a change affects platform or reload behavior, verify it on the
relevant platform or push and watch CI; a kernel-only portability build is
not enough for Linux runtime changes.

## Hard rules

1. **snake_case everywhere**, types included. Conventional Commits with
   identifiers in backticks.
2. **No internal roadmap codenames** ("phase 1/2/…") in commit messages,
   comments, or user-facing strings. Say "not supported yet" or "planned".
3. **Every bug fix dies into a test** in the matching suite. Do not
   weaken an existing assertion to make a change pass — if an assertion
   is wrong, that is its own commit with its own reasoning.
4. **Rejection messages are contracts.** `tests/rejections/` and the
   mock harness assert their exact wording; changing a message means
   changing them in the same commit.
5. **Logs go through `neko::log`** (`include/neko/log.hpp`) — the level
   cats. Never `fprintf` a raw `[neko]`-style prefix.
6. New dependencies must be justified against the vendored-doctest
   precedent: configure must not require network unless the feature is
   optional and off by default in restricted environments.
7. The supported product surface is the library API. Do not present
   development utilities under `tools/` as a user-facing CLI or add public
   compatibility promises for them.
8. **Comments are concise prose.** Do not simulate headings with repeated
   punctuation or numbered ruler banners such as `// ---- 1. ... ----`.
   Prefer a short sentence that explains intent, constraints, or invariants.

## Known sharp edges

- `loader.cpp` has repeatedly suffered partial-application corruption
  from incremental edits. Prefer whole-file rewrites for large changes
  there, and always verify the file tail compiles after editing.
- The playground and demo scripts compile the hot TU with exact flag
  sets ("build information"); changing those flags changes test
  semantics — keep `rebuild_hot.sh.in` in sync with the CMake target.
