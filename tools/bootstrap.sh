#!/bin/bash
# 自举引导脚本（CI / 本地通用）
#
# 用冻结种子 build/shadow.exe 把 tools/build_shadow.shadow（权威构建程序，Shadow 源码）
# 先编成驱动 build/build_shadow.exe，再运行该驱动执行 7 阶段自举，
# 最终部署出 build/shadow.exe（新鲜自托管编译器）+ 编译 rt/*.o。
#
# 前置环境：
#   LLVM_HOME  —— clang+llvm-22.1.0-x86_64-pc-windows-msvc 根目录（Windows 原生路径，
#                供 native 工具链 shadow 驱动 / clang / lld-link 使用）
#   LIB        —— MSVC + Windows SDK(um/ucrt) x64 库路径（提供 LLVM-C 间接需要的 winhttp.lib）
#   PATH       —— 含 $LLVM_HOME/bin（提供 LLVM-C.dll / llc / clang / llvm-objcopy）
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

[ -z "${LLVM_HOME:-}" ] && { echo "[X] LLVM_HOME not set"; exit 1; }
[ -x build/shadow.exe ] || { echo "[X] 冻结种子 build/shadow.exe 缺失"; exit 1; }

# native -> mingw 形式，供 bash 侧工具（llc/clang/llvm-objcopy）调用
if command -v cygpath >/dev/null 2>&1; then
  LLVM_M="$(cygpath -u "$LLVM_HOME")"
else
  LLVM_M="$LLVM_HOME"
fi
bin="$LLVM_M/bin"
export PATH="$bin:$PATH"

mkdir -p build/rt

echo "[1] emit IR: tools/build_shadow.shadow -> build/build_shadow.ll"
build/shadow.exe tools/build_shadow.shadow -o build/build_shadow.ll

echo "[2] llc -> build/build_shadow.o"
"$bin/llc.exe" -O0 -filetype=obj build/build_shadow.ll -o build/build_shadow.o

echo "[3] precompile runtime objects"
"$bin/clang.exe" -O0 -I bootstrap -c rt/rt_zip.c -o build/rt/rt_zip.o
"$bin/clang.exe" -O0 -c rt/shadow_gc_supplement.c -o build/rt/shadow_gc_supplement.o
"$bin/clang.exe" -O1 -Wall -c rt/shadow_index.c -o build/rt/shadow_index.o
"$bin/clang.exe" -O0 -I bootstrap -c rt/rt_proc_spawn.c -o build/rt/rt_proc_spawn.o
"$bin/clang.exe" -O0 -I bootstrap -c bootstrap/sys_exec_cpa.c -o build/rt/sys_exec_cpa.o
"$bin/llvm-objcopy.exe" --redefine-sym=shadow_sys_exec=shadow_sys_exec_blob \
  bootstrap/runtime_for_selfhost.o build/rt/runtime_for_selfhost_wk.o

echo "[4] link driver build/build_shadow.exe"
"$bin/clang++.exe" -std=c++17 \
  build/build_shadow.o build/rt/runtime_for_selfhost_wk.o bootstrap/miniz.o \
  build/rt/shadow_gc_supplement.o build/rt/rt_zip.o build/rt/shadow_index.o \
  build/rt/rt_proc_spawn.o build/rt/sys_exec_cpa.o \
  -o build/build_shadow.exe -Wl,/subsystem:console -lws2_32 -lLLVM-C -L"$LLVM_M/lib"

echo "[5] run bootstrap (7-stage self-host)"
build/build_shadow.exe

echo "BOOTSTRAP_OK"
