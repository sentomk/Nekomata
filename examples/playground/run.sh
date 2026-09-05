#!/usr/bin/env bash
# Build demo.cpp/main.cpp and start the playground animation.
# Requires a Debug build of Nekomata at NEKO_ROOT (default: ~/Nekomata).
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
NEKO_ROOT="${NEKO_ROOT:-$HOME/Nekomata}"
CXX="${CXX:-g++}"
INC="-I$NEKO_ROOT/include"
# Keep these demo.cpp flags identical to those in reload.sh.
HOT_FLAGS="-std=c++20 -O0 -fno-pie -fno-pic -fno-exceptions -fno-asynchronous-unwind-tables"

"$CXX" $HOT_FLAGS $INC -c "$HERE/demo.cpp" -o "$HERE/demo.orig.o"
"$CXX" -std=c++20 -O0 -fno-pie -fno-pic $INC -c "$HERE/main.cpp" -o "$HERE/main.o"
"$CXX" "$HERE/main.o" "$HERE/demo.orig.o" -no-pie \
  "$NEKO_ROOT/build/debug/src/libneko.a" \
  "$NEKO_ROOT/build/debug/src/backends/elf/libneko_backend_elf.a" -ldl -lm \
  -o "$HERE/demo"

echo "[run.sh] starting demo — terminal >= 80x24 recommended"
exec "$HERE/demo" "$HERE/demo.new.o"
