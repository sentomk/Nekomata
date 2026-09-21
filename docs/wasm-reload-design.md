# Browser WASM hot-reload design

Status: the private candidate lifecycle is implemented. Remote generation
delivery, integration with `neko::reload_session`, and a public WASM backend
are planned. This document distinguishes those goals from the current code.

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
| `emscripten_loader` | Adapt `emscripten_dlopen`, resolve the descriptor, and own the loader reference. |
| `module_image` | Keep loaded code available and release an uncommitted reference on destruction. |
| `candidate` | Validate a requested contract and own the prepared result until activation or discard. |
| `prepared_module` | Own a copied entry set and its image; provide immutable lookup to callers. |
| `active_module` | Consume a ready candidate and replace the complete active entry set. |
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

An invalid input contract is rejected before loading. Construction can also
finish with a ready or rejected result immediately: an already-loaded immutable
path can complete synchronously inside `open()`.

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
`missing_descriptor`, `incompatible`, and `invalid_descriptor`; the exact
diagnostics live with the implementation and regression tests.

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
  runs on that world. Rejected candidates leave the current behavior advancing.
- `neko.e2e.wasm.candidate_lifetime` checks late callbacks after cancellation
  and destruction, loader destruction, cached synchronous completion, and
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

The next layers must supply complete generation identity and immutable
artifacts, integrity and compatibility checks before instantiation where
possible, delivery ordering and supersession policy, and observable acceptance
or rejection results. Their transport and metadata format are not selected by
the candidate implementation.

Session integration must connect preparation and safe-point activation to the
library's reload model without making the browser loader responsible for
building code or owning the world. Public backend factories remain a final
integration step; application-facing CMake integration follows that factory
work. Neither native backend factories nor a native embedded WASM runtime are
prerequisites for this browser path.

A later browser demo can show boids acquiring new avoidance and vortex code
while retaining identity, position, velocity, trail, and world age. Two pages
receiving the same generation should keep their separate worlds. The current
fixtures remain tests, not that demo or proof of remote generation delivery.
