#!/usr/bin/env bash
# Builds the persistent main module once. The reloadable side modules are
# built by publish.py per generation.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
build="$here/.build"
public="$here/public"

emxx="${NEKOMATA_EMXX:-$(command -v emxx || true)}"
emcc="${NEKOMATA_EMCC:-$(command -v emcc || true)}"
if [[ -z "$emxx" || -z "$emcc" ]]; then
  echo "emcc/em++ not found; install Emscripten or set NEKOMATA_EMXX/NEKOMATA_EMCC" >&2
  exit 1
fi

mkdir -p "$build" "$public/offers/modules"

flags=(-std=c++20 -O0 -g -Wall -Wextra -Werror -sASSERTIONS=2
  -sMAIN_MODULE=1 -sENVIRONMENT=web -sALLOW_MEMORY_GROWTH=1 -sFETCH=1 "-I$root/src" "-I$here")
# Exceptions stay enabled for the main module: the offer codec reports
# malformed manifests by throwing. The side module keeps the fixture
# dialect (-fno-exceptions) in publish.py.

# The SHA-256 implementation is C; compile it as C rather than letting the
# C++ driver reject the .c input.
"$emcc" -c -O2 -Wall -Wextra -Werror "$root/src/base/c/neko_sha256.c" -o "$build/neko_sha256.o"

"$emxx" "${flags[@]}" \
  "$here/main.cpp" \
  "$root/src/backends/wasm/candidate.cpp" \
  "$root/src/backends/wasm/offer_poller.cpp" \
  "$root/src/backends/wasm/session.cpp" \
  "$root/src/backends/wasm/emscripten_loader.cpp" \
  "$root/src/protocol/wasm_offer.cpp" \
  "$build/neko_sha256.o" \
  -o "$public/main.js"

cp "$here/index.html" "$public/index.html"
echo "main module built into $public"
