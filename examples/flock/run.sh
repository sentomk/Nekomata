#!/usr/bin/env bash
# One command: build Nekomata for the browser, build the page, publish the
# first behavior generation, then serve the page and republish on every save.
set -euo pipefail

source "$(dirname "${BASH_SOURCE[0]}")/scripts/setup.sh"
port="${PORT:-8940}"

run_step "page: configure" cmake -S "$root" -B "$build/web" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$toolchain" -DCMAKE_BUILD_TYPE=Release \
  -Dnekomata_DIR="$nekomata_dir" -DNEKOMATA_PUBLISHER_EXECUTABLE="$publisher"
run_step "page: build" cmake --build "$build/web" --target site
run_step "first generation: publish" cmake --build "$build/web" --target flock_reload

exec python3 "$root/scripts/dev.py" --build "$build/web" --port "$port"
