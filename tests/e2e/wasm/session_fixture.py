"""Publish real CMake generations and gate the browser's in-flight B artifact."""

import pathlib
import shlex
import shutil
import subprocess
import tempfile
import threading


def publish_generation(args, build, offer_root, generation, interface_version, group_id="flock"):
    commands = [
        [args.emcmake, args.cmake,
         "-S", str(pathlib.Path(__file__).parent / "session_project"),
         "-B", str(build), "-G", "Ninja",
         f"-DCMAKE_MAKE_PROGRAM={args.ninja}",
         f"-DNEKOMATA_PUBLISHER_EXECUTABLE={args.publisher}",
         f"-DOFFER_ROOT={offer_root}", f"-DGROUP_ID={group_id}",
         f"-DGENERATION_ID={generation}", f"-DINTERFACE_VERSION={interface_version}"],
        [args.cmake, "--build", str(build), "--target", "flock_reload"],
    ]
    for command in commands:
        result = subprocess.run(command, capture_output=True, text=True, timeout=30)
        if result.returncode:
            raise RuntimeError(f"command failed: {command}\n{result.stdout}\n{result.stderr}")
    # Read only generated fixture fields; production parsing stays in C++.
    rows = [shlex.split(line) for line in (offer_root / "latest").read_text().splitlines()]
    fields = {row[0]: row[1:] for row in rows if len(row) > 1}
    if int(fields["sequence"][0]) != generation or fields["group_id"][0] != group_id:
        raise RuntimeError(f"unexpected publication identity: {fields}")
    return fields["artifact"][0]


class session_fixture:
    def __init__(self, args):
        self.args = args
        self.storage = tempfile.TemporaryDirectory(prefix="neko-wasm-session-")
        self.root = pathlib.Path(self.storage.name)
        self.public = self.root / "public"
        self.build = self.root / "build"
        self.b_path = None
        self.b_requested = threading.Event()
        self.release_b = threading.Event()
        self.lock = threading.Lock()
        self.generation = 0
        try:
            self.public.mkdir()
            for name in ("index.html", "session.js", "session.wasm"):
                shutil.copy2(args.root / name, self.public / name)
            self.publish(1)
        except BaseException:
            self.close()
            raise

    def publish(self, generation):
        with self.lock:
            if generation != self.generation + 1:
                raise RuntimeError("unexpected publication order")
            artifact = publish_generation(
                self.args, self.build, self.public / "offers", generation,
                2 if generation == 3 else 1)
            if generation == 2:
                self.b_path = "/offers/" + artifact
            self.generation = generation

    def before_get(self, handler):
        # The manifest is served through the same lock as publication, so B's
        # gate exists before a poller can discover its immutable artifact URL.
        if handler.path == "/offers/latest":
            with self.lock:
                body = (self.public / "offers/latest").read_bytes()
            handler.send_response(200)
            handler.send_header("Content-Type", "text/plain")
            handler.send_header("Cache-Control", "no-store")
            handler.send_header("Content-Length", str(len(body)))
            handler.end_headers()
            handler.wfile.write(body)
            return True
        if handler.path == self.b_path:
            self.b_requested.set()
            if not self.release_b.wait(timeout=30):
                handler.send_error(504, "browser did not release B")
                return True
        return False

    def post(self, handler):
        if handler.path not in ("/publish-b", "/publish-c", "/release-b"):
            return False
        try:
            if handler.path == "/release-b":
                if not self.b_requested.wait(timeout=30):
                    raise RuntimeError("B artifact was never requested")
                self.release_b.set()
            else:
                self.publish(2 if handler.path == "/publish-b" else 3)
            handler.send_response(204)
            handler.end_headers()
        except (OSError, RuntimeError, subprocess.SubprocessError) as error:
            print(str(error), flush=True)
            handler.send_error(500, "fixture publication or gate failed")
        return True

    def close(self):
        self.release_b.set()
        # A failed browser may exit while a request is still building C.
        with self.lock:
            self.storage.cleanup()
