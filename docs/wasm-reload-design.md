# Browser WASM hot-reload design

Status: the public `neko::wasm::create_backend()` connects browser delivery and
activation to `neko::reload_session`. CMake-generated group registration,
owning entry snapshots, host ABI validation and installed-library browser
acceptance are implemented. Migrating the existing demo to the public API
remains separate work.

## Purpose and scope

Nekomata's WASM direction combines C++ hot reload with Emscripten's browser
target: keep a running application's world, prepare replacement code, and
switch behavior at an application-controlled safe point. It does not embed a
WASM runtime in a native host. Native applications continue to use native
backends.

A motivating example is a browser physics game whose rare collision bug takes
30 minutes to reproduce. Replacing the faulty update function should preserve
the current objects, player state, and simulation time. Refreshing the page or
creating a replacement world would destroy the useful reproduction state.

The intended end-to-end flow is:

```text
developer rebuilds C++ with emcc
              |
       publishes generation             HTTP-polling delivery
              |
browser receives and verifies artifacts
              |
      prepares candidate                backend-private lifecycle
              |
    ready, but not yet active
              |
application reaches a safe point
              |
  replace complete active entry set
              |
 new behavior uses the existing world
```

The [managed reload design](managed-reload-design.md) describes the broader
generation and publication model. This document describes the browser-specific
mechanism; it does not imply that the existing native publication format or
native session implementation is used inside the browser.

## Component boundaries

The implementation lives in [`src/backends/wasm`](../src/backends/wasm/).
Its headers are private, not a supported application API.
Applications include [`neko/wasm.hpp`](../include/neko/wasm.hpp) and
[`neko/session.hpp`](../include/neko/session.hpp), without implementation headers.

| Component | Responsibility |
| --- | --- |
| `module_loader` | Own a load request and deliver exactly one result on the calling event loop. |
| `manifest_fetcher` | Own one manifest URL fetch and deliver its text on the calling event loop. |
| `offer_poller` | Poll the stable manifest URL, order offers, and turn superseding offers into loading candidates. |
| `poll_scheduler` | Schedule observation independently of frame commits; an owning subscription controls each enable cycle. |
| `emscripten_loader` | Fetch artifact bytes, verify the contract SHA-256 before instantiation, stage under a private MEMFS path, and own the loader reference. |
| `emscripten_manifest_fetcher` | Fetch one manifest URL as text on the browser event loop. |
| `module_image` | Keep loaded code available and release an uncommitted reference on destruction. |
| `candidate` | Validate a requested contract and own the prepared result until activation or discard. |
| `prepared_module` | Own a copied entry set and its image; provide immutable lookup to callers. |
| `active_module` | Consume a ready candidate and replace the complete active entry set. |
| `reload_session` | Own the poller and the active module; report generation transactions at the application's safe point. |
| Application | Own persistent state, define entry signatures, and establish the safe point. |

The public session exclusively owns a `backend::session_driver`. The native
bundle constructor creates the unchanged native lifecycle; the browser factory
creates a driver owning its loader, fetcher, scheduler and private session.
The private session is destroyed before its adapters. Browser builds do not
compile the native worker or machine-code transaction implementation.

## Public usage and ownership

Declare groups in the build system, then link their targets into the page.
`MANIFEST_URL` names the served manifest, not the local publication directory:

```cmake
find_package(nekomata CONFIG REQUIRED)
include("${nekomata_DIR}/nekomata.cmake")

nekomata_add_reload_group(behavior SOURCES behavior.cpp
  GROUP_ID flock ABI_ID flock-v1 ENTRIES tick
  MANIFEST_URL offers/latest OFFER_ROOT "${CMAKE_BINARY_DIR}/offers")
add_executable(page main.cpp)
target_link_libraries(page PRIVATE nekomata::neko behavior)
```

The cross-build also needs `NEKOMATA_PUBLISHER_EXECUTABLE` pointing to a native
host publisher. The group target adds only generated registration to the page;
`behavior_reload` builds and publishes its side module. The application does
not repeat the URL, ABI identity or entry membership in C++. This example
assumes the module implements `tick` with the agreed signature and world layout:

```cpp
#include <neko/session.hpp>
#include <neko/wasm.hpp>

struct world_state { float position; float velocity; };
world_state world{100.0f, 1.0f};
neko::reload_session session{neko::wasm::create_backend()};

void start() { session.watch(); }
void pause() { session.unwatch("flock"); }

void frame() {
    // No reloadable entry is executing at this safe point.
    [[maybe_unused]] const auto outcomes = session.update();
    if (const auto entries = neko::wasm::acquire(session, "flock")) {
        entries.get<void(world_state*)>("tick")(&world);
    }
}
```

`acquire()` is empty until the first successful activation. One `entry_set`
pins one immutable generation: retain it for all entry calls in a frame.
`get<signature>(name)` checks that an entry exists, not its actual C++ type;
the caller must supply the signature covered by the group's ABI identity.
An empty snapshot or an unknown entry throws a configuration error.

`acquire(session, group_id)` selects an already registered group; it never
registers one. Each factory copies the build-generated metadata into an
independent session, with its own observation history and active code. A session
move transfers that state; acquisition from a moved-from or non-browser session
throws. Destroying a session stops observation; saved entry snapshots remain
callable. A new session starts with no active generation, even if an older
session consumed the same stream.

Generated records are installed before ordinary application global constructors,
so a global session can discover them. Factory construction validates all
records and rejects duplicate IDs. The installed `neko/detail` registration
header is an internal build/runtime contract, not an application registration API.

Group IDs, manifest URLs and ABI IDs must be nonempty and contain no embedded
NUL; entry names must additionally be nonempty and unique. Manifest URLs name
stable files without query parameters. The host pins the exact ordered entry
membership and ABI identity. A superseding offer that disagrees advances the
observation cursor but becomes an `incompatible` rejection without downloading
or instantiating its artifact. `update()` reports it once while retaining old
code; pausing retains that unreported rejection like any other prepared result.

Lifecycle semantics are shared with native reload: watch, pause, resume,
safe-point update, results and snapshots. Activation is deliberately different:
native reload redirects function entries; WASM replaces the registered entry
set. Direct C++ calls are not transparently redirected. Object-path `watch`
overloads reject on the browser backend. The existing
`redirected_function_count` field counts activated WASM entries, not patched
machine instructions.

With an Emscripten-built installation, ordinary `find_package(nekomata CONFIG
REQUIRED)` consumers link `nekomata::neko`. The aggregate includes the browser
backend and its required PIC, main-module, fetch, memory-growth and exception flags.
`nekomata::wasm` is also exported. Linking declared group targets adds their
registration through the existing CMake adapter. The current browser contract
uses the single application event loop, not pthreads or a native embedded runtime.

The current browser fixtures compile a persistent Emscripten main module and
reloadable side modules. The main module owns `world_state`; side-module
behavior receives that existing state. No state serialization, copying, or
migration occurs during activation. This is an explicit application contract,
not automatic preservation of arbitrary C++ globals, objects, or vtables.

## Descriptor and compatibility

[`module_descriptor.hpp`](../src/backends/wasm/module_descriptor.hpp) defines
the cooperative, same-toolchain contract. A module exports
`neko_wasm_descriptor`, returning a pointer to a descriptor header. The full
descriptor carries a layout version and size, an ABI identity, and an ordered
list of named function addresses.

Candidate preparation checks:

- A nonempty path and ABI identity, and nonempty, unique requested entry names;
  embedded NUL characters in these input strings are rejected.
- A descriptor exists and its header has the supported version and exact size,
  before accessing the rest of the descriptor.
- Its ABI identity matches the requested identity.
- Entry count and ordered names exactly match the requested membership, with
  no null entry array, name, or function address.

The ABI identity must cover the application's entry signatures and persistent
state layout. It is a declared identity, not a computed proof: the backend does
not inspect actual function signatures or discover ABI-breaking C++ changes.
The generic `module_function` representation must be converted back to the
agreed signature before invocation.

Names and addresses are copied before the candidate becomes ready. Later
descriptor metadata changes therefore do not alter the prepared entry set.
Descriptor pointers and strings themselves are trusted; checking a declared
size does not prove that an arbitrary pointer references readable storage.
This interface is not a parser or sandbox for hostile modules.

## Candidate lifecycle and ownership

```text
loading ---- validation succeeds ----> ready ---- activate ----> activated
   |                                     |
   +---- load/validation fails --> rejected
   |                                     |
   +---- cancel -------------------------+----> cancelled
```

An invalid input contract is rejected before loading — the contract's
SHA-256 is structurally required, so no load ever starts without an expected
digest. The adapter verifies the fetched bytes against that digest before
instantiation; a mismatch rejects as an `integrity` failure without touching
the compiler. Real fetches always complete on the event loop, though the
loader interface itself still permits a synchronous completion inside
`open()`.

The loader owns the request path and completion callback independently of the
loader object's lifetime. The callback holds only a weak reference to candidate
state. Destroying a candidate, or replacing it through move assignment, makes
an outstanding result discardable without accessing the old object. Moving a
candidate transfers the state to its new owner; a moved-from object reports
`cancelled`.

`cancel()` discards a loading or ready candidate. It does not abort network I/O,
undo instantiation, or change an already activated or rejected candidate.
A late result cannot revive a cancelled candidate. Its uncommitted image is
released through ordinary ownership cleanup.

All operations and completions use one event loop. These objects provide no
cross-thread synchronization, worker coordination, or cancellation of code
already executing.

## Activation and state continuity

`active_module::activate()` accepts only a ready candidate. It pins the image
to page lifetime, moves the prepared module into the active slot, and marks
the candidate activated. Passing a loading, rejected, cancelled, moved-from,
or consumed candidate returns false without replacing the active module.

Activation performs no allocation, descriptor validation, or application
behavior invocation. The caller must keep reloadable code quiescent during
the call. In the fixtures, the application uses a frame boundary.

`current()` returns an owning, immutable snapshot. A frame should retain one
snapshot and resolve all of its entries through that snapshot, so its identity
and update functions belong to the same module. Separately fetching the active
module across a switch would not provide that guarantee.

Here, atomic activation means replacing one complete entry set on the event
loop, not a hardware-atomic operation across threads. It also does not redirect
arbitrary existing direct C++ calls or function pointers. Calls must go through
the selected entry set. A saved old entry still refers to old code.

Committed images remain resident even after every C++ owner is destroyed.
This deliberately favors valid old code pointers over reclamation. Repeated
updates can accumulate code and runtime resources for the page's lifetime.
Discarding an uncommitted image calls `dlclose` to release the adapter's
reference; it does not promise reclamation of linear memory or table slots.

## Failure boundary

Load failures and descriptor rejection leave the active slot untouched. The
browser tests verify that the previous behavior continues advancing the world.
The error classifications are `invalid_contract`, `load_failed`,
`integrity`, `missing_descriptor`, `incompatible`, and `invalid_descriptor`;
the exact diagnostics live with the implementation and regression tests.

This guarantee depends on cooperative modules. Emscripten instantiates a module
before descriptor validation; initialization and descriptor access must be
side-effect-free. A constructor that writes shared application memory cannot
be rolled back by rejecting its descriptor. Traps, allocation failure, and
arbitrary exceptions are not converted into a recoverable transaction by this
implementation. Failure in application behavior after activation is also not
an automatic rollback to the previous module.

Paths must name immutable contents because the underlying loader may cache
them. Overwriting a URL is not a supported way to identify a new generation.
Artifact digests and host offer metadata are checked before instantiation.
Publisher authentication and inspection of the actual module descriptor before
instantiation are not implemented.

## Verification and CI

[`tests/unit/backends/wasm`](../tests/unit/backends/wasm/) exercises contract
validation, header-only rejection, ownership, moves, cancellation, late and
synchronous completion, copied metadata, and activation using a mock loader.
These tests participate in the ordinary native and sanitizer CI jobs. Native
execution tests the lifecycle logic, not execution of WASM in a native host.

[`tests/e2e/wasm`](../tests/e2e/wasm/) executes the real adapter in Chrome:

- `neko.e2e.wasm.generations` holds a download until the old world advances,
  keeps a ready candidate inactive for three frames, then switches both entries
  at a frame boundary with identical world fields and address. New behavior
  runs on that world. Rejected candidates leave the current behavior
  advancing: an incompatible descriptor, an incomplete entry table, a missing
  artifact, and — against real bytes — a tampered digest rejected before
  instantiation.
- `neko.e2e.wasm.candidate_lifetime` checks late callbacks after cancellation
  and destruction, loader destruction, asynchronous cached completion, and
  invocation of committed code after all C++ owners are gone.
- `neko.e2e.wasm.poll_scheduler` checks deferred browser timer callbacks,
  subscription ownership, cancellation, and self-destruction during a callback.
- `neko.e2e.wasm.session_lifecycle` publishes real CMake-built generations over
  HTTP to the private session. It checks disabled observation, preparation
  without frame updates, an artifact finishing while paused, retained work
  committed on resume, exact world-state continuity and one incompatible
  generation rejection followed by continued old-code execution. Server gates
  establish download ordering; the fixture observes real loader completions.
  Public observation snapshots expose the paused ready state and the accepted
  cursor before activation; transaction counters follow consumed events only.
- `neko.e2e.wasm.public_factory` builds and installs the Emscripten library,
  then compiles an ordinary package consumer using only public headers. Two
  real publication streams exercise public watch/unwatch, paused completion,
  resume, mixed rejection/application, sorted results and snapshots, persistent
  worlds, session moves and entries surviving session destruction. Both the
  `SOURCES` and `UNITS` CMake forms generate registration and publish the real
  generations from the same declarations. Global session construction checks
  registration initialization order. Existing
  private single- and multi-group tests retain their transport instrumentation.
- `neko.e2e.wasm.empty_registry` links no group targets and checks empty factory
  construction, snapshots, updates and the exact all-group watch rejection.

The [`wasm` CI job](../.github/workflows/ci.yml) pins Emscripten 6.0.9, builds
the main and side modules, and runs native lifecycle tests plus the browser
tests in Ubuntu's Chrome. CTest diagnostics are uploaded on failure. Wiring
these jobs is not evidence that a particular remote run passed; consult that
commit's CI results.

For local reproduction, install Emscripten, Python 3, and Chrome/Chromium, then
use the commands in the [browser suite README](../tests/e2e/wasm/README.md).
`bash scripts/check.sh debug` runs the full configured checks. Browser fixtures
are opt-in with `NEKOMATA_TEST_WASM=ON`; configuration does not download the SDK.
The fixtures exercise the installed application build adapter; the test runners
and HTTP gates themselves remain test infrastructure.

## Delivery and session semantics

Delivery is selected: HTTP polling. The page fetches a stable manifest URL
on the application event loop; artifacts live at immutable paths and are
fetched independently; a lower `sequence` is ignored and an equal `sequence`
must repeat the identical offer, because one sequence slot belongs to one
immutable generation. Long-polling is a later optimization; WebSocket and
SSE push were rejected as dependency weight for the development loop.

The metadata format is selected with that transport: the
[`nekomata-wasm/1`](../src/protocol/wasm_offer.hpp) offer codec — one
immutable artifact with its SHA-256, the ordered entry set, the declared
`abi_id`, and the supersession `sequence`. Reusing the existing format
identifiers was considered and rejected: `nekomata-publisher-request/1`
describes build inputs before a generation exists, and the managed
`nekomata-generation/2` manifest has member rows that do not fit one
artifact with an ordered entry set. Build-provenance rows are deliberately
absent until a consumer exists. The codec touches neither network nor
filesystem; ordering decisions are a pure comparison on the value.

`offer_poller` fetches the manifest URL on the application event loop, orders
offers through `compare_wasm_offers`, and hands superseding offers to the
candidate lifecycle as observable events; its native suite drives it through
mock fetchers. The factory's private lifecycle implementation,
`neko::wasm::reload_session`, accepts a fixed list of private `group_registration`
values carrying group ID, manifest URL and optional diagnostics callback.
IDs must be nonempty and unique, and all groups start disabled. Its `watch()`
and `unwatch()` overloads control all groups or one named group while preserving
each poller and its consumer cursor. The browser scheduler polls every 100 ms
on the event loop, without requiring `update()`. Each enable cycle has a fresh
weak callback token, so a queued callback from an earlier cycle cannot start
another poll after disable or re-enable.

`update()` only consumes prepared results while enabled. In-flight fetches
may finish while paused, retaining a candidate or rejection for resume; they
cannot activate it. Destruction cancels scheduling and invalidates outstanding
observation callbacks. Transactions return the same `neko::update_result`
defined in [`include/neko/session.hpp`](../include/neko/session.hpp), including
`group_id`, `generation_id`, `redirected_function_count` and `any_applied()`.
No second WASM result type or conversion wrapper is exposed. `current(group_id)`
remains private; public applications use `neko::wasm::acquire(session, group_id)`.
The private session also returns `neko::session_snapshot` by value through
`snapshot() const`. The factory discovers build-generated registrations and
passes their expected host contracts to the private session.

Groups keep separate subscriptions, candidates, consumer cursors, active code
and reported-generation identities; identical sequence or generation IDs in
different groups do not collide. `update()` consumes enabled groups in ascending
group ID order. Each group is its own transaction, so one rejection does not
prevent another group from applying. This is not a cross-group atomic switch.
All event storage and bookkeeping are prepared before any activation, avoiding
allocation failure after an earlier group has already switched.

An empty registry has an empty snapshot and no update events; all-group `watch()`
rejects it. Unknown IDs are configuration errors for named operations, including
`current(group_id)`. If all-group `watch()` fails to schedule a group, previously
enabled groups stay enabled and a retry only starts the remaining subscriptions.

Snapshot reads do not poll, consume a result or activate code. Every registered
group is present from construction with `enabled=false`, sequence zero and no
applied generation. Its cursor follows the newest accepted offer, including a
manifest delivered while paused; stale, conflicting, wrong-group and malformed
offers do not advance it. The newest candidate determines observation state:

| Candidate state | Enabled group | Disabled group |
| --- | --- | --- |
| Ready, not consumed | `ready` | `ready` |
| Rejected, before or after reporting | `failed` | `failed` |
| Loading, activated, cancelled, or absent | `preparing` | `idle` |

`preparing` describes enabled observation, not proof of an outstanding network
request. A superseding offer replaces the candidate and may move `failed` back
to `preparing`, but it does not erase transaction history. `applied`, `rejected`
and `last_result` change only when `update()` consumes a transaction. A rejected
event preserves `last_applied_generation`; a successful event replaces it and
sets `last_result` to `applied generation '<id>': <count> function(s)`, matching
the native managed result text. Preparation, pause/resume, duplicate delivery
and superseded pending work do not change those counters. `watched_paths` is
empty because this private session has no individual object watches. Saved
snapshots own their values independently of later session activity. Groups are
sorted by ID; counters sum consumed transactions across groups and `last_result`
describes the final event consumed in that order, not network-completion order.

Candidate validation retains its private `candidate_error` vocabulary.
[`session_error.hpp`](../src/backends/wasm/session_error.hpp) maps that typed
classification to the public result without parsing diagnostic messages:

| Candidate error | Public `reload_error_code` |
| --- | --- |
| `none` | `none` |
| `invalid_contract` | `invalid_artifact` |
| `incompatible` | `incompatible` |
| `integrity` | `integrity` |
| `load_failed`, `missing_descriptor`, `invalid_descriptor` | `object_rejected` |

`load_failed` currently combines transport, staging and instantiation failures;
it cannot establish an integrity failure from its type alone. The session
preserves the diagnostic text unchanged. A rejected event redirects zero
functions and retains the active entry set. An activation refusal before any
entry switch is also `object_rejected`, not `commit_failed`: the latter is
reserved for an entry write that failed and was successfully rolled back.
Manifest transport/codec diagnostics and ignored offers remain observation
diagnostics, not generation transactions.

### Session lifecycle convergence

The public backend obeys the existing
[managed lifecycle contract](managed-reload-design.md), rather than expose
the private browser session as a second public interface:

| Operation | Required managed behavior |
| --- | --- |
| Construction | Register known groups, initially disabled. |
| `watch()` / `watch(group_id)` | Idempotently enable all known groups or one group and start observation and preparation. |
| `unwatch()` / `unwatch(group_id)` | Pause observation and consumption; retain the cursor, prepared work and unreported rejection. Applied code keeps running. |
| Re-enable | Resume from the cursor; retained work is eligible for consumption unless superseded, without fetching already-observed generations again. |
| `update()` | Consume enabled groups' prepared results at the application safe point; callbacks never activate code. |
| `snapshot()` | Expose immutable observation state, independently from the callable entry set. |

Unknown group IDs are configuration errors. A disabled group can retain
`ready` or `failed` state, so `enabled` must be observed separately. Each group
is its own transaction; disabling one must not block another enabled group.
The native contract is covered by `neko.integration.session.lifecycle` in the
ordinary platform and sanitizer CI jobs. The WASM session unit suite drives
observation separately from commits and covers pauses before and after
completion, cursor retention, old ticks after resume, scheduling failure, and
destruction with outstanding requests. The real browser scheduler is tested
separately. The same unit suite also covers multi-group selection, independent
pause/resume and cursors, sorted events and snapshots, mixed success/rejection,
scheduling failure and destruction with multiple outstanding requests. These
tests run in the existing native and WASM CI jobs.
`neko.e2e.wasm.session_lifecycle` covers the single-group CMake publisher-to-HTTP
pause/resume path with the private session,
including late artifact completion and world continuity.
`neko.e2e.wasm.multi_group_session` drives two independent CMake streams in
one Chrome page: one group pauses while another applies, a gated artifact
finishes into paused ready state, and a single update reports an incompatible
group alongside a successfully applied group in sorted order. Both worlds
retain identity and advance under their own active code. The test also checks
cursor isolation, aggregate history and duplicate suppression. Both browser
session tests are required explicitly by CI. The separate installed-consumer
`public_factory` gate establishes public integration; none promises cross-group atomicity.

Browser observation uses asynchronous event-loop scheduling instead of a
native worker thread. Build targets supply registration; owning entry snapshots
provide the behavior-call seam. Shared lifecycle results and observation snapshots remain
distinct from owning callable snapshots; the private `current(group_id)->entry(...)`
access pattern is not a public session promise.

### Build integration and remaining work

The demo at [`examples/wasm_reload`](../examples/wasm_reload/) consumes the
same CMake registration as native groups: under the Emscripten toolchain,
`nekomata_add_reload_group` accepts `ABI_ID`, `ENTRIES`, `MANIFEST_URL`, and `OFFER_ROOT`
arguments, links its units into one side module, and drives the host
publisher's `wasm` mode — reusing the request pipeline and the publisher's
lock, sequence, and atomic-release discipline. The request still describes
build inputs; the published manifest differs per backend
(`nekomata-generation/2` versus `nekomata-wasm/1`), and the browser flavor
emits no native embedded descriptor section. Linking a browser group target
adds generated host registration without linking its hot objects into the page.
The demo is not registered in CI and still uses the private session. Migrating
it to the public factory remains follow-up work.

The public integration connects preparation and safe-point activation without
making the browser loader responsible for building code or owning the world.
The existing CMake publication path supplies the consumer registration as well.
Neither native backend factories nor a native embedded WASM runtime are
prerequisites for this browser path.

A richer demo can still show boids acquiring new avoidance and vortex code
while retaining identity, position, velocity, trail, and world age; the
current ball demo establishes the loop with a smaller world. Two pages
receiving the same generation keep their separate worlds — the world is
per-page state. The current fixtures remain tests, and the demo is a local
development tool, not proof of remote generation delivery.
