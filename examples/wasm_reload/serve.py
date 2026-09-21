#!/usr/bin/env python3
"""Serves the demo page and its published generations on loopback.

The polled manifest lives at offers/latest and changes between generations,
so responses under /offers/ are marked non-cacheable; artifact paths under
offers/modules/ are immutable and cache freely.
"""

import argparse
import functools
import http.server
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent / "public"


class demo_handler(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        if self.path.startswith("/offers/") and not self.path.startswith("/offers/modules/"):
            self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def do_POST(self):
        if self.path == "/smoke-result":
            length = int(self.headers.get("Content-Length", 0))
            body = self.rfile.read(length).decode()
            self.server.last_smoke = body
            self.send_response(204)
            self.end_headers()
            return
        self.send_error(404)

    def log_message(self, fmt, *args):
        sys.stderr.write("serve: %s\n" % (fmt % args))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8931)
    args = parser.parse_args()
    if not (ROOT / "main.js").exists():
        sys.exit("public/main.js is missing; run bash run_demo.sh (or build_main.sh) first")
    handler = functools.partial(demo_handler, directory=str(ROOT))
    server = http.server.ThreadingHTTPServer(("127.0.0.1", args.port), handler)
    server.last_smoke = None
    print(f"open http://127.0.0.1:{args.port}/ then publish: python3 publish.py <1|2|3>",
          flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
