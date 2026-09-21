# Browser generation fixtures

These browser tests exercise `src/backends/wasm` candidate preparation,
activation and private session delivery. They are not a demo or a public
Nekomata WASM backend. The descriptor is private; application state and entry
signatures are test-specific.
All persistent state belongs to the main module; side modules have no
side-effecting constructors. Modules remain loaded until the page closes.

Enable from a native host build with an installed Emscripten SDK, Python 3
and Chromium/Chrome. Configuration does not download any dependencies:

```sh
bash scripts/configure.sh debug -DNEKOMATA_TEST_WASM=ON \
  -DNEKOMATA_BUILD_TOOLS=ON \
  -DNEKOMATA_WASM_BROWSER=/path/to/chrome
bash scripts/build.sh debug --target neko_wasm_fixtures
bash scripts/test.sh debug -R '^neko\.e2e\.wasm\.'
```

`NEKOMATA_EMXX` can select a particular `em++`; select `NEKOMATA_EMCMAKE`
from the same SDK for the session fixture. CMake and Ninja come from the host
build's toolchain. Candidate fixtures use identical explicit main/side flags;
the session main module also enables exception catching for the offer codec.
The Python standard-library runner serves the fixtures on loopback and starts headless Chromium
with a temporary profile. A browser assertion, crash or timeout fails CTest.
It does not reuse an existing browser profile or disable the browser sandbox.

The suite checks that two concurrently loaded modules with the same descriptor
export resolve to their own callable code through the backend's loader.
This is browser execution evidence, not native host execution of WASM.

The main module owns a world with a tick count, position and velocity. The
server holds B's response until the first A frame acknowledges progress, so
A must advance while the download is pending without relying on a timed sleep.
B then stays ready for three A frames. At the next frame boundary the runner
switches the entire descriptor, checking exact field equality and address
identity before running the next tick. Only B contains the bounce branch;
the next three ticks must execute it on the existing world. Both the identity
and update entries must come from the same generation.

After activation the suite offers an incompatible interface version, a null
update entry, a missing artifact, and real bytes with a tampered digest, in
that order. Each must produce its expected backend rejection classification
without changing the active module; the digest mismatch is caught before
instantiation. The world must continue advancing under B for at least three
frames after each rejection. This validates cooperative fixture activation,
not arbitrary module initialization rollback, thread safety or production ABI
validation. The incompatible fixture deliberately retains the same descriptor
prefix; the version check happens after loading, before accessing its
function entries.

No public `neko::reload_session` backend is introduced here. Pre-instantiation
ABI metadata, authenticated publication and old-module reclamation remain
separate work. The loopback server serves immutable artifacts and receives
test coordination signals and the browser's assertion result.

The `wasm` CI job installs Emscripten 6.0.9 and runs these tests in the Ubuntu
runner's Chrome. Candidate ownership and validation tests also run in the
ordinary native and sanitizer jobs. CI uploads CTest diagnostics on failure.

`candidate_lifetime` drives actual late loader callbacks after cancellation,
candidate destruction and loader destruction, then checks cached completion
on the event loop and ready-candidate cancellation. Finally it destroys every
C++ owner of a committed module and calls a saved entry to verify page
residency. The server holds the first response until a browser frame releases
it, so the cancelled request cannot complete early by accident.

`poll_scheduler` exercises the browser event-loop timer: callbacks are deferred,
subscriptions survive destruction of their scheduler, a cancelled subscription
never fires, and an executing callback can destroy its own subscription without
firing again. It is registered in the same WASM CI job.

`session_lifecycle` connects the real CMake adapter and native publisher to
HTTP manifest polling, the browser loader and the private session. It requires
`NEKOMATA_BUILD_TOOLS=ON`; the fixture build target also builds the publisher.
Each run gets an isolated build/publication directory and rebuilds the same
reload group through three compile configurations, without changing tracked
source files. CI explicitly requires this test to be present and pass.

The page starts disabled, then watches and applies A. After A advances, it
pauses observation while the server builds and publishes B. Independent timer
pulses and fetch counters check that paused frames do not start requests.
After resuming, preparation proceeds without calling `update()`. The server
holds B's artifact response until the page pauses again and releases it, so
B must finish into retained state while A keeps running. Several frames later,
resuming and immediately calling `update()` commits B without another poll.
Every update checks exact world-field equality; every tick checks identity,
age and generation-consistent behavior. Finally, the CMake target publishes
C with an incompatible descriptor version. One rejection is reported; B
continues advancing through repeated delivery of C without replaying it.

Download release and loader-completion observations establish ordering, not
elapsed-time guesses. Deadlines only turn a stalled test into a failure. The
native session unit suite retains finer-grained coverage of stale scheduling
callbacks, late manifests, destruction and scheduler failures. This browser
case does not establish public API integration or multi-group support.
