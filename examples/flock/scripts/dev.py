#!/usr/bin/env python3
"""Serve the flock page and republish the behavior generation on every save.

The page polls offers/latest, so the manifest must never be cached; module
artifacts live at immutable paths and may be. Saving src/flock.cpp rebuilds
and publishes a new generation. Saving src/world.hpp or src/flock.hpp changes
the contract hash, so the running page rejects that generation as
incompatible and keeps its current behavior; reload the page to adopt a new
contract.
"""

import argparse
import functools
import http.server
import pathlib
import subprocess
import sys
import threading
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
WATCHED = [ROOT / "src" / "flock.cpp", ROOT / "src" / "flock.hpp", ROOT / "src" / "world.hpp"]


class handler(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        if self.path.startswith("/offers/") and not self.path.startswith("/offers/modules/"):
            self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def log_message(self, fmt, *args):
        pass


def stamp():
    return time.strftime("%H:%M:%S")


def mtimes():
    return {path: path.stat().st_mtime_ns for path in WATCHED if path.exists()}


def publish(build, changed):
    names = ", ".join(sorted(path.relative_to(ROOT).as_posix() for path in changed))
    print(f"{stamp()}  {names} changed — rebuilding", flush=True)
    started = time.monotonic()
    result = subprocess.run(["cmake", "--build", str(build), "--target", "flock_reload"],
                            capture_output=True, text=True)
    elapsed = time.monotonic() - started
    if result.returncode == 0:
        print(f"{stamp()}  published in {elapsed:.1f}s — the page switches at its next frame",
              flush=True)
        return
    output = (result.stdout + result.stderr).strip().splitlines()
    errors = [line for line in output if "error" in line or line.startswith(" ")]
    print("\n".join((errors or output)[-30:]), file=sys.stderr)
    print(f"{stamp()}  build failed — nothing published, the page keeps its current generation",
          flush=True)


def watch(build, interval):
    seen = mtimes()
    while True:
        time.sleep(interval)
        if mtimes() == seen:
            continue
        # Editors often write in several steps; let the files settle first.
        time.sleep(0.15)
        current = mtimes()
        changed = [path for path, value in current.items() if seen.get(path) != value]
        seen = current
        if changed:
            publish(build, changed)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build", type=pathlib.Path, default=ROOT / "build" / "web")
    parser.add_argument("--port", type=int, default=8940)
    parser.add_argument("--no-watch", action="store_true", help="serve only")
    args = parser.parse_args()

    public = args.build / "public"
    if not (public / "main.js").exists():
        sys.exit(f"{public}/main.js is missing; run examples/flock/run.sh first")

    server = http.server.ThreadingHTTPServer(
        ("127.0.0.1", args.port), functools.partial(handler, directory=str(public)))
    threading.Thread(target=server.serve_forever, daemon=True).start()
    print(f"\n  open http://127.0.0.1:{args.port}/", flush=True)
    if args.no_watch:
        print("  serving only; publish with: cmake --build build/web --target flock_reload\n",
              flush=True)
    else:
        print("  edit src/flock.cpp and save — Ctrl-C stops\n", flush=True)
    try:
        if args.no_watch:
            threading.Event().wait()
        else:
            watch(args.build, 0.2)
    except KeyboardInterrupt:
        pass
    finally:
        server.shutdown()


if __name__ == "__main__":
    main()
