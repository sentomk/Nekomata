# Browser generation fixtures

These are test fixtures for Emscripten runtime linking, not a demo or a
supported Nekomata WASM backend. The descriptor is a private test contract.
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

The suite checks that two concurrently loaded modules with the same export
name resolve to their own descriptors and callable code via distinct handles.
This is browser execution evidence, not native host execution of WASM.
