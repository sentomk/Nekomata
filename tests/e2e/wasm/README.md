# Browser generation fixtures

These browser tests exercise `src/backends/wasm` candidate preparation and
activation. They are not a demo or a public Nekomata WASM backend. The module
descriptor is private; application state and entry signatures are test-specific.
All persistent state belongs to the main module; side modules have no
side-effecting constructors. Modules remain loaded until the page closes.

Enable from a native host build with an installed Emscripten SDK, Python 3
and Chromium/Chrome. Configuration does not download any dependencies:

```sh
bash scripts/configure.sh debug -DNEKOMATA_TEST_WASM=ON \
  -DNEKOMATA_WASM_BROWSER=/path/to/chrome
bash scripts/build.sh debug --target neko_wasm_fixtures
bash scripts/test.sh debug -R '^neko\.e2e\.wasm\.'
```

`NEKOMATA_EMXX` can select a particular `em++`. The fixtures use identical
explicit flags for the main and side modules. The Python standard-library
runner serves the build directory on loopback and starts headless Chromium
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
update entry and a missing artifact, in that order. Each must produce its
expected backend rejection classification without changing the active module.
The world must continue advancing under B for at least three frames after
each rejection. This validates cooperative fixture activation, not arbitrary
module initialization rollback, thread safety or production ABI validation.
The incompatible fixture deliberately retains the same descriptor prefix;
the version check happens after loading, before accessing its function entries.

No `neko::reload_session` or public WASM backend is introduced here. Remote
generation streams, artifact digests, pre-instantiation ABI metadata and old
module reclamation are subsequent work. The loopback server only supplies
immutable test artifacts and receives the browser's assertion result.

The `wasm` CI job installs Emscripten 6.0.9 and runs these tests in the Ubuntu
runner's Chrome. Candidate ownership and validation tests also run in the
ordinary native and sanitizer jobs. CI uploads CTest diagnostics on failure.

`candidate_lifetime` drives actual late loader callbacks after cancellation,
candidate destruction and loader destruction, then checks synchronous cached
completion and ready-candidate cancellation. Finally it destroys every C++
owner of a committed module and calls a saved entry to verify page residency.
The server holds the first response until a browser frame releases it, so the
cancelled request cannot complete early by accident.
