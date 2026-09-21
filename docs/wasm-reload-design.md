# Browser WASM hot-reload design

Status: the private candidate lifecycle, HTTP-polling delivery with digest
verification, the private browser reload session, and a local browser demo
are implemented. A public WASM backend and application-facing CMake
integration are planned. This document distinguishes those goals from the
current code.

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
       publishes generation             planned delivery layer
              |
browser receives and verifies artifacts
              |
      prepares candidate                implemented private lifecycle
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
session implementation already supports WASM.

## Component boundaries

The implementation lives in [`src/backends/wasm`](../src/backends/wasm/).
Its headers are private, not a supported application API.

| Component | Responsibility |
| --- | --- |
| `module_loader` | Own a load request and deliver exactly one result on the calling event loop. |
| `manifest_fetcher` | Own one manifest URL fetch and deliver its text on the calling event loop. |
| `offer_poller` | Poll the stable manifest URL, order offers, and turn superseding offers into loading candidates. |
| `emscripten_loader` | Fetch artifact bytes, verify the contract SHA-256 before instantiation, stage under a private MEMFS path, and own the loader reference. |
| `emscripten_manifest_fetcher` | Fetch one manifest URL as text on the browser event loop. |
| `module_image` | Keep loaded code available and release an uncommitted reference on destruction. |
| `candidate` | Validate a requested contract and own the prepared result until activation or discard. |
| `prepared_module` | Own a copied entry set and its image; provide immutable lookup to callers. |
| `active_module` | Consume a ready candidate and replace the complete active entry set. |
| `reload_session` | Own the poller and the active module; report generation transactions at the application's safe point. |
| Application | Own persistent state, define entry signatures, and establish the safe point. |

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
There is currently no artifact digest, publisher authentication, remote stream,
or pre-instantiation compatibility check.

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

The [`wasm` CI job](../.github/workflows/ci.yml) pins Emscripten 6.0.9, builds
the main and side modules, and runs native candidate tests plus both browser
tests in Ubuntu's Chrome. CTest diagnostics are uploaded on failure. Wiring
these jobs is not evidence that a particular remote run passed; consult that
commit's CI results.

For local reproduction, install Emscripten, Python 3, and Chrome/Chromium, then
use the commands in the [browser suite README](../tests/e2e/wasm/README.md).
`bash scripts/check.sh debug` runs the full configured checks. Browser fixtures
are opt-in with `NEKOMATA_TEST_WASM=ON`; configuration does not download the SDK.
This CMake registration is test infrastructure, not application build integration.

## Remaining design work

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

Still planned on top of the offer: the poller now exists —
`offer_poller` fetches the manifest URL on the application event loop, orders
offers through `compare_wasm_offers`, and hands superseding offers to the
candidate lifecycle as observable events; its native suite drives it through
mock fetchers. Session integration also exists in private form:
`neko::wasm::reload_session` pins one group, polls inside `update()`, reports
completed candidates as applied or rejected transactions using `candidate_error`,
and exposes the active snapshot for per-frame entry resolution. It has no
`watch()`, `unwatch()` or public observation `snapshot()` yet. Sharing a safe
point does not make its lifecycle equivalent to the native managed session.

### Session lifecycle convergence

The public backend must obey the existing
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
ordinary platform and sanitizer CI jobs. Equivalent browser lifecycle and
late-completion tests are still required; the existing browser fixtures do
not establish watch/unwatch parity.

Browser observation can use asynchronous event-loop scheduling instead of a
native worker thread. Public integration also requires page-side group
registration, unified transaction results, and a stable behavior-call seam.
Adding a factory alone does not provide these capabilities, and the private
`current()->entry(...)` access pattern is not a new public session promise.

### Build integration and remaining work

The demo at [`examples/wasm_reload`](../examples/wasm_reload/) consumes the
same CMake registration as native groups: under the Emscripten toolchain,
`nekomata_add_reload_group` gains `ABI_ID`, `ENTRIES`, and `OFFER_ROOT`
arguments, links its units into one side module, and drives the host
publisher's `wasm` mode — reusing the request pipeline and the publisher's
lock, sequence, and atomic-release discipline. The request still describes
build inputs; the published manifest differs per backend
(`nekomata-generation/2` versus `nekomata-wasm/1`), and the browser flavor
emits no embedded descriptor section. The demo is not registered in CI.
What remains is public backend factories with the convergence of the two
event vocabularies at that factory boundary.

Session integration must connect preparation and safe-point activation to the
library's reload model without making the browser loader responsible for
building code or owning the world. Public backend factories remain a final
integration step; application-facing CMake integration follows that factory
work. Neither native backend factories nor a native embedded WASM runtime are
prerequisites for this browser path.

A richer demo can still show boids acquiring new avoidance and vortex code
while retaining identity, position, velocity, trail, and world age; the
current ball demo establishes the loop with a smaller world. Two pages
receiving the same generation keep their separate worlds — the world is
per-page state. The current fixtures remain tests, and the demo is a local
development tool, not proof of remote generation delivery.
