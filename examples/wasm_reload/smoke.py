#!/usr/bin/env python3
"""Headless smoke check: the page must apply a published generation and
advance the world. Serves public/, launches the browser the e2e suite uses,
and waits for the page to report its smoke verdict."""

import pathlib
import subprocess
import sys
import tempfile
import time

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import serve  # noqa: E402

import http.server


def main():
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), serve.demo_handler)
    server.last_smoke = None
    import functools
    server.RequestHandlerClass = functools.partial(serve.demo_handler,
                                                    directory=str(serve.ROOT))
    import threading
    threading.Thread(target=server.serve_forever, daemon=True).start()
    port = server.server_address[1]

    browser = None
    profile = tempfile.mkdtemp(prefix="neko-demo-smoke-")
    try:
        candidates = ["/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
                      "google-chrome", "chromium", "chromium-browser"]
        browser_path = next((c for c in candidates if pathlib.Path(c).exists()
                             or subprocess.run(["which", c], capture_output=True).returncode == 0),
                            None)
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
            print("smoke: ok — a polled generation applied and the world advanced")
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
