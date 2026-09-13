# Reload model

> Status: design direction. The current public API implements only part of this
> model; this document defines the intended boundaries for upcoming work.

## Small integration surface

The common path remains deliberately small:

```cpp
neko::reload_session session{neko::elf::create_backend()};
session.watch("build/hot.new.o", "src/hot.cpp");

for (;;) {
  run_application_work();
  session.update(); // The application chooses this safe point.
}
```

Additional API should support lifecycle, structured results and observation
without turning Nekomata into the owner of the application loop.

## Responsibilities

| Component | Owns | Does not own |
|---|---|---|
| Application | Compilation policy, artifact publication, serialization of session calls, thread quiescence and the decision to call `update()` | Parsing, relocation, patch writes or rollback |
| `reload_session` | Claiming complete offers, validation, preparation, transactional commit, rollback and structured outcomes | Starting, pausing or coordinating application threads |
| TUI | User intent, rendering immutable snapshots and presenting update results | Calling `update()` from its background thread or deciding when the process is safe to patch |

All `reload_session` member calls are externally serialized. Before
`update()`, the application must ensure that no thread can enter or execute
reloadable code, and it must preserve that quiescent state until the call
returns.

This is a precondition, not an operation Nekomata can infer from an arbitrary
host program. The library remains responsible for transactional integrity after
the precondition is met.

## Complete build generations

A multi-TU reload uses a generation manifest rather than relying on unrelated
files happening to appear during the same poll:

```cpp
neko::reload_session session{neko::elf::create_backend()};
session.watch(neko::generation_watch{"build/nekomata/generation.ready"});
```

The watched manifest is itself the ready marker. It has this line-oriented,
versioned format:

```text
nekomata-generation-v1
id "2026-09-13T01:42:18Z-17"
changed "/project/include/ui.hpp"
object "generations/17/widget.o" "/project/src/widget.cpp" "-std=c++20 -O0 -g"
object "generations/17/view.o" "/project/src/view.cpp" "-std=c++20 -O0 -g"
```

- Every value is a C++-style quoted string; escapes and spaces are supported.
- Relative object and source paths are resolved against the manifest directory.
- `id` identifies one build attempt and must not be reused after it applies.
- Each `changed` row records an input that triggered the build.
- Each `object` row records an immutable object, its translation-unit source
  identity, and its complete build-information string.
- Blank lines and lines whose first non-space character is `#` are ignored.

The build integration writes every object to a generation-specific path, closes
the files, writes the manifest to a staging path on the same filesystem, and
renames that staging file to the watched path. It must never modify a listed
object after publication. Nekomata atomically claims and consumes only the
manifest; it reads listed objects without renaming or deleting them.

Nekomata claims and prepares the complete generation, validates every affected
function, detects conflicting replacements, and commits the generation
all-or-nothing. An incomplete or rejected generation must never leak a subset
of its redirects into the running process. A rejected marker is consumed, but
its immutable artifacts remain available for diagnostics, and the producer may
republish the corrected attempt with the same ID. Once an ID applies, publishing
it again is rejected.

The configured patch planner must produce exactly the source identities listed
by the manifest. The default planner treats every `changed` path as one
translation unit, which covers direct source changes. GCC and Clang builds can
expand header changes through their GNU Make-compatible dependency files:

```cpp
auto planner = std::make_shared<neko::depfile_planner>(
    std::vector<neko::depfile_entry>{
        {"src/widget.cpp", "build/widget.d", "/project"},
        {"src/view.cpp", "build/view.d", "/project"},
    });

auto backend = neko::elf::create_backend();
backend.planner = planner;
neko::reload_session session{std::move(backend)};
session.watch(neko::generation_watch{"build/nekomata/generation.ready"});
```

Compile each registered translation unit with dependency output enabled, for
example `-MMD -MP -MF build/widget.next.d`. The planner reads every registered
file when planning and returns all translation units that depend on any
`changed` path, in registration order. Relative translation-unit and depfile
paths use the entry's compilation working directory. Missing or malformed
files reject planning rather than silently omitting a translation unit.

The configured `.d` files are the active snapshot used to plan and validate a
generation. A build writes its next dependency files to staging or
generation-specific paths, publishes the generation, waits until that manifest
has been consumed, and only then atomically promotes the next dependency
snapshot. Keeping the prior graph through validation matters when an edit
removes the include edge that triggered the build.

This parser deliberately supports the GNU Make depfile format emitted by GCC
and Clang; dependency output is not compiler-universal. MSVC exposes include
information through `/showIncludes` or `/sourceDependencies` JSON, which will
require a separate provider adapter rather than pretending those formats are
`.d` files.

Independent watch targets may still produce independent generations. Their
results must remain distinguishable rather than being collapsed into one
boolean.

## Preparation and commit

File I/O, object parsing, dependency expansion, symbol matching, relocation,
code allocation and validation do not need to consume the caller's quiescent
window. The implementation should separate these operations from the final
entry redirects even if `update()` remains the one-call convenience API.

The target flow is:

1. Detect and claim a complete generation.
2. Prepare and validate it while application threads continue running.
3. Report that an update is ready.
4. At a caller-selected quiescent point, redirect all entries transactionally.
5. Publish a structured applied or rejected result.

A future explicit preparation API may expose steps 1–3 when an application
needs a very short pause. It should complement, not replace, the simple
`watch()` plus `update()` path.

## API direction

The intended supporting types are:

- a stable `watch_handle` returned by `watch()`;
- `unwatch(watch_handle)` for registration lifecycle;
- an `update_result` containing per-generation applied or rejected events;
- stable error codes alongside human-readable rejection messages;
- a `session_snapshot` for observers such as the TUI.

Rejected build artifacts are normal hot-reload outcomes and should be
representable without forcing an exception through every frame loop. Exceptions
remain appropriate for programming errors, resource exhaustion and violated
internal invariants.

Exact type layouts are not frozen here. The important contract is that idle,
applied and rejected outcomes are machine-readable, and that multiple
independent generations cannot be misrepresented by one boolean.

## Templates and optimized code

Template support builds on the multi-TU generation model. A template instance
must be identified by concrete symbol identity, linkage, source ownership and
COMDAT membership. A changed template header must pull every affected
translation unit into the same generation.

Optimized builds extend the same dependency problem. Redirecting an out-of-line
entry is insufficient when old behavior has been inlined, constant-propagated,
folded or optimized away in callers. Accepting an optimized generation requires
either replacing every observable copy or conservatively rejecting it.

Restricted optimized configurations, such as explicitly non-inlined reloadable
functions, may be supported earlier, but must not be presented as complete
`-O2` support.

## TUI integration

The TUI has two policies:

- automatic: the application calls `update()` at each suitable safe point;
- manual: the TUI records a reload request and the application consumes it at
  the next suitable safe point.

The manual control flow is:

```cpp
if (tui.consume_reload_request()) {
  const auto result = session.update();
  tui.publish(result);
}

tui.publish(session.snapshot());
```

The current monitor still calls `update()` and reads session statistics from
its background thread. That prototype behavior does not satisfy this contract
and must be replaced before TUI reload control is multithread-safe.

The TUI input/render thread communicates through a thread-safe request channel
and immutable snapshots. It does not retain mutable control of the session.
This keeps exactly one owner for safe-point policy: the host application.
