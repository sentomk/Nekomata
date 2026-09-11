# Roadmap

Planned work, in order, with the acceptance bar for each step. Nothing here is
a promise about dates; it is a list of what is deliberately not built yet, and
why the order is what it is.

This file is the only place the project's internal phase vocabulary appears —
CONTRIBUTING.md keeps it out of commit messages, comments and user-facing
strings, where a reader has no way to map "Phase 2" to anything.

| Phase | Scope | Status |
|---|---|---|
| 1 · Single-function prototype | Linux/ELF, `-O0`, one TU, fixed-moment swap | ✅ done |
| 2 · Real-world usable | DWARF ranges, whole-TU reloads, dependency graph, safe points, PIE | ⏳ next |
| 3 · Windows | PE/PDB (DIA), MSVC + `/hotpatch` | ⏳ |
| 4 · Optimized builds | `-O2` inline units (`DW_TAG_inlined_subroutine`), COMDAT folding | ⏳ high risk |
| 5 · Class layout migration | object migration + vtable updates | ⏳ hardest |
