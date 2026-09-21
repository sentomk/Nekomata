# Browser hot-reload demo

A minimal visible loop over the private WASM backend through the real CMake
integration: a page owns a world, `neko::wasm::reload_session` polls
`offers/latest`, and every `ball_reload` build publishes a new behavior
generation on the existing world. The trail color marks each generation and
the tick counter never restarts — that is the whole point.

This is a development demo, not a supported application API and not test
infrastructure; the headers under `src/backends/wasm` stay private.

## Run it

Requirements: Emscripten (`emcc`/`em++` on `PATH`, or `EMSDK`), Python 3, any
browser, and one native configure of this repository (`bash
scripts/configure.sh debug`) providing the host publisher.

```sh
bash run_demo.sh            # terminal 1: configures, builds, serves the page
```

Open the printed URL (default `http://127.0.0.1:8931/`). The ball waits
until the first generation arrives — `run_demo.sh` publishes one already.
Then iterate from another terminal:

```sh
build=examples/wasm_reload/build
cmake --build $build --target ball_reload                    # republish hot.cpp
cmake -B $build -DDEMO_BEHAVIOR=2 && cmake --build $build --target ball_reload
```

Behavior 1 is a linear bounce, 2 gravity arcs, 3 a spring pull toward the
center; editing `hot.cpp` directly is the same loop. The HUD reports the
active behavior, the ever-growing tick count, and applied/rejected counts;
the log lists delivery events (acceptance, staleness, conflicts) and
rejections with their classification.

## How it maps to the integration

- `nekomata_add_reload_group(ball ...)` under the Emscripten toolchain links
  the group's units into one side module (`-sSIDE_MODULE=2` compile,
  `-sSIDE_MODULE=1` link) and drives the host publisher's `wasm` mode: the
  artifact lands at an immutable sequence-named path below `offers/modules/`
  and the manifest atomically replaces `offers/latest`. `ABI_ID` and
  `ENTRIES` are declared here; the side module's descriptor must match them,
  and the candidate validation rejects drift.
- The page links the backend sources directly and calls
  `session.update()` once per frame. That call is the safe point: the poll
  happens inside it, a ready candidate activates inside it, and the frame
  resolves every entry through the one `current()` snapshot.
- Each artifact is verified against the manifest's SHA-256 before
  instantiation by `emscripten_loader`; a tampered digest rejects as an
  integrity failure without touching the running behavior.

A headless smoke check exists for quick verification without eyes — it
republishes through the `ball_reload` target and reports whether a polled
generation applied and the world advanced:

```sh
bash run_demo.sh &   # once, to configure and build
python3 smoke.py
```
