#!/usr/bin/env bash
# 改完 demo.cc 后执行：重编译并原子投递，动画立刻热替换。
# 编译失败则什么都不投递，动画继续跑旧代码。
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
NEKO_ROOT="${NEKO_ROOT:-$HOME/Nekomata}"
CXX="${CXX:-g++}"

if "$CXX" -std=c++20 -O0 -fno-pie -fno-pic -fno-exceptions \
     -fno-asynchronous-unwind-tables -I"$NEKO_ROOT/include" \
     -c "$HERE/demo.cc" -o "$HERE/demo.staging.o"; then
    mv "$HERE/demo.staging.o" "$HERE/demo.new.o"  # 原子投递 = 显式确认
    echo "[reload.sh] fresh demo.o offered — next frame takes it"
else
    echo "[reload.sh] compile failed — nothing offered, animation keeps old code"
fi
