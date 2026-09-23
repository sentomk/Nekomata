#!/usr/bin/env bash
# One-command demo entrypoint: locates the Emscripten toolchain and a native
# publisher, installs the browser library, builds the page, then serves it. Republish a
# generation from another terminal with:
#   cmake --build examples/wasm_reload/build --target ball_reload
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
build="$here/build"

# The host publisher comes from a native build of this repository.
publisher="${NEKOMATA_PUBLISHER_EXECUTABLE:-$repo/build/debug/tools/neko_publisher/neko_publisher}"
if [[ ! -x "$publisher" ]]; then
  if [[ -f "$repo/build/debug/build.ninja" ]]; then
    cmake --build "$repo/build/debug" --target neko_publisher >/dev/null
  else
    echo "no native publisher at '$publisher' and no configured build/debug;" \
         "run bash scripts/configure.sh debug first, or set NEKOMATA_PUBLISHER_EXECUTABLE" >&2
    exit 1
  fi
fi

toolchain="${NEKOMATA_TOOLCHAIN_FILE:-}"
if [[ -z "$toolchain" ]]; then
  emcc_path="$(command -v emcc || true)"
  if [[ -n "$emcc_path" ]]; then
    emcc_real="$(readlink -f "$emcc_path" 2>/dev/null || realpath "$emcc_path" 2>/dev/null || true)"
    [[ -n "$emcc_real" ]] || emcc_real="$emcc_path"
    candidate="$(dirname "$emcc_real")/../libexec/cmake/Modules/Platform/Emscripten.cmake"
    if [[ -f "$candidate" ]]; then
      toolchain="$(cd "$(dirname "$candidate")" && pwd)/$(basename "$candidate")"
    fi
  fi
fi
if [[ -z "$toolchain" && -n "${EMSDK:-}" && -f "$EMSDK/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake" ]]; then
  toolchain="$EMSDK/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake"
fi
if [[ -z "$toolchain" ]]; then
  echo "cannot locate the Emscripten CMake toolchain; set NEKOMATA_TOOLCHAIN_FILE or EMSDK" >&2
  exit 1
fi

cmake -S "$repo" -B "$build/nekomata-library" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$toolchain" \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_INSTALL_PREFIX="$build/nekomata-install" \
  -DNEKOMATA_BUILD_TESTS=OFF \
  -DNEKOMATA_BUILD_TOOLS=OFF \
  -DNEKOMATA_BUILD_EXAMPLES=OFF
cmake --build "$build/nekomata-library" --target install

cmake -S "$here" -B "$build" -G Ninja \
  -Dnekomata_DIR="$build/nekomata-install/lib/cmake/nekomata" \
  -DCMAKE_TOOLCHAIN_FILE="$toolchain" \
  -DCMAKE_BUILD_TYPE=Debug \
  -DNEKOMATA_PUBLISHER_EXECUTABLE="$publisher"
cmake --build "$build" --target demo_page ball_reload

exec python3 "$here/serve.py" --directory "$build/public" "$@"
