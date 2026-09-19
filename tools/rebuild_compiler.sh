#!/bin/bash
# 快速重建 build/shadow.exe（dyn/supertrait 迭代用）
# 等价于 bootstrap.sh 的 [stage1]：用当前 build/shadow.exe 把 src/main.shadow 编成 stage1.ll，
# llc + clang++ 链接成 build/shadow.exe。跳过 stage2/3 固定点比对（v6 已证明收敛单级收敛）。
# 用法：bash tools/rebuild_compiler.sh [--purge]
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
[ -f tools/env.local.sh ] && . tools/env.local.sh
: "${LLVM_HOME:=${LLVM_HOME_WIN:-}}"
[ -n "${LLVM_HOME:-}" ] || { echo "[X] LLVM_HOME 未设置且 env.local.sh 无默认"; exit 1; }

if [ "${1:-}" = "--purge" ]; then
  echo "[purge] 清空 .lu 缓存"
  rm -rf build/.lu_* build/.lu 2>/dev/null
  rm -rf build/lu_cache 2>/dev/null
fi

bin="$LLVM_HOME/bin"
rt_lib="$LLVM_HOME/lib"
echo "[1] compile src/main.shadow -> build/stage1.ll (fresh)"
build/shadow.exe src/main.shadow -o build/stage1.ll || { echo "[X] compile failed"; exit 1; }
echo "[2] llc -> build/stage1.o"
"$bin/llc.exe" -O0 -filetype=obj build/stage1.ll -o build/stage1.o || { echo "[X] llc failed"; exit 1; }
echo "[3] clang++ link -> build/shadow.exe"
MSYS2_ARG_CONV_EXCL='*' "$bin/clang++.exe" -std=c++17 \
  build/stage1.o build/rt/runtime_for_selfhost_wk.o build/rt/miniz.o \
  build/rt/shadow_gc_supplement.o build/rt/rt_zip.o build/rt/shadow_index.o \
  build/rt/rt_proc_spawn.o build/rt/sys_exec_cpa.o build/rt/cxa_atexit_shim.o \
  -o build/shadow.exe -Wl,/subsystem:console -Wl,/Brepro -Wl,/stack:8388608 \
  -lws2_32 -lLLVM-C -L"$rt_lib" -Lpkg/shadow/llvm/crt || { echo "[X] link failed"; exit 1; }
echo "REBUILD_OK"