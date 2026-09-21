# Meson project integration

Status: proposed adapter design with an unresolved delivery prerequisite. The
managed runtime API exists, but the Meson module described here does not.

This document is the concrete Meson adapter specification for
[managed hot-reload integration](managed-reload-design.md). The managed design
is authoritative for runtime, transaction, identity, compatibility, and
publication semantics. This document is authoritative for the proposed Meson
surface. The sibling build-adapter contracts are
[CMake project integration](cmake-integration-design.md),
[GNU Make project integration](make-integration-design.md), and
[GN project integration](gn-integration-design.md).

The key words **MUST**, **MUST NOT**, **SHOULD**, **SHOULD NOT**, and **MAY**
express requirements on the proposed design.

## 1. Delivery prerequisite

Meson's language does not allow a subproject or included build fragment to
define new project functions or methods. A wrap-provided dependency can expose
libraries and variables, but it cannot make this declaration valid:

```meson
nekomata = import('nekomata')
gameplay_hot = nekomata.reload_group(...)
```

That surface requires a real Meson extension module. The delivery mechanism
MUST be supported by Meson itself and MUST be maintainable across every Meson
version Nekomata claims to support. An upstream module, or an official
third-party module mechanism with an equally stable contract, could satisfy
this requirement.

Until that mechanism is agreed and tested, this document is a target contract,
not installation guidance. Nekomata MUST NOT present any of these as an
equivalent supported adapter:

- a `.build` fragment that pretends to define `reload_group()`;
- generated rewrites of the user's source tree;
- a CMake invocation hidden inside Meson;
- per-project `custom_target()` boilerplate;
- a script that scrapes Meson's private build database or output layout.

This boundary follows Meson's documented statement that user-defined functions
and methods are not supported:
<https://mesonbuild.com/Syntax.html#user-defined-functions-and-methods>.

Meson subprojects and `declare_dependency()` remain valid ways to deliver
libraries, but do not solve the callable adapter surface by themselves. See
[Subprojects](https://mesonbuild.com/Subprojects.html) and
[`declare_dependency()`](https://mesonbuild.com/Reference-manual_functions_declare_dependency.html).

## 2. Intended developer experience

Once the prerequisite is satisfied, consider a multithreaded game with this
layout:

```text
game/
  meson.build
  src/
    main.cpp
    gameplay/
      ball.cpp
      gravity.cpp
    physics/
      collision.cpp
      world.cpp
  data/
    behaviour.json
```

`main.cpp` owns the process and job system. Gameplay and physics are
reloadable. The build remains a normal Meson/Ninja build; Nekomata does not
become a second build driver.

A simple multi-TU integration has one Meson declaration:

```meson
project('game', 'cpp', default_options: ['cpp_std=c++20'])

nekomata = import('nekomata')
nekomata_elf_dep = dependency('nekomata-backend-elf')

gameplay_hot = nekomata.reload_group(
  'gameplay_hot',
  sources: files(
    'src/gameplay/ball.cpp',
    'src/gameplay/gravity.cpp',
  ),
  include_directories: include_directories('src'),
)

executable(
  'game',
  'src/main.cpp',
  dependencies: [nekomata_elf_dep, gameplay_hot.dependency()],
)
```

The application integration is independent of TU count:

```cpp
#include <neko/neko.hpp>
#include <neko/platforms/elf.hpp>

int main() {
  neko::reload_session session{neko::elf::create_backend()};
  session.watch();

  while (running()) {
    finish_game_jobs();
    static_cast<void>(session.update());
    start_next_frame();
  }

  session.unwatch();
}
```

Normal builds use normal commands:

```sh
meson setup build
meson compile -C build game
```

After editing either gameplay source, a developer publishes a generation with:

```sh
meson compile -C build gameplay_hot_reload
```

No source path, object path, depfile path, manifest path, or compiler command
appears in application code.

## 3. User-facing Meson contract

The extension exports two methods:

```meson
unit = nekomata.reload_unit(<name>, ...)
group = nekomata.reload_group(<name>, ...)
```

Each return value is an immutable module holder. A group holder exposes:

```meson
group.dependency()
```

The returned dependency carries the baseline objects, descriptor, usage
requirements, and retention requirements into a native consumer. The public
reload target is named `<group>_reload`; a separate method for retrieving or
renaming it is intentionally absent.

The publisher executable invoked by the module is private implementation
machinery and is not a supported user-facing CLI.

### 3.1 `reload_group`

The simple form owns sources with one coherent compile configuration:

```meson
group = nekomata.reload_group(
  <name>,
  sources: <source-or-generated-source-array>,
  group_id: <optional-stable-id>,
  dependencies: <optional-dependency-array>,
  include_directories: <optional-include-array>,
  cpp_args: <optional-string-array>,
  c_args: <optional-string-array>,
  objects: <optional-object-array>,
)
```

The complex form groups predeclared reload units:

```meson
group = nekomata.reload_group(
  <name>,
  units: [<unit-holder>, ...],
  group_id: <optional-stable-id>,
)
```

Exactly one of `sources` and `units` is required. Mixing them is a configure
error. Compile keywords are invalid in the `units` form; each unit retains its
own configuration.

The keyword allowlist MUST follow Meson's native build-target semantics where
possible. The module MUST reject unsupported keywords rather than silently
discarding or approximately translating them.

### 3.2 `reload_unit`

Heterogeneous groups declare each coherent compile configuration separately:

```meson
unit = nekomata.reload_unit(
  <name>,
  sources: <source-or-generated-source-array>,
  dependencies: <optional-dependency-array>,
  include_directories: <optional-include-array>,
  cpp_args: <optional-string-array>,
  c_args: <optional-string-array>,
  objects: <optional-object-array>,
)
```

A unit is not independently publishable and does not create `<name>_reload`.
A unit MUST belong to exactly one linked reload group in one executable image.

The module MUST construct a native Meson compilation target. It MUST NOT
serialize these arguments into a second compiler command owned by Nekomata.

## 4. Single-TU project

```meson
project('demo', 'cpp', default_options: ['cpp_std=c++20'])

nekomata = import('nekomata')
nekomata_elf_dep = dependency('nekomata-backend-elf')

demo_hot = nekomata.reload_group(
  'demo_hot',
  sources: files('src/tick.cpp'),
)

executable(
  'demo',
  'src/main.cpp',
  dependencies: [nekomata_elf_dep, demo_hot.dependency()],
)
```

This is a reload group with one member, not a separate runtime mode. It follows
the same descriptor, generation, validation, and commit rules as a multi-TU
group.

## 5. Simple multi-TU project

```meson
project('game', 'cpp', default_options: ['cpp_std=c++20'])

nekomata = import('nekomata')
nekomata_elf_dep = dependency('nekomata-backend-elf')
game_options = declare_dependency(
  compile_args: ['-DGAMEPLAY_VERSION=2'],
  include_directories: include_directories('src'),
)

gameplay_hot = nekomata.reload_group(
  'gameplay_hot',
  sources: files(
    'src/gameplay/ball.cpp',
    'src/gameplay/gravity.cpp',
  ),
  dependencies: game_options,
)

executable(
  'game',
  'src/main.cpp',
  dependencies: [nekomata_elf_dep, gameplay_hot.dependency()],
)
```

If a header change rebuilds only `ball.cpp`, the publisher still describes and
publishes the complete current membership of `gameplay_hot`. An unchanged
`gravity.cpp` object comes from the current native build output. Publication is
atomic at the group boundary, not at the individual compiler invocation.

## 6. Heterogeneous multi-TU project

Use units when sources cannot truthfully share one compile target:

```meson
project('game', 'c', 'cpp')

nekomata = import('nekomata')
nekomata_elf_dep = dependency('nekomata-backend-elf')

behaviour_codegen = executable(
  'behaviour_codegen',
  'tools/behaviour_codegen.cpp',
  native: true,
)
generated_behaviour = custom_target(
  'generated_behaviour',
  input: 'data/behaviour.json',
  output: 'behaviour.cpp',
  command: [behaviour_codegen, '@INPUT@', '@OUTPUT@'],
)

physics_hot = nekomata.reload_unit(
  'physics_hot',
  sources: files(
    'src/physics/collision.cpp',
    'src/physics/world.cpp',
  ),
  cpp_args: ['-DPHYSICS_PRECISE=1'],
)

gameplay_hot = nekomata.reload_unit(
  'gameplay_hot',
  sources: [
    'src/gameplay/ball.cpp',
    generated_behaviour,
  ],
  include_directories: include_directories('src'),
)

gameplay_group = nekomata.reload_group(
  'gameplay_group',
  units: [physics_hot, gameplay_hot],
)

executable(
  'game',
  'src/main.cpp',
  dependencies: [nekomata_elf_dep, gameplay_group.dependency()],
)
```

The units retain different definitions and generated dependencies.
`gameplay_group_reload` waits for every unit and publishes one transaction. If
the units may commit independently, declare two groups.

## 7. What the module creates

For every group, the extension creates or generates:

1. native Meson compilation for each reload unit;
2. a descriptor retained in the baseline executable;
3. a publication rule consuming the exact native object set;
4. the public `<group>_reload` target;
5. build-option-specific immutable generations under the build tree.

The descriptor contains the group ID, exact membership identities,
compatibility fingerprint inputs, and a relocatable generation-root locator.
`group.dependency()` contributes both the baseline code and descriptor to the
consumer.

The module MUST ensure descriptor retention under dead stripping and link-time
optimization. Depending on an unreferenced constructor or an ordinarily linked
static archive is not sufficient evidence of retention.

## 8. One native compile graph

The extension MUST construct native Meson build objects and preserve Meson's
compiler, machine, option, dependency, generated-source, and language
semantics. Baseline linking and generation publication consume the same native
object-producing edges.

The module MUST obtain the exact object set through a supported Meson module
API. It MUST NOT:

- reconstruct command lines from `compile_commands.json`;
- infer object names from source names;
- scan the private build directory layout;
- unpack a baseline archive and assume it represents exact membership;
- read or modify Meson's private serialized build state.

If no supported extension API exposes the object set required for publication,
the adapter remains blocked. That is a delivery constraint, not permission to
introduce a second compiler driver.

## 9. Publication target

`<group>_reload` is a normal named Meson build target backed by a native custom
target constructed by the module. It depends on:

- every native object-producing unit;
- generated source producers;
- descriptor and compatibility inputs;
- the runnable host publisher.

The publisher receives an ordered object list and generated protocol inputs
directly from the module. It stages an immutable generation, validates
membership, and releases it only after every file is durable. A failed compile,
generator, or publisher command produces no discoverable offer.

Building `<group>_reload` repeatedly without changed effective inputs MAY be a
no-op. A successfully released generation identifier MUST never be reused for
different bytes.

## 10. Build options and backend identity

Meson build options, compiler identity, target machine, language standard,
instrumentation, PIC mode, visibility, and relevant link settings contribute
to the compatibility fingerprint.

Separate build directories naturally have separate generation roots. Changing
a compatibility-relevant option requires a new generation stream and usually a
baseline restart. The module MUST NOT publish a generation whose identity does
not match the linked descriptor.

Debug and every claimed optimized setup require tests. Supporting `-O2` means
the adapter and runtime pass those tests; it does not mean arbitrary optimizer
transformations are reload-safe.

## 11. Dependencies and generated sources

Meson/Ninja remains authoritative for dependency expansion. Header changes,
generated sources, and code generators use the normal target graph. Nekomata
does not parse compiler depfiles in the managed Meson path.

The module accepts Meson dependency objects and generated source objects. It
MUST preserve their ordering and machine semantics. A generated source is not
ready until its producing target succeeds; the reload target then waits for the
native compilation that consumes it.

The module MUST reject a host-generated source tool accidentally built for the
target machine when cross-compiling, unless Meson can execute it through an
explicit supported wrapper.

## 12. Group identity

By default, `group_id` is derived from the normalized subdirectory-relative
Meson target identity, not from an absolute build path. It MUST be stable across
build-directory moves and distinct for same-named groups in different source
subdirectories.

`group_id` overrides the default when an application needs a durable identity:

```meson
gameplay_hot = nekomata.reload_group(
  'gameplay_hot',
  group_id: 'com.example.game.gameplay',
  sources: files(
    'src/gameplay/ball.cpp',
    'src/gameplay/gravity.cpp',
  ),
)
```

Duplicate group IDs linked into one image are a configuration error. The
runtime also rejects duplicate descriptors defensively.

## 13. Multithreaded applications and TUI

Meson integration changes build ownership, not runtime synchronization.
Application threads may execute reloadable code while a worker prepares a
candidate. Only the host calls `update()` after establishing its safe point.

```cpp
while (running()) {
  jobs.stop_accepting();
  jobs.join_reloadable_work();

  const neko::update_result result = session.update();
  ui.record_reload_result(result);

  jobs.resume();
}
```

The optional TUI may request an update. It MUST NOT commit from its input or
render thread. The host observes the request and calls the same `update()` at
the same safe point.

## 14. Cross-compilation

Reload units and the backend are target artifacts. The publisher and build-time
generators are host tools. The module MUST model that distinction through
Meson's native/host and target-machine concepts.

The library may be provided by a system dependency or a fallback subproject.
In both cases, the module contract and compatibility identity MUST be the same.
A fallback MAY provide the Nekomata libraries and host publisher, but cannot by
itself provide the callable `import('nekomata')` surface described in section 1.

If no runnable host publisher is available, setup MUST fail with a precise
diagnostic. Silently disabling publication is forbidden.

## 15. Migration of an existing target

Start with an ordinary static library:

```meson
gameplay_lib = static_library(
  'gameplay',
  files(
    'src/gameplay/ball.cpp',
    'src/gameplay/gravity.cpp',
  ),
  include_directories: include_directories('src'),
  cpp_args: ['-DGAMEPLAY_VERSION=2'],
)

executable('game', 'src/main.cpp', link_with: gameplay_lib)
```

Replace the declaration and consume the returned dependency:

```meson
nekomata = import('nekomata')
nekomata_elf_dep = dependency('nekomata-backend-elf')

gameplay = nekomata.reload_group(
  'gameplay',
  sources: files(
    'src/gameplay/ball.cpp',
    'src/gameplay/gravity.cpp',
  ),
  include_directories: include_directories('src'),
  cpp_args: ['-DGAMEPLAY_VERSION=2'],
)

executable(
  'game',
  'src/main.cpp',
  dependencies: [nekomata_elf_dep, gameplay.dependency()],
)
```

Migration changes target ownership but does not add an object list, manifest,
depfile path, compiler command, or rebuild script.

If an existing target uses a Meson feature outside the module's tested keyword
allowlist, setup MUST fail with the unsupported feature named. The module MUST
NOT silently alter include propagation, definitions, link ordering, language
mode, PIC, visibility, optimization, or machine selection.

## 16. Failure contract

| Condition | Required behavior |
| --- | --- |
| extension module cannot be loaded | fail `meson setup` |
| group specifies both `sources` and `units` | fail `meson setup` |
| a unit is missing or belongs to conflicting groups | fail `meson setup` |
| a keyword cannot be represented exactly | fail `meson setup` |
| duplicate linked group ID | fail setup or final link |
| compile or generator target fails | publish no generation |
| exact native object set is unavailable | fail `meson setup` or reload target |
| host publisher is unavailable | fail `meson setup` |
| descriptor cannot be retained | fail the final link |
| publication is interrupted before release | ignore staging data |
| manifest is incomplete or incompatible | return a rejected event |
| one object fails cross-TU validation | reject the whole group |
| commit write fails | restore all writes and reject |
| unknown runtime group ID | throw a configuration exception |
| `update()` has no enabled group | return an empty result |

Build failures remain build failures. Published-artifact rejections are
reported through `update_result`. Fatal session and programming errors use the
exception path defined by the managed design.

## 17. Acceptance criteria

Meson integration is not complete until automated tests demonstrate:

- module discovery through the chosen supported distribution mechanism;
- single-TU, simple multi-TU, and heterogeneous multi-TU projects;
- one native compile edge feeding baseline and generation publication;
- no application-side path, manifest, depfile, or compiler configuration;
- ordinary header, generated-source, and dependency-object rebuilds;
- exact full-group publication after a one-TU change;
- cross-TU reference resolution and atomic rollback;
- stable default group IDs in nested subdirectories;
- explicit group-ID override and duplicate-ID rejection;
- descriptor retention under dead stripping and supported LTO modes;
- debug and every claimed optimized setup;
- system dependency and fallback-subproject library delivery;
- native and supported cross-compilation builds;
- source and build paths containing spaces;
- long object-list and response-file handling;
- publication failure producing no offer;
- two processes consuming one immutable offer;
- moved generation-root override;
- repeated `watch()` and `unwatch()` preserving the cursor;
- later valid generation recovery after rejection;
- TUI requests committing only through the host safe point;
- migration from an existing target without a parallel build script.

## 18. Current repository gap

The repository does not currently provide a Meson extension module,
`reload_unit()`, `reload_group()`, or the Meson publication targets. The
runtime already discovers embedded ELF descriptors and provides managed
`watch()`/`unwatch()` with structured update events.

The first implementation task is not publisher plumbing. It is a proof that a
supported Meson extension mechanism can be distributed and can obtain the exact
native object set without depending on Meson internals. If that proof fails,
the proposed surface must be revised explicitly.

Current object watches and handwritten
demo rebuild scripts are implementation and compatibility mechanisms. They
must not be presented as satisfying this Meson contract.
