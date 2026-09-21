# Browser hot-reload demo

A minimal visible loop over the private WASM backend: a page owns a world,
`neko::wasm::reload_session` polls `offers/latest`, and every publish swaps
the ball's physics on the existing world. The trail color marks each
generation and the tick counter never restarts — that is the whole point.

This is a development demo, not a supported application API and not test
infrastructure; the headers under `src/backends/wasm` stay private.

## Run it

Requirements: Emscripten (`emcc`/`em++` on `PATH` or via
`NEKOMATA_EMXX`/`NEKOMATA_EMCC`), Python 3, any browser.

```sh
bash run_demo.sh            # terminal 1: builds the main module, serves the page
```

Open the printed URL (default `http://127.0.0.1:8931/`). The ball waits
until the first generation arrives:

```sh
python3 publish.py 1        # terminal 2: linear bounce — the ball starts moving
python3 publish.py 2        # gravity arcs, on the same world
python3 publish.py 3        # spring pull toward the center
python3 publish.py 1        # and back; the trail shows every switch
```

The HUD reports the active behavior, the ever-growing tick count, and the
count of applied and rejected generations; the log lists delivery events
(acceptance, staleness, conflicts) and rejections with their classification.

## How it maps to the backend

- `publish.py` compiles `hot.cpp` with a fixed flag set, writes the artifact
  to an immutable sequence-named path, and atomically replaces
  `offers/latest` — the manifest fetch itself is the ready marker.
- Each artifact is verified against the manifest's SHA-256 before
  instantiation by `emscripten_loader`; a tampered digest would reject as an
  integrity failure without touching the running behavior.
- `main.cpp` calls `session.update()` once per frame. That call is the safe
  point: the poll happens inside it, a ready candidate activates inside it,
  and the frame resolves every entry through the one `current()` snapshot.
- Behaviors share the `demo-ball-v1` ABI identity. Publishing a module with
  a different `abi_id` or entry set would be rejected, leaving the previous
  behavior active.

A headless smoke check exists for quick verification without eyes — it
applies the newest published generation and reports whether the world
advanced:

```sh
python3 publish.py 1   # have at least one generation published
python3 smoke.py
```
