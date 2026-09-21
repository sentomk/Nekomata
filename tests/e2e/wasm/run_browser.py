"""Run the fixture in an isolated Chromium process; no third-party packages."""

import argparse
import functools
import http.server
import json
import pathlib
import shutil
import subprocess
import sys
import tempfile
import threading
import time


def release_profile(path):
    """Remove the browser profile, tolerating Chrome's lingering children.

    Headless Chrome's helper processes can outlive the reaped browser and
    keep writing the profile directory; a cleanup race here must not fail
    a test whose assertions already passed."""
    for _ in range(5):
        try:
            shutil.rmtree(path)
            return
        except OSError:
            time.sleep(0.2)
    shutil.rmtree(path, ignore_errors=True)
    print(f"warning: browser profile at {path} could not be fully removed",
          file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--browser", required=True)
    parser.add_argument("--root", required=True, type=pathlib.Path)
    parser.add_argument("--runner", choices=("runner", "lifecycle", "scheduler"), default="runner")
    args = parser.parse_args()
    completed = threading.Event()
    a_frame = threading.Event()
    lifecycle_frame = threading.Event()
    results = []

    class fixture_handler(http.server.SimpleHTTPRequestHandler):
        def log_message(self, *_args):
            pass

        def do_GET(self):
            if self.path in ("/late.wasm", "/abandoned.wasm"):
                if not lifecycle_frame.wait(timeout=10):
                    self.send_error(504, "lifecycle frame did not run")
                    return
            if self.path == "/b.wasm":
                # Release B only after a real A frame, independent of machine speed.
                if not a_frame.wait(timeout=10):
                    self.send_error(504, "A did not advance while B was pending")
                    return
            super().do_GET()

        def do_POST(self):
            if self.path == "/lifecycle-frame":
                lifecycle_frame.set()
                self.send_response(204)
                self.end_headers()
                return
            if self.path == "/a-frame":
                a_frame.set()
                self.send_response(204)
                self.end_headers()
                return
            if self.path != "/result":
                self.send_error(404)
                return
            try:
                length = int(self.headers.get("Content-Length", "0"))
                if not 0 < length <= 16384:
                    raise ValueError("invalid result size")
                result = json.loads(self.rfile.read(length))
                if not isinstance(result, dict) or type(result.get("ok")) is not bool:
                    raise ValueError("invalid result")
                results.append(result)
            except (ValueError, UnicodeError) as error:
                results.append({"ok": False, "message": str(error)})
            self.send_response(204)
            self.end_headers()
            completed.set()

    handler = functools.partial(fixture_handler, directory=str(args.root.resolve()))
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    profile = None
    try:
        profile = tempfile.mkdtemp(prefix="neko-wasm-browser-")
        with tempfile.TemporaryFile(mode="w+") as browser_log:
            browser = subprocess.Popen([
                args.browser, "--headless", "--disable-gpu", "--no-first-run",
                "--no-default-browser-check", "--disable-background-networking",
                f"--user-data-dir={profile}",
                f"http://127.0.0.1:{server.server_port}/index.html?runner={args.runner}",
            ], stdout=browser_log, stderr=browser_log)
            try:
                deadline = time.monotonic() + 40
                while not completed.wait(0.1):
                    if browser.poll() is not None or time.monotonic() >= deadline:
                        browser_log.seek(0)
                        raise RuntimeError("browser exited or timed out\n" + browser_log.read())
                result = results[0]
                print(json.dumps(result), flush=True)
                return 0 if result["ok"] else 1
            finally:
                browser.terminate()
                try:
                    browser.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    browser.kill()
                    browser.wait()
    finally:
        if profile is not None:
            release_profile(profile)
        a_frame.set()
        lifecycle_frame.set()
        server.shutdown()
        server.server_close()
        thread.join()


if __name__ == "__main__":
    raise SystemExit(main())
