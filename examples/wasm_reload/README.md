# Browser hot-reload demo

A minimal visible loop over the public `neko::reload_session` through the real
CMake integration — the same shape as the native backends. A page owns a
world, the build-discovered groups poll `offers/latest`, and every
`ball_reload` build publishes a new behavior generation on the existing
world. The backend updates the PLT slots at `session.update()`, so the page's
ordinary `demo::update_world()` call reaches the active generation. The trail
color marks each generation and the tick counter never restarts.

This is a development demo, not test infrastructure. The page drives the
public session and PLT header, and links the installed Emscripten library.

## Run it

Requirements: Emscripten (`emcc`/`em++` on `PATH`, or `EMSDK`), Python 3, any
browser, and one native configure of this repository (`bash
scripts/configure.sh debug`) providing the host publisher. The script builds
and installs a matching Emscripten library in the demo build directory.

```sh
bash run_demo.sh            # terminal 1: configures, builds, serves the page
```

Open the printed URL (default `http://127.0.0.1:8931/`). The ball waits
until the first generation arrives — `run_demo.sh` publishes one already.
Then iterate from another terminal:

```sh
build=examples/wasm_reload/build
cmake --build $build --target ball_reload                    # republish hot.cpp
cmake -S examples/wasm_reload -B $build -DDEMO_BEHAVIOR=2 && cmake --build $build --target ball_reload
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
  `ENTRIES` are declared here; `EXPORT_HEADER` and `EXPORT_NAMESPACE` let the
  adapter build the side-module descriptor from `hot.hpp`. `hot.cpp` contains
  behavior only. Candidate validation rejects drift without exposing the
  descriptor layout to application code.
- The page links `nekomata::neko` and calls `session.watch()`
  to start event-loop polling. `session.update()` once per frame is the safe
  point: it activates a ready candidate without starting a fetch, and the
  frame calls through the registered PLT slots.
  `unwatch()` pauses polling and activation while preserving the running
  behavior, pending candidate and consumer cursor; `watch()` resumes them.
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
