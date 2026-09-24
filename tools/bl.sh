#!/bin/bash
# 编译+链接单个 .shadow 基准（镜像 tools/build_shadow.shadow 的链接配方）
# 用法： bl.sh bench/shadow/foo   （foo.shadow 必须存在；产物 foo.shadow.exe 与 foo.ll/foo.o）
set -e
SRC="$1.shadow"
BASE="$1"
LLVM_HOME="${LLVM_HOME:-D:/llvm/clang+llvm-22.1.0-x86_64-pc-windows-msvc}"
BIN="$LLVM_HOME/bin"
RT_LIB="$LLVM_HOME/lib"
RT=build/rt
# BOOT=bootstrap  # 已废弃：runtime 源在 rt/，生成 .o 在 build/rt/

echo "[1/3] shadow -> ll"
build/shadow.exe "$SRC" -o "$BASE.ll"
echo "[2/3] llc -> o"
"$BIN/llc.exe" -O2 -filetype=obj "$BASE.ll" -o "$BASE.o"
echo "[3/3] clang link -> exe"
# 用户程序 runtime：与 main_link_exe Windows 分支同配方（统一 cpp runtime）。
# runtime_for_selfhost_user.o（真 GC 保留 + runtime_lib 冲突符号加前缀）
# + 辅助 .o；不链接 shadow_gc_supplement.o（no-op GC 会与真 GC 重复定义）。
"$BIN/clang++.exe" -std=c++17 \
  "$BASE.o" \
  "$RT/runtime_for_selfhost_user.o" \
  "$RT/miniz.o" \
  "$RT/rt_zip.o" \
  "$RT/shadow_index.o" \
  "$RT/rt_proc_spawn.o" \
  "$RT/sys_exec_cpa.o" \
  "$RT/cxa_atexit_shim.o" \
  -o "$BASE.shadow.exe" \
  -Wl,/subsystem:console -Wl,/stack:8388608 \
  -lws2_32 -lLLVM-C -L"$RT_LIB" -Lpkg/shadow/llvm/crt
echo "OK -> $BASE.shadow.exe"
