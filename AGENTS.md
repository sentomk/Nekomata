# AGENTS.md

Guidance for AI coding agents working in this repository. Human
contributors: see `CONTRIBUTING.md` — the conventions are the same.

## What this is

nekomata is an open-source native hot-reload library for C/C++: it
recompiles changed translation units and redirects function entries in a
live process, preserving program state. The product surface is the
library API (`neko::reload_session`); there is deliberately no driver
CLI (`tools/nekomata inspect` is a development utility only).

## Layout

- `include/neko/{core,runtime}/` — public kernel headers (module split)
- `src/core/`, `src/runtime/` — kernel implementation
- `src/platforms/{elf,dwarf}/` — Linux platforms; gated to Linux builds
- `examples/hello_reload/` — acceptance demo, doubles as the end-to-end test
- `examples/playground/` — manual playground (bouncing ball)
- `tests/` — see the suite map in `CONTRIBUTING.md`

## Verify before declaring done

```sh
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
find include src tests tools examples -type f \( -name '*.cpp' -o -name '*.hpp' \) \
  -print0 | xargs -0 clang-format --dry-run --Werror
```

On macOS this builds the kernel only. The ELF/DWARF platforms and the
reload/rejections suites require Linux — a Linux box or CI. If you only
touched kernel code, the macOS run is sufficient verification; if you
touched `src/platforms/` or anything affecting reload behavior, a Linux
run (or pushing and watching CI) is mandatory.

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

## Known sharp edges

- `loader.cpp` has repeatedly suffered partial-application corruption
  from incremental edits. Prefer whole-file rewrites for large changes
  there, and always verify the file tail compiles after editing.
- The playground and demo scripts compile the hot TU with exact flag
  sets ("build information"); changing those flags changes test
  semantics — keep `rebuild_hot.sh.in` in sync with the CMake target.
- Kerberos-authed dev boxes expire tickets; `ssh` failing with
  `gssapi-with-mic` means the human needs to `kinit`, not a repo problem.
