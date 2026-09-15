# GNU Make project integration

Status: proposed adapter design. The managed runtime API exists, but the GNU
Make include fragment and publication targets described here are not
implemented yet.

This document is the concrete GNU Make adapter specification for
[managed hot-reload integration](managed-reload-design.md). The managed design
is authoritative for runtime, transaction, identity, compatibility, and
publication semantics. This document is authoritative for the proposed GNU
Make surface. The sibling build-adapter contracts are
[CMake project integration](cmake-integration-design.md),
[GN project integration](gn-integration-design.md), and
[Meson project integration](meson-integration-design.md).

The key words **MUST**, **MUST NOT**, **SHOULD**, **SHOULD NOT**, and **MAY**
express requirements on the proposed design.

## 1. Scope

This adapter supports directly authored GNU Make projects. It is not a generic
interface for every Make dialect:

- CMake's `Unix Makefiles` and `NMake Makefiles` generators belong to the CMake
  adapter; users continue to invoke them through `cmake --build`;
- BSD make and other make implementations are not implicitly supported;
- direct NMake projects are not part of this contract;
- native MSBuild projects require a separate `.props`/`.targets` integration
  after a PE/PDB runtime backend exists.

The supported GNU Make version and host/target combinations MUST be recorded by
the implementation and exercised in CI. A project MUST NOT be described as
supported merely because its Makefile happens to accept the syntax below.

## 2. Intended developer experience

An installed package provides `nekomata.mk`. A simple multi-TU project declares
one group and uses the generated baseline link inputs:

```make
nekomata_prefix ?= /opt/nekomata
include $(nekomata_prefix)/share/nekomata/nekomata.mk

gameplay_hot_nekomata_sources := \
  src/gameplay/ball.cpp \
  src/gameplay/gravity.cpp
gameplay_hot_nekomata_cppflags := $(CPPFLAGS) -Iinclude
gameplay_hot_nekomata_cxxflags := $(CXXFLAGS) -std=c++20
$(eval $(call nekomata_reload_group,gameplay_hot))

game: build/main.o $(gameplay_hot_nekomata_link_inputs)
	$(CXX) $(LDFLAGS) -o $@ $^ $(nekomata_elf_ldlibs) $(LDLIBS)
```

The runtime code is independent of TU count and build system:

```cpp
#include <neko/neko.hpp>
#include <neko/platforms/elf.hpp>

int main() {
  neko::reload_session session{neko::elf::create_backend()};
  session.watch();

  while (running()) {
    finish_reloadable_jobs();
    report_reload_events(session.update());
    start_next_frame();
  }
}
```

Normal linking and publication remain ordinary Make targets:

```sh
make game
make gameplay_hot_reload
```

Application code never names sources, objects, depfiles, manifests, or ready
markers. The Makefile declares sources once for the adapter; it does not contain
a handwritten publication recipe.

## 3. Installed Make contract

The installed `nekomata.mk` file is public build-system API. It exports:

- `nekomata_reload_unit`, a GNU Make macro for one coherent compile setup;
- `nekomata_reload_group`, a macro for one atomic publication boundary;
- backend-specific link variables such as `nekomata_elf_ldlibs`.

Its public configuration variables are:

- `nekomata_build_root`, the adapter-owned object, descriptor, and generation
  root;
- `<name>_nekomata_sources` or `<name>_nekomata_units`;
- `<name>_nekomata_group_id` for an explicit stable group ID;
- `<name>_nekomata_cc` and `<name>_nekomata_cxx`;
- `<name>_nekomata_cppflags`, `<name>_nekomata_cflags`, and
  `<name>_nekomata_cxxflags`.

In names above, `<name>` is replaced by the unit or group name passed to the
macro; angle brackets are not Make syntax. The implementation MUST specify the
default `nekomata_build_root` and MUST keep every generated file beneath it.

The publisher program invoked by generated recipes is private implementation
machinery, not a supported user-facing CLI. Its command-line syntax and path may
change without compatibility guarantees.

For a group named `gameplay_hot`, the group macro defines:

- `gameplay_hot_nekomata_objects`: the ordered native object set;
- `gameplay_hot_nekomata_descriptor`: the baseline descriptor object;
- `gameplay_hot_nekomata_link_inputs`: objects plus descriptor for the final
  executable link;
- `gameplay_hot_reload`: the public publication target.

Names with the `nekomata_internal_` prefix are reserved. A consuming Makefile
MUST NOT depend on them.

## 4. Single-TU project

```make
include third_party/nekomata/nekomata.mk

tick_hot_nekomata_sources := src/tick.cpp
tick_hot_nekomata_cxxflags := $(CXXFLAGS) -std=c++20
$(eval $(call nekomata_reload_group,tick_hot))

demo: build/main.o $(tick_hot_nekomata_link_inputs)
	$(CXX) $(LDFLAGS) -o $@ $^ $(nekomata_elf_ldlibs) $(LDLIBS)
```

This is a normal group with one member. `tick_hot_reload` rebuilds that member
when necessary and publishes one complete one-object generation.

## 5. Simple multi-TU project

```make
include third_party/nekomata/nekomata.mk

gameplay_hot_nekomata_sources := \
  src/gameplay/ball.cpp \
  src/gameplay/gravity.cpp
gameplay_hot_nekomata_cppflags := $(CPPFLAGS) -Iinclude
gameplay_hot_nekomata_cxxflags := $(CXXFLAGS) -std=c++20
$(eval $(call nekomata_reload_group,gameplay_hot))

game: build/main.o $(gameplay_hot_nekomata_link_inputs)
	$(CXX) $(LDFLAGS) -o $@ $^ $(nekomata_elf_ldlibs) $(LDLIBS)
```

The macro owns compilation of its sources. GCC or Clang depfiles cause GNU Make
to rebuild only affected objects, but publication includes the exact current
object set. If only `ball.cpp` rebuilds, the new generation still contains both
`ball.cpp` and `gravity.cpp` objects.

## 6. Heterogeneous multi-TU project

Sources with different compilation requirements use separate units:

```make
include third_party/nekomata/nekomata.mk

physics_hot_nekomata_sources := \
  src/physics/collision.cpp \
  src/physics/world.cpp
physics_hot_nekomata_cppflags := $(CPPFLAGS) -Iinclude
physics_hot_nekomata_cxxflags := $(CXXFLAGS) -std=c++20 \
  -DPHYSICS_PRECISE=1
$(eval $(call nekomata_reload_unit,physics_hot))

gameplay_hot_nekomata_sources := \
  src/gameplay/ball.cpp \
  build/generated/behaviour.cpp
gameplay_hot_nekomata_cppflags := $(CPPFLAGS) -Iinclude
gameplay_hot_nekomata_cxxflags := $(CXXFLAGS) -std=c++23
$(eval $(call nekomata_reload_unit,gameplay_hot))

gameplay_group_nekomata_units := physics_hot gameplay_hot
$(eval $(call nekomata_reload_group,gameplay_group))

build/generated/behaviour.cpp: data/behaviour.json build/behaviour_codegen
	build/behaviour_codegen $@ $<

game: build/main.o $(gameplay_group_nekomata_link_inputs)
	$(CXX) $(LDFLAGS) -o $@ $^ $(nekomata_elf_ldlibs) $(LDLIBS)
```

`gameplay_group_reload` waits for both units and publishes one transaction. If
physics and gameplay may commit independently, the build declares two groups.

A group specifies exactly one of `<group>_nekomata_sources` and
`<group>_nekomata_units`. Every unit contains at least one source, has one
coherent compiler and flag set, and belongs to exactly one linked group.

## 7. Native dependency ownership

The adapter owns the compile recipes for sources declared through these macros.
The same object files feed both the baseline executable and later publication.
This is the invariant that prevents compile-command drift.

The adapter MUST NOT:

- introspect or scrape an arbitrary pre-existing recipe;
- derive flags from command output or a compilation database;
- infer objects by scanning a build directory;
- attach to a prebuilt archive and assume it represents exact membership;
- ask the application to repeat object paths for publication.

Wrapping arbitrary existing object recipes is not part of the initial contract.
A project migrates reloadable sources into adapter-owned units. If that cannot
preserve the original compilation semantics, the integration fails explicitly
instead of silently building a second version of the TU.

GNU Make's conventional `CC`, `CXX`, `CPPFLAGS`, `CFLAGS`, and `CXXFLAGS`
variables provide defaults. If a corresponding group- or unit-specific
lowercase variable is defined, its value is the complete effective value; it
is not appended to the conventional variable a second time. A caller that
wants both writes, for example:

```make
demo_hot_nekomata_cxxflags := $(CXXFLAGS) -std=c++20
```

The macro snapshots all inputs at `eval` time; they MUST be assigned before the
macro call. Exactly those captured values drive both compilation and
compatibility identity. C sources use the captured C compiler and flags; C++
sources use the captured C++ compiler and flags. Unsupported source languages
are rejected during Makefile evaluation.

Changing a compiler, a compatibility-relevant flag, or group membership MUST
invalidate the affected objects and descriptor. Because GNU Make does not
naturally rebuild a target when a variable's value changes, the adapter MUST
materialize and depend on a deterministic command/configuration stamp.

## 8. Header and generated dependencies

The initial adapter supports GCC- and Clang-style depfile emission. Generated
compile recipes write depfiles and `nekomata.mk` includes them through ordinary
GNU Make semantics. The runtime does not parse those depfiles in the managed
path.

Generated sources and headers use ordinary Make prerequisites. For example:

```make
build/generated/config.hpp: config/schema.json tools/generate_config
	tools/generate_config $@ $<

$(gameplay_hot_nekomata_objects): build/generated/config.hpp
```

The adapter MUST preserve explicit prerequisites and order-only prerequisites
needed by the native compilation graph. It MUST NOT introduce a second
Nekomata-specific list of header dependencies.

MSVC dependency reporting and NMake syntax are outside this GNU Make adapter.
They do not block CMake-generated NMake or Visual Studio project support, whose
dependency edges are owned by CMake.

## 9. Descriptor and publication rules

The baseline link inputs contain a retained group descriptor with the exact
ordered membership, compatibility identity, ABI identity, publication key, and
baseline sequence. Descriptor retention MUST survive dead stripping and every
claimed supported LTO mode.

The macro creates a file-producing publication stamp that depends on:

- every exact object in the group;
- its configuration/command stamp;
- descriptor and membership inputs;
- the runnable host publisher.

The public `<group>_reload` target is phony and depends on that stamp. A rebuild
of one object makes the stamp stale. If no object or compatibility input
changes, invoking `<group>_reload` again MUST NOT publish another generation.

After prerequisites succeed, the private publisher stages all current group
objects, verifies membership and hashes, releases one immutable generation, and
then atomically publishes its unique ready offer. A failed compile, generator,
or publisher leaves no discoverable partial generation.

Concurrent GNU Make processes may target the same group. The publisher MUST
serialize sequence allocation and release with the stream lock; Make's own job
coordination does not protect independent processes.

## 10. Identity and build directories

By default, `group_id` derives from a normalized project-relative group
identity, never from an absolute source or build path. A project may set
`<group>_nekomata_group_id` before macro expansion when it requires a durable
explicit ID. Duplicate IDs linked into one process are errors.

Debug, release, sanitizer, architecture, and other incompatible variants MUST
use distinct object roots, generation streams, and compatibility identities.
The adapter provides an overridable build-root variable and does not assume all
projects use a directory named `build`.

Cross-compilation distinguishes the target compiler from the runnable host
publisher. If the host publisher is absent, Make fails before publication; it
MUST NOT try to execute a target binary or fall back to guessed paths.

GNU Make represents many inputs as whitespace-separated words. The initial
adapter MUST either implement and test a documented escaping grammar for spaces
and special Make characters or reject unsupported paths during macro expansion.
It MUST NOT silently split or reinterpret a source, object, or build path.

## 11. Multithreaded applications and TUI

GNU Make integration changes build ownership, not runtime synchronization.
Application code calls `update()` only after it has established a safe point;
the caller externally serializes all public session operations.

The optional TUI may request an update. It does not invoke Make and MUST NOT
commit from its input or rendering thread. The host observes the request,
establishes the same safe point, and calls the same `update()`.

## 12. Failure contract

| Condition | Required behavior |
| --- | --- |
| a group specifies both sources and units | fail during Makefile evaluation |
| a unit is empty, missing, or assigned to conflicting groups | fail during Makefile evaluation |
| compiler or compatibility flags change | rebuild affected outputs and change compatibility identity |
| compile or generated-source recipe fails | publish no generation |
| an exact object path cannot be derived by the adapter | fail before publication |
| a path cannot be represented safely | reject it during macro expansion |
| host publisher is unavailable | fail the reload target |
| stream lock cannot be acquired | fail publication |
| publication is interrupted before release | ignore staging data |
| manifest is incomplete or incompatible | return a rejected event |
| one object fails cross-TU validation | reject the whole group |
| commit write fails | restore all writes and reject |
| unknown runtime group ID | throw a configuration exception |

Build failures remain build failures. Published-artifact rejections are
reported through `update_result`; they do not partially change active code.

## 13. Acceptance criteria

GNU Make integration is not complete until automated tests demonstrate:

- the documented minimum GNU Make version;
- GCC and Clang compilation on supported targets;
- single-TU, simple multi-TU, and heterogeneous multi-TU projects;
- one native compile edge feeding baseline and generation publication;
- no application-side path, manifest, depfile, or compiler configuration;
- ordinary header, generated-source, and explicit prerequisite rebuilds;
- exact full-group publication after a one-TU change;
- compiler and flag changes rebuilding outputs and compatibility identity;
- exact member order and duplicate/missing-member rejection;
- cross-TU reference resolution and atomic rollback;
- descriptor retention under dead stripping and claimed LTO modes;
- parallel `make -j` builds and two concurrent publisher processes;
- no-op rebuilds publishing no new generation;
- interrupted publication producing no offer;
- two processes consuming one immutable offer independently;
- repeated `watch()` and `unwatch()` preserving each consumer cursor;
- debug and every claimed optimized setup;
- host/target separation in a supported cross-compilation setup;
- the documented path grammar, including early rejection of unsupported paths;
- no handwritten reload shell script or supported driver CLI.

## 14. Current repository gap

The runtime currently discovers embedded ELF group descriptors, consumes
immutable generation streams, exposes managed `watch()`/`unwatch()`, and
reports structured update events. The repository does not yet install
`nekomata.mk`, provide the GNU Make macros above, or connect native Make targets
to the private generation publisher.

Implementation should begin with the `SOURCES` form and a real GNU Make fixture
using GCC and Clang depfiles. The first proof must show that one adapter-owned
object set feeds baseline linking and complete publication under serial and
parallel builds before heterogeneous units are added.
