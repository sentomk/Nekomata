# CMake project integration

Status: proposed adapter design. The managed runtime API exists, but the CMake
functions and publication targets described here are not implemented yet.

This document is the concrete CMake adapter specification for
[managed hot-reload integration](managed-reload-design.md). The managed design
is authoritative for runtime, transaction, identity, compatibility, and
publication semantics. This document is authoritative for the proposed CMake
surface. The sibling build-adapter contracts are
[GNU Make project integration](make-integration-design.md),
[GN project integration](gn-integration-design.md), and
[Meson project integration](meson-integration-design.md).

The key words **MUST**, **MUST NOT**, **SHOULD**, **SHOULD NOT**, and **MAY**
express requirements on the proposed design.

## 1. Intended developer experience

Consider a multithreaded game with this layout:

```text
game/
  CMakeLists.txt
  src/
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

`main.cpp` owns the process and job system. Gameplay and physics are
reloadable. The build remains a normal CMake build; Nekomata does not become a
second build driver.

A simple multi-TU integration has one CMake declaration:

```cmake
cmake_minimum_required(VERSION 3.21)
project(game LANGUAGES CXX)

find_package(nekomata CONFIG REQUIRED)

nekomata_add_reload_group(gameplay_hot
  SOURCES
    src/gameplay/ball.cpp
    src/gameplay/gravity.cpp
)
target_compile_features(gameplay_hot PRIVATE cxx_std_20)
target_include_directories(gameplay_hot PRIVATE src)

add_executable(game src/main.cpp)
target_link_libraries(game PRIVATE gameplay_hot nekomata::backends::elf)
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
cmake -S . -B build -G Ninja
cmake --build build --target game
```

After editing either gameplay source, a developer publishes a generation with:

```sh
cmake --build build --target gameplay_hot_reload
```

No source path, object path, depfile path, manifest path, or compiler command
appears in application code.

## 2. User-facing CMake contract

Installing Nekomata exports:

- imported library targets such as `nekomata::backends::elf`;
- `nekomata_add_reload_unit()`;
- `nekomata_add_reload_group()`.

The helper functions are CMake API. The publisher executable they invoke is
private implementation machinery and is not a supported user-facing CLI.

### 2.1 `nekomata_add_reload_group`

The simple form owns sources with one coherent compile configuration:

```cmake
nekomata_add_reload_group(<name>
  SOURCES <source>...
  [GROUP_ID <stable-id>]
)
```

It creates these public build targets:

- `<name>`: the baseline link dependency and compile-property target;
- `<name>_reload`: build changed inputs and publish one complete generation.

`<name>` behaves as an object-bearing native target. Ordinary CMake commands
configure it:

```cmake
target_compile_definitions(gameplay_hot PRIVATE GAMEPLAY_VERSION=2)
target_compile_options(gameplay_hot PRIVATE -fno-omit-frame-pointer)
target_include_directories(gameplay_hot PRIVATE include)
target_link_libraries(gameplay_hot PRIVATE project_warnings)
```

Those properties affect both the baseline image and later reload generations
because both consume the same native compile edges.

The complex form groups predeclared reload units:

```cmake
nekomata_add_reload_group(<name>
  UNITS <unit>...
  [GROUP_ID <stable-id>]
)
```

Exactly one of `SOURCES` and `UNITS` is required. Mixing them is a configure
error. Group-level compile properties in the `UNITS` form are a configure
error; compile each unit through its own target instead.

### 2.2 `nekomata_add_reload_unit`

Heterogeneous groups declare each coherent compile configuration separately:

```cmake
nekomata_add_reload_unit(<name>
  SOURCES <source>...
)
```

The unit is an object-bearing native target. It is not independently
publishable and does not create `<name>_reload`. A unit MUST belong to exactly
one linked reload group in one executable image.

Normal target commands configure each unit. This preserves C/C++ language
settings, generated inputs, definitions, options, include paths, and target
dependencies without copying them into Nekomata-specific arguments.

## 3. Single-TU project

```cmake
find_package(nekomata CONFIG REQUIRED)

nekomata_add_reload_group(demo_hot
  SOURCES src/tick.cpp
)
target_compile_features(demo_hot PRIVATE cxx_std_20)

add_executable(demo src/main.cpp)
target_link_libraries(demo PRIVATE demo_hot nekomata::backends::elf)
```

This is not a special runtime mode. It is a reload group with one member, so
it follows the same descriptor, generation, validation, and commit rules as a
multi-TU group.

## 4. Simple multi-TU project

```cmake
find_package(nekomata CONFIG REQUIRED)

nekomata_add_reload_group(gameplay_hot
  SOURCES
    src/gameplay/ball.cpp
    src/gameplay/gravity.cpp
)
target_compile_features(gameplay_hot PRIVATE cxx_std_20)
target_include_directories(gameplay_hot PRIVATE src)
target_link_libraries(gameplay_hot PRIVATE game_options)

add_executable(game src/main.cpp)
target_link_libraries(game PRIVATE gameplay_hot nekomata::backends::elf)
```

If a header change rebuilds only `ball.cpp`, the publisher still describes and
publishes the complete current membership of `gameplay_hot`. An unchanged
`gravity.cpp` object comes from the current native build output. Publication is
atomic at the group boundary, not at the individual compiler invocation.

## 5. Heterogeneous multi-TU project

Use units when sources cannot truthfully share one compile target:

```cmake
find_package(nekomata CONFIG REQUIRED)

nekomata_add_reload_unit(physics_hot
  SOURCES
    src/physics/collision.cpp
    src/physics/world.cpp
)
target_compile_features(physics_hot PRIVATE cxx_std_20)
target_compile_definitions(physics_hot PRIVATE PHYSICS_PRECISE=1)

add_custom_command(
  OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/behaviour.cpp"
  COMMAND behaviour_codegen
          "${CMAKE_CURRENT_BINARY_DIR}/generated/behaviour.cpp"
  DEPENDS behaviour_codegen data/behaviour.json
  VERBATIM
)

nekomata_add_reload_unit(gameplay_hot
  SOURCES
    src/gameplay/ball.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/generated/behaviour.cpp"
)
target_compile_features(gameplay_hot PRIVATE cxx_std_23)
target_include_directories(gameplay_hot PRIVATE src)

nekomata_add_reload_group(gameplay_group
  UNITS physics_hot gameplay_hot
)

add_executable(game src/main.cpp)
target_link_libraries(game PRIVATE gameplay_group nekomata::backends::elf)
```

The units retain different definitions, language requirements, and generated
dependencies. `gameplay_group_reload` waits for every unit and publishes one
transaction. If the units may commit independently, declare two groups.

## 6. What the adapter creates

For every group, the adapter creates or generates:

1. native object compilation for each reload unit;
2. a descriptor linked into the baseline executable;
3. a publication rule consuming the exact native object set;
4. the public `<group>_reload` target;
5. configuration-specific immutable generations under the build tree.

The descriptor contains the group ID, exact membership identities,
compatibility fingerprint inputs, and a relocatable generation-root locator.
The ordinary `<group>` target contributes both its objects and descriptor to
the final executable.

The adapter MUST ensure descriptor retention under dead stripping and link-time
optimization. Depending on an unreferenced constructor or archive member is
not sufficient evidence of retention.

## 7. One native compile graph

The adapter MUST NOT reconstruct compiler command lines or create a parallel
compilation database. Baseline linking and generation publication consume the
same CMake object-producing targets.

CMake object libraries compile sources without archiving them, and their
objects are addressable through `$<TARGET_OBJECTS:target>`. This is the native
primitive the adapter uses. See CMake's
[`add_library`](https://cmake.org/cmake/help/latest/command/add_library.html)
and
[`cmake-buildsystem(7)`](https://cmake.org/cmake/help/latest/manual/cmake-buildsystem.7.html)
documentation.

For a `SOURCES` group, the public target is object-bearing. For a `UNITS`
group, the public group target injects every member unit's exact object set and
the descriptor into its direct consumer. The adapter MUST test that object
propagation is complete; merely placing object libraries behind a transitive
link interface is insufficient because CMake does not always propagate their
object files as link inputs.

The baseline executable and publisher therefore see identical TU outputs.
There is no second copy of compile flags to drift.

## 8. Publication target implementation

An object library cannot itself receive the build-event form of
`add_custom_command(TARGET ...)`. The adapter therefore creates a separate
stamp-producing `add_custom_command(OUTPUT ...)` and a public custom target:

```cmake
add_custom_command(
  OUTPUT <configuration-specific-stamp>
  COMMAND <private-publisher> <generated-arguments>...
  DEPENDS
    <all-unit-targets>
    <all-unit-object-lists>
    <descriptor-inputs>
  COMMAND_EXPAND_LISTS
  VERBATIM
)
add_custom_target(<group>_reload DEPENDS <configuration-specific-stamp>)
```

This sketch is descriptive, not public copy-and-paste API. The implementation
uses generator expressions to pass the complete `$<TARGET_OBJECTS:...>` lists.
The object paths are also file-level dependencies of the stamp, so recompiling
one member makes the publication rule stale. Target dependencies alone are not
sufficient for that decision. The adapter MUST use `COMMAND_EXPAND_LISTS` and
`VERBATIM` so list expansion and quoting remain correct. See CMake's
[`cmake-generator-expressions(7)`][cmake-generator-expressions].

[cmake-generator-expressions]:
  https://cmake.org/cmake/help/v3.29/manual/cmake-generator-expressions.7.html

The publisher MUST NOT infer an object path from a source name, scan an output
directory, or assume a generator's private layout.

## 9. Build configurations and generators

The contract covers single-config and multi-config generators.

- Generation roots and publication stamps MUST include the effective
  configuration where a generator has one.
- The adapter MUST use `$<CONFIG>` and target properties rather than assuming
  `CMAKE_BUILD_TYPE` is set.
- `gameplay_hot_reload` names the same public target in every configuration;
  `cmake --build ... --config <config>` selects its outputs.
- Debug and optimized configurations MUST have distinct compatibility
  identities and generation streams.

The CMake adapter covers these generator families. Coverage phases with the
runtime backends: the ELF backend makes the POSIX generators the first
delivery, and the NMake Makefiles and Visual Studio families follow the
PE/PDB backend rather than preceding it.

| Generator | Configuration model | Required coverage | Phase |
| --- | --- | --- | --- |
| Ninja | single configuration | object expansion, incremental publication, response files | with ELF |
| Ninja Multi-Config | multiple configurations | configuration isolation plus the Ninja requirements | with ELF |
| Unix Makefiles | single configuration | dependency-driven republishing, parallel Make, shell quoting | with ELF |
| NMake Makefiles | single configuration | Windows paths, `.obj` inputs, NMake constraints, `cmake --build` | after PE/PDB |
| Visual Studio | configuration and platform | generated MSBuild projects, configuration/platform isolation, response files, parallel builds | after PE/PDB |

The implementation and release notes MUST name the exact generator identifiers,
CMake versions, host platforms, and compiler toolsets covered by tests. A family
row above does not promise every historical Visual Studio, NMake, Ninja, or Make
version.

These are one CMake adapter with one generator test matrix, not separate public
adapters. The implementation MUST consume CMake target metadata and generator
expressions. It MUST NOT parse generated `build.ninja`, Makefiles, `.vcxproj`,
or `.sln` files.

NMake and Visual Studio coverage establishes that the CMake build graph and
publication rules are portable to those generators, and is delivered after the
PE/PDB backend exists instead of claiming build-side-only support earlier.
A native, hand-authored MSBuild project is outside this adapter and will
require its own `.props`/`.targets` integration after that backend exists.

Every claimed generator requires automated tests for exact object-list
expansion, generated sources, incremental rebuilds, quoting, long command lines,
and configuration separation where applicable.

The application always invokes the generated reload target through CMake. These
commands illustrate the generator-specific configuration boundary:

```sh
# Ninja
cmake -S . -B build/ninja -G Ninja
cmake --build build/ninja --target gameplay_hot_reload

# Ninja Multi-Config
cmake -S . -B build/ninja-multi -G "Ninja Multi-Config"
cmake --build build/ninja-multi --config Debug --target gameplay_hot_reload

# Unix Makefiles
cmake -S . -B build/unix-make -G "Unix Makefiles"
cmake --build build/unix-make --target gameplay_hot_reload

# NMake Makefiles, from a matching Visual Studio developer environment
cmake -S . -B build/nmake -G "NMake Makefiles"
cmake --build build/nmake --target gameplay_hot_reload

# One concrete Visual Studio generator example
cmake -S . -B build/vs2022 -G "Visual Studio 17 2022" -A x64
cmake --build build/vs2022 --config Debug --target gameplay_hot_reload
```

Directly invoking the generated `ninja`, `make`, `nmake`, or MSBuild files is not
part of the public adapter contract. Keeping `cmake --build` as the entry point
preserves CMake's configuration, platform, parallelism, and tool-selection
semantics.

## 10. Dependencies and generated sources

CMake remains authoritative for dependency expansion. A header, module input,
or generated source MUST be expressed through the normal CMake target graph.
Nekomata does not parse depfiles in the managed CMake path.

Generated sources are valid when CMake knows their producing rule. The reload
target transitively waits for those rules through the native unit target. A
generator failure means no generation is published.

Target-level dependencies that do not appear naturally in the object graph MAY
be added with normal `add_dependencies(<unit> ...)`. The adapter MUST preserve
them; it MUST NOT offer a second Nekomata-specific dependency list.

## 11. Group identity

By default, `group_id` is derived from the normalized source-directory-relative
CMake target identity, not from an absolute build path. It MUST be stable across
build-directory moves and distinct for same-named groups in different source
directories.

`GROUP_ID` overrides the default when an application needs a durable identity:

```cmake
nekomata_add_reload_group(gameplay_hot
  GROUP_ID com.example.game.gameplay
  SOURCES src/gameplay/ball.cpp src/gameplay/gravity.cpp
)
```

Duplicate group IDs linked into one image are a configuration error. The
runtime also rejects duplicate descriptors defensively.

## 12. Multithreaded applications and TUI

CMake integration changes build ownership, not runtime synchronization.
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

## 13. Cross-compilation

The reload objects and backend are target artifacts. The private publisher is
a host tool. The exported package MUST distinguish them and MUST NOT try to run
a target executable while building a cross-compiled application.

If CMake cannot locate a runnable host publisher for the selected package, it
MUST fail configuration with a precise diagnostic. Silently disabling
publication or falling back to guessed object paths is forbidden.

## 14. Migration of an existing target

Start with an ordinary object-bearing target:

```cmake
add_library(gameplay OBJECT
  src/gameplay/ball.cpp
  src/gameplay/gravity.cpp
)
target_compile_features(gameplay PRIVATE cxx_std_20)
target_include_directories(gameplay PRIVATE src)

add_executable(game src/main.cpp)
target_link_libraries(game PRIVATE gameplay)
```

Change only its declaration and add the backend dependency:

```cmake
nekomata_add_reload_group(gameplay
  SOURCES
    src/gameplay/ball.cpp
    src/gameplay/gravity.cpp
)
target_compile_features(gameplay PRIVATE cxx_std_20)
target_include_directories(gameplay PRIVATE src)

add_executable(game src/main.cpp)
target_link_libraries(game PRIVATE gameplay nekomata::backends::elf)
```

Existing target configuration remains next to the target. The migration MUST
NOT require copying flags into a manifest, maintaining an object list, or
adding a rebuild script.

If an existing target's semantics cannot be represented without changing its
public usage requirements, the adapter MUST fail or require an explicit
refactor. It MUST NOT silently alter include propagation, definitions, link
ordering, language mode, PIC, visibility, or optimization.

## 15. Failure contract

| Condition | Required behavior |
| --- | --- |
| group specifies both `SOURCES` and `UNITS` | fail CMake configure |
| a unit is missing or belongs to conflicting groups | fail CMake configure |
| duplicate linked group ID | fail configure or final link |
| compile or generator rule fails | publish no generation |
| exact native object set cannot be expanded | fail the reload target |
| host publisher is unavailable | fail CMake configure |
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

## 16. Acceptance criteria

CMake integration is not complete until automated tests demonstrate:

- single-TU, simple multi-TU, and heterogeneous multi-TU projects;
- one native compile edge feeding baseline and generation publication;
- no application-side path, manifest, depfile, or compiler configuration;
- ordinary header, generated-source, and explicit target dependencies;
- exact full-group publication after a one-TU change;
- cross-TU reference resolution and atomic rollback;
- stable default group IDs in nested source directories;
- explicit group-ID override and duplicate-ID rejection;
- descriptor retention under dead stripping and supported LTO modes;
- debug and every claimed optimized configuration;
- Ninja and Ninja Multi-Config, including configuration separation;
- Unix Makefiles, including parallel builds and shell quoting;
- NMake Makefiles with Windows paths and `.obj` inputs, after PE/PDB;
- Visual Studio generators with configuration and platform isolation, after
  PE/PDB;
- source and build paths containing spaces;
- response-file and long object-list handling;
- a host publisher in a supported cross-compilation setup;
- publication failure producing no offer;
- two processes consuming one immutable offer;
- moved generation-root override;
- repeated `watch()` and `unwatch()` preserving the cursor;
- later valid generation recovery after rejection;
- TUI requests committing only through the host safe point;
- migration from an existing target without duplicated compile settings.

## 17. Current repository gap

The runtime currently discovers embedded ELF group descriptors, consumes
immutable generation streams, exposes managed `watch()`/`unwatch()`, and
reports structured update events. The repository does not yet export
`nekomata_add_reload_unit()`, `nekomata_add_reload_group()`, or the CMake
publication targets described here.

Current object watches and handwritten
demo rebuild scripts are implementation and compatibility mechanisms. They
must not be presented as satisfying this CMake contract.

Implementation should begin with the `SOURCES` form and prove that one CMake
object set can feed both the baseline image and the publisher across the full
generator matrix without copying compile commands or guessing output paths.
The `UNITS` form follows after that invariant is established.
