#!/usr/bin/env bash
# Build the static site for GitHub Pages into build/site. The page and every
# tour generation are compiled here, ahead of time; the site needs no server
# beyond static files. Preview with: python3 -m http.server -d build/site
set -euo pipefail

source "$(dirname "${BASH_SOURCE[0]}")/setup.sh"

# Generations are built by editing sources, so work on a scratch copy.
work="$build/site-work"
rm -rf "$work"
mkdir -p "$work/project"
cp -R "$root/CMakeLists.txt" "$root/src" "$root/web" "$work/project/"

run_step "site page: configure" cmake -S "$work/project" -B "$work/build" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$toolchain" -DCMAKE_BUILD_TYPE=Release \
  -Dnekomata_DIR="$nekomata_dir" -DNEKOMATA_PUBLISHER_EXECUTABLE="$publisher"
run_step "site page: build" cmake --build "$work/build" --target site
echo "  tour generations:"
python3 "$root/scripts/generations.py" --project "$work/project" --build "$work/build" \
  --out "$build/site"
echo "site ready in $build/site"
