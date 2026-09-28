# Shared by run.sh and scripts/build_site.sh. Sourcing this locates the
# Emscripten toolchain, builds a native publisher and installs the browser
# library from the enclosing Nekomata checkout, then leaves these for the
# caller: root nekomata build log toolchain publisher nekomata_dir run_step

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
nekomata="$(cd "$root/../.." && pwd)"
build="$root/build"
log="$build/run.log"
mkdir -p "$build"
: > "$log"

run_step() {
  local label="$1"
  shift
  printf '  %-44s' "$label"
  if "$@" >> "$log" 2>&1; then
    echo "done"
  else
    echo "failed"
    tail -n 40 "$log" >&2
    echo "full log: $log" >&2
    exit 1
  fi
}

toolchain="${NEKOMATA_TOOLCHAIN_FILE:-}"
if [[ -z "$toolchain" ]]; then
  emcc_path="$(command -v emcc || true)"
  if [[ -n "$emcc_path" ]]; then
    emcc_real="$(realpath "$emcc_path" 2>/dev/null || echo "$emcc_path")"
    for candidate in \
        "$(dirname "$emcc_real")/../libexec/cmake/Modules/Platform/Emscripten.cmake" \
        "$(dirname "$emcc_real")/cmake/Modules/Platform/Emscripten.cmake"; do
      if [[ -f "$candidate" ]]; then
        toolchain="$(cd "$(dirname "$candidate")" && pwd)/Emscripten.cmake"
        break
      fi
    done
  fi
fi
if [[ -z "$toolchain" && -n "${EMSDK:-}" ]]; then
  candidate="$EMSDK/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake"
  [[ -f "$candidate" ]] && toolchain="$candidate"
fi
if [[ -z "$toolchain" ]]; then
  echo "cannot locate the Emscripten CMake toolchain; install Emscripten or set EMSDK" >&2
  exit 1
fi

echo "nekomata flock — using Nekomata at $nekomata"

run_step "native publisher: configure" cmake -S "$nekomata" -B "$build/publisher" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DNEKOMATA_BUILD_TESTS=OFF -DNEKOMATA_BUILD_EXAMPLES=OFF \
  -DNEKOMATA_BUILD_TOOLS=ON
run_step "native publisher: build" cmake --build "$build/publisher" --target neko_publisher
publisher="$build/publisher/tools/neko_publisher/neko_publisher"

run_step "browser library: configure" cmake -S "$nekomata" -B "$build/nekomata" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$toolchain" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$build/nekomata-install" \
  -DNEKOMATA_BUILD_TESTS=OFF -DNEKOMATA_BUILD_TOOLS=OFF -DNEKOMATA_BUILD_EXAMPLES=OFF
run_step "browser library: build and install" cmake --build "$build/nekomata" --target install
nekomata_dir="$build/nekomata-install/lib/cmake/nekomata"
