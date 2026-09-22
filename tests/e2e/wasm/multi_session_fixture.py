"""Independent CMake streams with an explicit late-completion gate for alpha."""

import pathlib
import shutil
import subprocess
import tempfile
import threading

from session_fixture import publish_generation


class multi_session_fixture:
    def __init__(self, args):
        self.args = args
        self.storage = tempfile.TemporaryDirectory(prefix="neko-wasm-multi-session-")
        self.root = pathlib.Path(self.storage.name)
        self.public = self.root / "public"
        self.lock = threading.Lock()
        self.generation = 0
        self.gated_path = None
        self.requested = threading.Event()
        self.released = threading.Event()
        try:
            self.public.mkdir()
            for name in ("index.html", args.runner + ".js", args.runner + ".wasm"):
                shutil.copy2(args.root / name, self.public / name)
            self.publish(1)
        except BaseException:
            self.close()
            raise

    def publish(self, generation):
        with self.lock:
            if generation != self.generation + 1:
                raise RuntimeError("unexpected multi-group publication order")
            for group in ("alpha", "beta"):
                artifact = publish_generation(
                    self.args, self.root / group, self.public / group, generation,
                    2 if group == "alpha" and generation == 3 else 1, group)
                if group == "alpha" and generation == 2:
                    self.gated_path = f"/alpha/{artifact}"
            self.generation = generation

    def before_get(self, handler):
        if handler.path in ("/alpha/latest", "/beta/latest"):
            # Both manifests are hidden until publication and gate setup finish.
            with self.lock:
                body = (self.public / handler.path.lstrip("/")).read_bytes()
            handler.send_response(200)
            handler.send_header("Content-Type", "text/plain")
            handler.send_header("Cache-Control", "no-store")
            handler.send_header("Content-Length", str(len(body)))
            handler.end_headers()
            handler.wfile.write(body)
            return True
        if handler.path == self.gated_path:
            self.requested.set()
            if not self.released.wait(timeout=30):
                handler.send_error(504, "browser did not release alpha")
                return True
        return False

    def post(self, handler):
        if handler.path not in ("/publish-second", "/publish-third", "/release-alpha"):
            return False
        try:
            if handler.path == "/release-alpha":
                if not self.requested.wait(timeout=30):
                    raise RuntimeError("alpha artifact was never requested")
                self.released.set()
            else:
                self.publish(2 if handler.path == "/publish-second" else 3)
            handler.send_response(204)
            handler.end_headers()
        except (OSError, RuntimeError, subprocess.SubprocessError) as error:
            print(str(error), flush=True)
            handler.send_error(500, "multi-group fixture publication or gate failed")
        return True

    def close(self):
        self.released.set()
        with self.lock:
            self.storage.cleanup()
