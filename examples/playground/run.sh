#!/usr/bin/env bash
# 启动 playground：编译 demo.cpp/main.cpp 并以动画模式运行。
# 依赖：~/Nekomata 已构建（cmake --preset debug）。
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
NEKO_ROOT="${NEKO_ROOT:-$HOME/Nekomata}"
CXX="${CXX:-g++}"
INC="-I$NEKO_ROOT/include"
# 注意：demo.cpp 必须与当初链接进二进制的编译命令完全一致（build information）
HOT_FLAGS="-std=c++20 -O0 -fno-pie -fno-pic -fno-exceptions -fno-asynchronous-unwind-tables"

"$CXX" $HOT_FLAGS $INC -c "$HERE/demo.cpp" -o "$HERE/demo.orig.o"
"$CXX" -std=c++20 -O0 -fno-pie -fno-pic $INC -c "$HERE/main.cpp" -o "$HERE/main.o"
"$CXX" "$HERE/main.o" "$HERE/demo.orig.o" -no-pie \
  "$NEKO_ROOT/build/debug/src/libneko.a" \
  "$NEKO_ROOT/build/debug/src/backends/elf/libneko_backend_elf.a" -ldl -lm \
  -o "$HERE/demo"

echo "[run.sh] starting demo — terminal >= 80x24 recommended"
exec "$HERE/demo" "$HERE/demo.new.o"
