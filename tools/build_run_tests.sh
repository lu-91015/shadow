#!/usr/bin/env bash
# 构建 Shadow 原生测试运行器 build/run_tests.exe
#
# 注意：build/shadow.exe X.shadow -o X.exe 只出 IR、不链接（链接是 build_shadow.sh 的
# 独立步骤）。本脚本复刻 build_shadow.sh 的链接命令，把 test/run_tests.shadow 编成
# 可执行的 build/run_tests.exe。
#
# 用法：
#   export LLVM_HOME="D:/llvm/clang+llvm-22.1.0-x86_64-pc-windows-msvc"
#   export PATH="$LLVM_HOME/bin:$PATH"
#   bash tools/build_run_tests.sh
#   build/run_tests.exe            # 从仓库根启动
set -e
cd "$(dirname "$0")/.."           # 回到仓库根（tools/ 的上一级）
BOOT=bootstrap
: "${LLVM_HOME:?set LLVM_HOME to your LLVM installation (e.g. D:/llvm/clang+llvm-XX)}"
LLVM="$LLVM_HOME"
LLCC="$LLVM/bin/llc.exe"
CLANG="$LLVM/bin/clang++.exe"
CLANG_C="$LLVM/bin/clang.exe"
RT_LIB="$LLVM/lib"

mkdir -p build/rt
echo "[0] precompile rt objects (与 build_shadow.sh [0/7] 一致)"
"$CLANG_C" -O0 -I "$BOOT" -c rt/rt_zip.c -o build/rt/rt_zip.o
"$CLANG_C" -O0 -c rt/shadow_gc_supplement.c -o build/rt/shadow_gc_supplement.o
"$CLANG_C" -O1 -Wall -c rt/shadow_index.c -o build/rt/shadow_index.o
"$CLANG_C" -O0 -I "$BOOT" -c rt/rt_proc_spawn.c -o build/rt/rt_proc_spawn.o

# [FIX] 同 build_shadow.sh：shadow_sys_exec 改用 CreateProcessA（含空格路径可启动）。
# 编译独立 .o 并把 blob 内 shadow_sys_exec 改名为 shadow_sys_exec_blob，让本文件强符号覆盖。
"$CLANG_C" -O0 -I "$BOOT" -c bootstrap/sys_exec_cpa.c -o build/rt/sys_exec_cpa.o
"$LLVM/bin/llvm-objcopy.exe" --redefine-sym=shadow_sys_exec=shadow_sys_exec_blob "$BOOT/runtime_for_selfhost.o" build/rt/runtime_for_selfhost_wk.o

echo "[1] compile test/run_tests.shadow -> build/run_tests.ll"
build/shadow.exe test/run_tests.shadow -o build/run_tests.ll
echo "[2] llc -> build/run_tests.o"
"$LLCC" -O0 -filetype=obj build/run_tests.ll -o build/run_tests.o
echo "[3] clang++ link -> build/run_tests.exe"
"$CLANG" -std=c++17 build/run_tests.o \
    build/rt/runtime_for_selfhost_wk.o "$BOOT/miniz.o" \
    build/rt/shadow_gc_supplement.o build/rt/rt_zip.o build/rt/shadow_index.o build/rt/rt_proc_spawn.o build/rt/sys_exec_cpa.o \
    -o build/run_tests.exe -Wl,/subsystem:console -lws2_32 -lLLVM-C -L"$RT_LIB"
echo "    wrote build/run_tests.exe"
echo "done. 运行（从仓库根）： build/run_tests.exe"
