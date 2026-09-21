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

A multi-TU reload no longer watches loose manifests. Build integration
publishes managed generations through the publisher, and the session consumes
them per embedded reload group:

```cpp
neko::reload_session session{neko::elf::create_backend()};
session.watch(); // every reload group embedded in this program
```

Each generation is one immutable object set described by the managed
`nekomata-generation/2` manifest and published behind a `.ready` marker: the
producer writes every object, closes them, and renames the marker last.
Nekomata prepares the complete generation, validates every affected function,
detects conflicting replacements, and commits the generation all-or-nothing.
An incomplete or rejected generation must never leak a subset of its
redirects into the running process, and a rejected generation leaves the
previous behavior active. See docs/managed-reload-design.md for the manifest
codec, the publication protocol, and per-group consumer cursors.

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
