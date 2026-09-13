# Roadmap

Planned work, in order, with the acceptance bar for each step. Nothing here is
a promise about dates; it is a list of what is deliberately not built yet, and
why the order is what it is. The intended runtime API and TUI responsibility
split are described in [reload-model.md](reload-model.md).

This file is the only place the project's internal phase vocabulary appears —
CONTRIBUTING.md keeps it out of commit messages, comments and user-facing
strings, where a reader has no way to map the numbered milestones.

| Phase | Scope | Acceptance bar | Status |
|---|---|---|---|
| 1 · Single-function prototype | Linux/ELF, `-O0`, one TU, fixed-moment swap | State survives a transactional function-entry redirect | ✅ done |
| 2 · Multi-TU projects | Dependency discovery and complete build generations | Every affected TU is prepared, validated and committed or rejected as one generation | ✅ done |
| 3 · Multithreaded applications | Caller-coordinated quiescent points | Expensive preparation happens before the pause; entry redirects happen only while the caller keeps reloadable code quiescent | ⏳ |
| 4 · Template code | Instantiations, linkage, COMDAT groups and header fan-out | Concrete template instances are identified without guessing and every affected TU joins the generation | ⏳ |
| 5 · Optimized builds | `-O2`, inline units and optimized-away bodies | Inlined and out-of-line copies cannot leave observable stale behavior after an accepted generation | ⏳ high risk |
| 6 · TUI reload integration | Manual and automatic update policy, immutable snapshots and structured results | The TUI requests reloads and renders outcomes without calling the session from its background thread | ⏳ |
| Later · Windows | PE/PDB (DIA), MSVC + `/hotpatch` | The runtime contract above has a Windows backend | ⏳ |
| Later · Class layout migration | Object migration and vtable updates | Live instances migrate without mixed layouts or stale vtables | ⏳ hardest |
