#!/usr/bin/env bash
# Run after editing demo.cpp: recompile and atomically offer the new object.
# If compilation fails, offer nothing and keep the old animation running.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
NEKO_ROOT="${NEKO_ROOT:-$HOME/Nekomata}"
CXX="${CXX:-g++}"

if "$CXX" -std=c++20 -O0 -fno-pie -fno-pic -fno-exceptions \
     -fno-asynchronous-unwind-tables -I"$NEKO_ROOT/include" \
     -c "$HERE/demo.cpp" -o "$HERE/demo.staging.o"; then
    mv "$HERE/demo.staging.o" "$HERE/demo.new.o"  # Atomic delivery confirms the reload.
    echo "[reload.sh] fresh demo.o offered — next frame takes it"
else
    echo "[reload.sh] compile failed — nothing offered, animation keeps old code"
fi
