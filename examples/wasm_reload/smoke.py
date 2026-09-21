#!/usr/bin/env python3
"""Headless smoke check through the CMake integration: builds the page and
publishes one generation with the `ball_reload` target, then verifies in a
real browser that the polled generation applied and the world advanced."""

import pathlib
import subprocess
import sys
import tempfile
import time

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent.parent


def main():
    build = HERE / "build"
    if not (build / "build.ninja").exists():
        sys.exit("demo not configured; run bash run_demo.sh once first")

    subprocess.run(["cmake", "--build", str(build), "--target", "ball_reload"], check=True,
                   stdout=subprocess.DEVNULL)

    sys.path.insert(0, str(HERE))
    import functools
    import http.server
    import serve

    public = build / "public"
    handler = functools.partial(serve.demo_handler, directory=str(public))
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
    server.last_smoke = None
    import threading
    threading.Thread(target=server.serve_forever, daemon=True).start()
    port = server.server_address[1]

    browser = None
    profile = tempfile.mkdtemp(prefix="neko-demo-smoke-")
    try:
        candidates = ["/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
                      "google-chrome", "chromium", "chromium-browser"]
        browser_path = None
        for candidate in candidates:
            probe = pathlib.Path(candidate)
            if probe.exists():
                browser_path = str(probe)
                break
            if subprocess.run(["which", candidate], capture_output=True).returncode == 0:
                browser_path = candidate
                break
        if not browser_path:
            sys.exit("no Chrome/Chromium found")

        url = f"http://127.0.0.1:{port}/?smoke"
        browser = subprocess.Popen(
            [browser_path, "--headless", "--disable-gpu", "--no-first-run",
             "--no-default-browser-check", "--disable-background-networking",
             f"--user-data-dir={profile}", url],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

        deadline = time.monotonic() + 40
        while server.last_smoke is None and time.monotonic() < deadline:
            time.sleep(0.1)
        verdict = server.last_smoke
        if verdict == "ok":
            print("smoke: ok — a CMake-published generation applied and the world advanced")
            return 0
        print(f"smoke: {verdict or 'timed out'}")
        return 1
    finally:
        if browser is not None:
            browser.terminate()
            try:
                browser.wait(timeout=5)
            except subprocess.TimeoutExpired:
                browser.kill()
        server.shutdown()
        server.server_close()


if __name__ == "__main__":
    sys.exit(main())
