# GN project integration

Status: proposed adapter design. The managed runtime API exists, but the GN
templates and publication targets described here are not implemented yet.

This document is the concrete GN adapter specification for
[managed hot-reload integration](managed-reload-design.md). The managed design
is authoritative for runtime, transaction, identity, compatibility, and
publication semantics. This document is authoritative for the proposed GN
surface. A change to either document that violates the other requires an
explicit design decision, not an incidental implementation workaround.

The sibling build-adapter contracts are
[CMake project integration](cmake-integration-design.md),
[GNU Make project integration](make-integration-design.md), and
[Meson project integration](meson-integration-design.md).

The key words **MUST**, **MUST NOT**, **SHOULD**, **SHOULD NOT**, and **MAY**
express requirements on the proposed design.

## 1. Intended developer experience

Consider a multithreaded game with this layout:

```text
game/
  BUILD.gn
  main.cpp
  gameplay/
    ball.cpp
    gravity.cpp
  physics/
    collision.cpp
    world.cpp
  generated/
    behaviour.cpp
```

`main.cpp` owns the process and job system. Gameplay and physics are reloadable.
The build remains a normal GN/Ninja build; Nekomata does not become a second
build driver.

A simple multi-TU integration has one GN declaration:

```gn
import("//third_party/nekomata/nekomata.gni")

nekomata_reload_group("gameplay_hot") {
  sources = [
    "gameplay/ball.cpp",
    "gameplay/gravity.cpp",
  ]

  configs = [ ":gameplay_config" ]
  deps = [ ":generate_gameplay_data" ]
}

executable("game") {
  sources = [ "main.cpp" ]

  deps = [
    ":gameplay_hot",
    "//third_party/nekomata:backend_elf",
  ]
}
```

The application integration is independent of TU count:

```cpp
#include <neko/neko.hpp>
#include <neko/platforms/elf.hpp>

int main() {
  neko::reload_session session{neko::elf::create_backend()};
  session.watch();

  while (running) {
    run_frame();

    auto reload_barrier = scheduler.quiesce_reloadable_jobs();
    const neko::update_result result = session.update();
    report_reload_events(result);
  }

  session.unwatch();
}
```

There are no object paths, depfile paths, manifests, marker paths, or compiler
commands in application code.

## 2. User-facing GN contract

The adapter exports two templates:

```gn
nekomata_reload_unit(target_name) {
  # Native compile boundary.
}

nekomata_reload_group(target_name) {
  # Atomic publication and runtime commit boundary.
}
```

For a group named `gameplay_hot`, the adapter creates these public targets:

- `:gameplay_hot` supplies baseline code and its embedded descriptor to
  dependents;
- `:gameplay_hot_reload` rebuilds native dependencies and publishes one
  complete generation.

Names reserved for adapter internals are not public and MUST NOT be referenced
by consuming projects.

`nekomata_reload_group()` supports exactly one of these forms:

```gn
nekomata_reload_group("single_configuration_group") {
  sources = [ ... ]
  # Compile variables are forwarded to one implicit reload unit.
}
```

```gn
nekomata_reload_group("heterogeneous_group") {
  units = [
    ":first_unit",
    ":second_unit",
  ]
}
```

`sources` and `units` are mutually exclusive. An empty group is rejected during
`gn gen`.

`nekomata_reload_unit()` accepts native compile inputs and settings.
`nekomata_reload_group()` in its `sources` form accepts the same settings.
The supported forwarding allowlist includes at least:

- `sources`, `public`, `inputs`;
- `deps`, `public_deps`, `data_deps`;
- `configs`, `public_configs`, `all_dependent_configs`;
- `defines`, `include_dirs`;
- `cflags`, `cflags_c`, `cflags_cc`;
- `visibility`, `testonly`, and `friend`.

The adapter MUST explicitly define and test this allowlist. It MUST NOT blindly
forward every invoker variable into an internal target. Unsupported variables
fail during `gn gen` with a message naming the variable and supported
alternative.

A reload unit may belong to only one reload group linked into a given
executable. Sharing it between active groups would give two transactions
ownership of the same patch targets and is rejected.

## 3. Normal build and reload flow

The initial build uses the existing workflow:

```sh
gn gen out/debug
autoninja -C out/debug game:game
```

The adapter ensures that `:gameplay_hot` contributes:

- the baseline objects compiled by the native target;
- one generated descriptor retained by the final link.

The normal executable build does not publish a reload offer.

After editing `gameplay/ball.cpp`, the developer runs:

```sh
autoninja -C out/debug game:gameplay_hot_reload
```

That target performs this sequence:

1. Ninja rebuilds every object affected by its native dependency graph.
2. The adapter waits until every reload-unit output is complete.
3. The publisher materializes every current member of `gameplay_hot`, including
   objects Ninja did not rebuild during this invocation.
4. The publisher writes and validates the immutable generation.
5. The publisher releases one uniquely named ready offer atomically.
6. Each watching process discovers and prepares the generation independently.
7. The next application-controlled `update()` commits the group atomically.

If compilation, code generation, or publication fails, no ready offer appears.
A partially staged directory is never a candidate.

`watch()` does not start Ninja and does not watch source files. It observes
generation streams described by the linked executable.

## 4. Runtime watch overloads

The target runtime surface is:

```cpp
class reload_session {
public:
  void watch();
  void watch(std::string_view group_id);

  void unwatch();
  void unwatch(std::string_view group_id);

  [[nodiscard]] update_result update();
  [[nodiscard]] session_snapshot snapshot() const;
};
```

`watch()` enables every descriptor linked into the process. This is the normal
choice, even when the executable contains several groups: only a group with a
newly published generation has work to prepare.

A project that deliberately wants a subset may use its logical group ID:

```cpp
session.watch("//game:gameplay_hot");
session.watch("//editor:inspector_hot");
```

The matching stop operations are:

```cpp
session.unwatch("//editor:inspector_hot");
session.unwatch();
```

All four operations are idempotent for known groups. An unknown ID is a
configuration error. Disabling a group discards uncommitted prepared work but
does not restore code already applied. Re-enabling resumes from the existing
cursor and does not replay older generations.

The GN label without its default toolchain is the default `group_id`, for
example `//game:gameplay_hot`. A declaration may provide an explicit stable
`group_id`. Two linked descriptors with the same ID and different contents are
a configuration error.

Path-shaped `watch()` overloads are not part of this API. Manual object or
manifest inputs select a low-level `generation_source` when constructing a
session, as specified by the managed design.

## 5. Multithreaded host contract

The preparation worker may discover files, validate objects, resolve symbols,
and relocate a candidate while the game continues to run. It never redirects
active code.

Before `update()`, the host MUST ensure that no thread can enter or remain
inside reloadable code. A real project may use a job-system barrier:

```cpp
while (running) {
  submit_frame_jobs();
  present_frame();

  {
    auto reload_barrier = scheduler.quiesce_reloadable_jobs();
    report_reload_events(session.update());
  }
}
```

The barrier must cover every execution domain that can call the group,
including rendering, audio, scripting, and background workers where
applicable. Merely protecting the thread that calls `update()` is insufficient.

The barrier remains held until `update()` returns. Nekomata owns transaction
rollback; the application owns thread quiescence.

A TUI follows the same rule. Its input thread may enqueue a reload request, but
the host consumes that request and calls `update()` at this safe point.

## 6. Complex project with heterogeneous units

Suppose physics uses different compile settings and gameplay includes generated
code, but both must become visible in the same frame:

```gn
import("//third_party/nekomata/nekomata.gni")

nekomata_reload_unit("physics_hot") {
  sources = [
    "physics/collision.cpp",
    "physics/world.cpp",
  ]

  configs = [ ":physics_config" ]
}

nekomata_reload_unit("gameplay_hot") {
  sources = [
    "gameplay/ball.cpp",
    "gameplay/gravity.cpp",
    "$target_gen_dir/behaviour.cpp",
  ]

  configs = [ ":gameplay_config" ]
  deps = [ ":generate_behaviour" ]
}

nekomata_reload_group("frame_hot") {
  units = [
    ":physics_hot",
    ":gameplay_hot",
  ]
}

executable("game") {
  sources = [ "main.cpp" ]

  deps = [
    ":frame_hot",
    "//third_party/nekomata:backend_elf",
  ]
}
```

`physics_hot` and `gameplay_hot` retain separate native configurations.
`frame_hot` defines one transaction.

If only `ball.cpp` changes, Ninja may compile only that TU. Publication still
contains the current objects of both units. Cross-TU references are resolved
against the complete candidate before any entry is modified.

If physics and gameplay may apply independently, the build declares two groups:

```gn
nekomata_reload_group("physics_group") {
  units = [ ":physics_hot" ]
}

nekomata_reload_group("gameplay_group") {
  units = [ ":gameplay_hot" ]
}
```

This is a semantic choice. Directory structure, existing library boundaries,
and the order of GN declarations do not create atomicity.

## 7. Single-TU project

A single-TU project uses the same group template:

```gn
import("//third_party/nekomata/nekomata.gni")

nekomata_reload_group("tick_hot") {
  sources = [ "tick.cpp" ]
}

executable("demo") {
  sources = [ "main.cpp" ]

  deps = [
    ":tick_hot",
    "//third_party/nekomata:backend_elf",
  ]
}
```

The descriptor has one member, and `tick_hot_reload` publishes a one-object
generation. No single-TU runtime mode or different C++ API exists.

## 8. Native dependency ownership

The adapter MUST own the native compilation target for every reload unit. It
cannot reliably attach after the fact to an arbitrary existing `source_set`.

GN `source_set` objects are propagated implicitly to dependent linker lines,
but `get_target_outputs()` does not expose the object list of binary targets or
source sets. The official reference restricts that function to action, copy,
and generated-file outputs:

<https://gn.googlesource.com/gn/+/main/docs/reference.md>

Consequently, an adapter that merely wraps an existing label cannot obtain a
portable, exact object set. It must not guess output paths or scrape Ninja's
private files.

The adapter may use a deterministic native container, a project toolchain hook,
or another proven GN mechanism internally. Whatever mechanism is selected
MUST satisfy all of these invariants:

- one native compile edge supplies both baseline and reload objects;
- source membership maps exactly to logical object keys;
- generated and ordinary sources are handled identically after compilation;
- object discovery works with every supported compiler and target toolchain;
- the application does not repeat compile flags for reload;
- changing an effective compile property changes `compatibility_id`.

The implementation is not accepted until tests prove these invariants. The
choice of archive, toolchain hook, or equivalent mechanism is not part of the
application-facing API.

## 9. Descriptor generation

The `:gameplay_hot` dependency causes a generated descriptor object to be
linked into the executable. The adapter MUST ensure the linker retains it; it
cannot depend on incidental static-library extraction.

The descriptor records:

- canonical logical group ID;
- exact ordered member keys;
- publication key;
- target and patch ABI identity;
- build compatibility identity;
- baseline publication sequence;
- optional default local generation-root hint.

Physical source, object, and output-directory paths are not logical identity.

The baseline sequence is captured when the executable is linked and serialized
with publication for that stream. A process therefore ignores offers already
represented by its executable but can consume an offer published after linking
and before its call to `watch()`.

The adapter derives compatibility from effective compiler and target
configuration, not from the spelling of the template invocation. It includes
compiler identity, target triple, language mode, relevant resolved configs,
reload instrumentation, and group membership. Normal source and header content
changes do not change compatibility identity.

If the adapter cannot prove that its fingerprint represents the effective
native compile edge, it MUST fail configuration rather than publish an
unverifiable generation.

## 10. Header and generated-file changes

GN and Ninja remain authoritative for dependency expansion. Nekomata does not
parse GN depfiles in the managed path.

For an ordinary header change, the native compiler depfile causes Ninja to
rebuild every affected group TU. The publication target then snapshots every
group member.

Generated sources use normal GN dependencies. When an action is declared
earlier in the same build file, its output may be used directly:

```gn
action("generate_behaviour") {
  script = "generate_behaviour.py"
  sources = [ "behaviour.schema" ]
  outputs = [ "$target_gen_dir/behaviour.cpp" ]
  args = [
    rebase_path(sources[0], root_build_dir),
    rebase_path(outputs[0], root_build_dir),
  ]
}

nekomata_reload_unit("gameplay_hot") {
  sources = get_target_outputs(":generate_behaviour")
  deps = [ ":generate_behaviour" ]
  configs = [ ":gameplay_config" ]
}
```

A generation is not published until the action and resulting compilation both
succeed.

## 11. Toolchains and cross-compilation

Runtime objects use the executable's target toolchain. Publisher and descriptor
generation tools run in an appropriate host toolchain.

The publication key distinguishes target toolchain and compatibility identity,
even when two targets share a logical group ID. A target process rejects an
offer whose ABI identity does not match.

The adapter MUST cover at least:

- default and explicitly selected GN toolchains;
- Clang and GCC target builds on supported Linux configurations;
- host tools built for a different architecture during cross-compilation;
- debug and supported optimized configurations;
- response files and command lines exceeding platform limits.

Host tools must not be linked into the target executable.

## 12. Publication and multiple processes

`<group>_reload` publishes a full immutable generation, not an incremental
object list. Reflink or hardlink use is optional; copying is the required
fallback. A hardlink is valid only when the source inode cannot later be
modified in place.

Each offer has a stream sequence and unique generation ID. Concurrent
publishers for one stream serialize allocation and release. Failed publication
may leave a sequence gap but never a ready offer.

Consumers never rename or delete an offer to claim it. Two running instances
of `game` may discover and apply the same generation using independent
in-process cursors.

## 13. Migration of an existing GN target

An existing target might be:

```gn
source_set("gameplay") {
  sources = [
    "gameplay/ball.cpp",
    "gameplay/gravity.cpp",
  ]
  configs = [ ":gameplay_config" ]
}
```

It becomes:

```gn
nekomata_reload_group("gameplay") {
  sources = [
    "gameplay/ball.cpp",
    "gameplay/gravity.cpp",
  ]
  configs = [ ":gameplay_config" ]
}
```

Consumers continue to depend on `:gameplay`. The adapter preserves the public
label so migration does not require a second target or duplicated settings.

Before conversion, the project must decide:

- whether the whole target is one atomic group;
- whether some sources require distinct reload units;
- which threads can execute its code;
- where the application can establish a safe point;
- whether its current compiler mode is supported by the backend.

The adapter MUST NOT silently change public include propagation, defines,
visibility, or generated-source ordering.

## 14. Failure behavior

| Condition | Required outcome |
| --- | --- |
| group has neither `sources` nor `units` | fail `gn gen` |
| group specifies both `sources` and `units` | fail `gn gen` |
| unit belongs to conflicting linked groups | fail link/configuration |
| forwarded compile variable is unsupported | fail `gn gen` |
| compile or generator action fails | publish no offer |
| exact native object set cannot be proven | fail the reload target |
| descriptor cannot be retained | fail the final link |
| stream lock cannot be acquired | fail publication |
| publication is interrupted before release | ignore staging data |
| manifest is incomplete or incompatible | return a rejected event |
| one object fails cross-TU validation | reject the whole group |
| commit write fails | restore all writes and reject |
| unknown runtime group ID | throw a configuration exception |
| `update()` has no enabled group | return an empty result |

Build failures remain build failures. Published-artifact rejections are
reported through `update_result`. Fatal session and programming errors use the
exception path defined by the managed design.

## 15. Acceptance criteria

GN integration is not complete until automated tests demonstrate:

- single-TU, simple multi-TU, and heterogeneous multi-TU projects;
- one compile configuration feeding baseline and generation objects;
- no application-side path, manifest, or depfile configuration;
- ordinary header and generated-source dependency rebuilds;
- exact full-group publication after a one-TU change;
- cross-TU reference resolution and atomic rollback;
- group IDs derived consistently in nested build directories;
- explicit group-ID override and duplicate-ID rejection;
- descriptor retention under dead stripping;
- debug and every claimed optimized configuration;
- default, non-default, host, and target toolchains;
- response-file handling;
- publication failure producing no offer;
- two processes consuming one immutable offer;
- moved generation-root override;
- repeated `watch()` and `unwatch()` preserving the cursor;
- later valid generation recovery after rejection;
- TUI requests committing only through the host safe point;
- migration from an existing target without duplicated compile settings.

## 16. Current repository gap

The repository does not currently contain `nekomata.gni`,
`nekomata_reload_unit`, `nekomata_reload_group`, or the GN publication targets.
The runtime already discovers embedded ELF descriptors and provides managed
`watch()`/`unwatch()` with structured update events.

Current object watches and handwritten
demo rebuild scripts are implementation and compatibility mechanisms. They
must not be presented as if they already satisfy this GN contract.

Implementation should begin only after the adapter proves how it obtains the
exact native object set and effective compatibility fingerprint without
guessing target paths or maintaining a second compiler command.
