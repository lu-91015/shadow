#!/usr/bin/env bash
# Shadow 0.5 —— 编译自举版测试运行器 run_tests.shadow，产出 build/linux/rtests。
# 与 Windows 端 tools/build_run_tests.shadow（产出 build/run_tests.exe）对称：
# Linux 全量回归统一用它，不再依赖 test/run_tests.py（Python 版已移除）。
#
# 前置：build/linux/shadow 及 build/linux/{runtime_for_selfhost,miniz,sys_exec_cpa,
#        rt_proc_spawn,shadow_index,shadow_gc_supplement}.o 必须已由
#        tools/routeA_linux.sh 产出（自举生成的编译器 + 现编 runtime）。
#        rt_test_par.o 在本脚本内从 rt/rt_test_par.c 现编。
#
# 用法（仓库根）：
#   bash tools/build_run_tests_linux.sh
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"
L="$ROOT/build/linux"
LLC="$(command -v llc || echo /usr/lib/llvm-20/bin/llc)"
export LLVM_HOME=/usr/lib/llvm-20
[ -d "$ROOT/build/.lu" ] || mkdir -p "$ROOT/build"
export SHADOW_LU_CACHE_DIR="$ROOT/build/.lu_rtestbuild"
mkdir -p "$SHADOW_LU_CACHE_DIR" "$L"

need() { for f in "$@"; do [ -f "$f" ] || { echo "[build_run_tests_linux] 缺少 $f —— 先跑 tools/routeA_linux.sh"; exit 1; }; done; }
need "$L/shadow" "$L/runtime_for_selfhost.o" "$L/miniz.o" "$L/sys_exec_cpa.o" \
     "$L/rt_proc_spawn.o" "$L/shadow_index.o" "$L/shadow_gc_supplement.o"

echo "[1/3] compile test/run_tests.shadow -> rtests.ll ($L/shadow)"
if ! "$L/shadow" "$ROOT/test/run_tests.shadow" -o "$L/rtests.ll" 2>"$ROOT/build/.lu_rtestbuild/compile.log"; then
  echo "[build_run_tests_linux] run_tests.shadow 编译失败，见 build/.lu_rtestbuild/compile.log"; exit 1
fi

echo "[2/3] llc rtests.ll -> rtests.o"
"$LLC" -O0 -mtriple=x86_64-unknown-linux-gnu -filetype=obj -o "$L/rtests.o" "$L/rtests.ll" || exit 1

echo "[3/3] compile rt_test_par.c + link rtests"
clang -c "$ROOT/rt/rt_test_par.c" -o "$L/rt_test_par.o" -I"$ROOT/rt" -std=c11 -fPIC -O1 || exit 1
clang++ -no-pie -O1 -fPIC -rdynamic \
  "$L/rtests.o" "$L/runtime_for_selfhost.o" "$L/miniz.o" "$L/sys_exec_cpa.o" \
  "$L/rt_proc_spawn.o" "$L/shadow_index.o" "$L/rt_test_par.o" "$L/shadow_gc_supplement.o" \
  -o "$L/rtests" \
  -L/usr/lib/llvm-20/lib -Wl,--no-as-needed -lLLVM-20 -lcurl -lpthread -ldl -lm || exit 1
echo "done -> $L/rtests ($(stat -c%s "$L/rtests") bytes)"