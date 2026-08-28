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

# 本机环境（未入库，含绝对路径）：存在则 source，用于 LLVM_HOME 等默认值
SHADOW_ROOT_WIN="$ROOT"
# shellcheck source=./env.local.sh
[ -f tools/env.local.sh ] && . tools/env.local.sh
: "${LLVM_HOME:=${LLVM_HOME_WIN:-}}"
export LLVM_HOME

[ -n "${LLVM_HOME:-}" ] || { echo "[X] LLVM_HOME 未设置且 tools/env.local.sh 无默认值"; exit 1; }
[ -x build/shadow.exe ] || { echo "[X] 冻结种子 build/shadow.exe 缺失"; exit 1; }

# 全新 .lu 函数级缓存（陈旧缓存会让 src/ 改动看起来完全无效）
TS="$(date +%Y%m%d_%H%M%S)"; export SHADOW_TS="$TS"
mkdir -p build/logs
find build -maxdepth 1 -name '.lu*' -exec rm -rf {} + 2>/dev/null
export SHADOW_LU_CACHE_DIR="$ROOT/build/.lu_boot_$TS"
mkdir -p "$SHADOW_LU_CACHE_DIR"
echo "[env] 全新 .lu 缓存 -> $SHADOW_LU_CACHE_DIR"

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

# ── [2a] runtime .o 无条件从源码现编 ─────────────────────────────
# 下面 objcopy 派生 wk.o / user.o 的母本就是 bootstrap/runtime_for_selfhost.o。
# 若沿用仓库里那份已提交二进制，改 rt/ 后再自举会静默链到旧实现；而 fixed-point 只逐字节
# 比 stage2.ll/stage3.ll，看不见 .o 漂移 → 假装通过（c0819b2 事故即此）。
# 不条件判断 mtime：checkout / 编辑器保留时间戳 / 时钟偏差都能骗过 -nt，一律现编。
RT_CPP=rt/linux/runtime_for_selfhost.cpp
RT_O=bootstrap/runtime_for_selfhost.o
echo "[2a] runtime .o 从源码现编 -> $RT_O"
"$bin/clang++.exe" -O1 -c "$RT_CPP" -o "$RT_O" \
  -I bootstrap -I "$LLVM_HOME/include" -std=c++17 -D_CRT_SECURE_NO_WARNINGS -w

echo "[3] precompile runtime objects"
"$bin/clang.exe" -O0 -I bootstrap -c rt/rt_zip.c -o build/rt/rt_zip.o
"$bin/clang.exe" -O0 -c rt/shadow_gc_supplement.c -o build/rt/shadow_gc_supplement.o
"$bin/clang.exe" -O1 -Wall -c rt/shadow_index.c -o build/rt/shadow_index.o
"$bin/clang.exe" -O0 -I bootstrap -c rt/rt_proc_spawn.c -o build/rt/rt_proc_spawn.o
"$bin/clang.exe" -O0 -I bootstrap -c bootstrap/sys_exec_cpa.c -o build/rt/sys_exec_cpa.o
# 用户程序 runtime 辅助对象：miniz 源已入库（bootstrap/miniz.c），一律现编，不复制预编译 .o。
# 同时刷新 bootstrap/miniz.o —— 驱动 build_shadow.exe 与 stage 链接行都引用该路径。
"$bin/clang.exe" -O1 -I bootstrap -c bootstrap/miniz.c -o bootstrap/miniz.o
"$bin/clang.exe" -O1 -I bootstrap -c bootstrap/miniz.c -o build/rt/miniz.o
"$bin/clang.exe" -O0 -I bootstrap -c bootstrap/cxa_atexit_shim.c -o build/rt/cxa_atexit_shim.o
# 编译器本体 runtime（wk 版）：与 tools/build_shadow.shadow 的 [0.4/7] 完全一致——
# stage1.o 内含 runtime_lib 强符号，C++ runtime 同名实现全部加 __cpp_ 前缀避免重复定义。
"$bin/llvm-objcopy.exe" \
  --redefine-sym=shadow_sys_exec=shadow_sys_exec_blob \
  --redefine-sym=shadow_any_box_ptr=__cpp_shadow_any_box_ptr \
  --redefine-sym=shadow_any_box_string=__cpp_shadow_any_box_string \
  --redefine-sym=shadow_any_print=__cpp_shadow_any_print \
  --redefine-sym=shadow_any_to_string=__cpp_shadow_any_to_string \
  --redefine-sym=shadow_any_unbox_ptr=__cpp_shadow_any_unbox_ptr \
  --redefine-sym=shadow_array_get_int=__cpp_shadow_array_get_int \
  --redefine-sym=shadow_array_get_ptr=__cpp_shadow_array_get_ptr \
  --redefine-sym=shadow_array_len=__cpp_shadow_array_len \
  --redefine-sym=shadow_array_pop=__cpp_shadow_array_pop \
  --redefine-sym=shadow_array_push_float=__cpp_shadow_array_push_float \
  --redefine-sym=shadow_array_push_int=__cpp_shadow_array_push_int \
  --redefine-sym=shadow_array_push_long=__cpp_shadow_array_push_long \
  --redefine-sym=shadow_array_push_ptr=__cpp_shadow_array_push_ptr \
  --redefine-sym=shadow_array_set_int=__cpp_shadow_array_set_int \
  --redefine-sym=shadow_array_set_ptr=__cpp_shadow_array_set_ptr \
  --redefine-sym=shadow_content_hash=__cpp_shadow_content_hash \
  --redefine-sym=shadow_gc_poll=__cpp_shadow_gc_poll \
  --redefine-sym=shadow_gc_register_type=__cpp_shadow_gc_register_type \
  --redefine-sym=shadow_gc_root_range=__cpp_shadow_gc_root_range \
  --redefine-sym=shadow_int_to_str=__cpp_shadow_int_to_str \
  --redefine-sym=shadow_llvm_set_alwaysinline=__cpp_shadow_llvm_set_alwaysinline \
  --redefine-sym=shadow_println_int=__cpp_shadow_println_int \
  --redefine-sym=shadow_println_str=__cpp_shadow_println_str \
  --redefine-sym=shadow_set_cli_args=__cpp_shadow_set_cli_args \
  --redefine-sym=shadow_string_concat=__cpp_shadow_string_concat \
  --redefine-sym=shadow_string_len=__cpp_shadow_string_len \
  --redefine-sym=shadow_sys_args=__cpp_shadow_sys_args \
  --redefine-sym=shadow_zip_list=__cpp_shadow_zip_list \
  --redefine-sym=shadow_zip_pack=__cpp_shadow_zip_pack \
  --redefine-sym=shadow_zip_unpack=__cpp_shadow_zip_unpack \
  bootstrap/runtime_for_selfhost.o build/rt/runtime_for_selfhost_wk.o
# 用户程序专用 runtime（main_link_exe Windows 分支链接用，与 Linux 同源 cpp 实现）：
# 用户产物内嵌 runtime_lib 的 define 与 C++ runtime 同名冲突 → 加 __cpp_ 前缀；
# GC 符号（gc_poll/gc_root_range/gc_register_type）保留原名（真 GC，对齐 Linux 用户产物）。
# 不链接 shadow_gc_supplement.o（no-op GC）——用户产物必须用真 GC。
"$bin/llvm-objcopy.exe" \
  --redefine-sym=shadow_sys_exec=__cpp_shadow_sys_exec \
  --redefine-sym=shadow_any_box_ptr=__cpp_shadow_any_box_ptr \
  --redefine-sym=shadow_any_box_string=__cpp_shadow_any_box_string \
  --redefine-sym=shadow_any_print=__cpp_shadow_any_print \
  --redefine-sym=shadow_any_to_string=__cpp_shadow_any_to_string \
  --redefine-sym=shadow_any_unbox_ptr=__cpp_shadow_any_unbox_ptr \
  --redefine-sym=shadow_array_get_int=__cpp_shadow_array_get_int \
  --redefine-sym=shadow_array_get_ptr=__cpp_shadow_array_get_ptr \
  --redefine-sym=shadow_array_len=__cpp_shadow_array_len \
  --redefine-sym=shadow_array_pop=__cpp_shadow_array_pop \
  --redefine-sym=shadow_array_push_float=__cpp_shadow_array_push_float \
  --redefine-sym=shadow_array_push_int=__cpp_shadow_array_push_int \
  --redefine-sym=shadow_array_push_long=__cpp_shadow_array_push_long \
  --redefine-sym=shadow_array_push_ptr=__cpp_shadow_array_push_ptr \
  --redefine-sym=shadow_array_set_int=__cpp_shadow_array_set_int \
  --redefine-sym=shadow_array_set_ptr=__cpp_shadow_array_set_ptr \
  --redefine-sym=shadow_content_hash=__cpp_shadow_content_hash \
  --redefine-sym=shadow_int_to_str=__cpp_shadow_int_to_str \
  --redefine-sym=shadow_llvm_set_alwaysinline=__cpp_shadow_llvm_set_alwaysinline \
  --redefine-sym=shadow_println_int=__cpp_shadow_println_int \
  --redefine-sym=shadow_println_str=__cpp_shadow_println_str \
  --redefine-sym=shadow_set_cli_args=__cpp_shadow_set_cli_args \
  --redefine-sym=shadow_string_concat=__cpp_shadow_string_concat \
  --redefine-sym=shadow_string_len=__cpp_shadow_string_len \
  --redefine-sym=shadow_sys_args=__cpp_shadow_sys_args \
  --redefine-sym=shadow_zip_list=__cpp_shadow_zip_list \
  --redefine-sym=shadow_zip_pack=__cpp_shadow_zip_pack \
  --redefine-sym=shadow_zip_unpack=__cpp_shadow_zip_unpack \
  bootstrap/runtime_for_selfhost.o build/rt/runtime_for_selfhost_user.o
echo "[user runtime] build/rt/runtime_for_selfhost_user.o written"

echo "[4] link driver build/build_shadow.exe"
"$bin/clang++.exe" -std=c++17 \
  build/build_shadow.o build/rt/runtime_for_selfhost_wk.o bootstrap/miniz.o \
  build/rt/shadow_gc_supplement.o build/rt/rt_zip.o build/rt/shadow_index.o \
  build/rt/rt_proc_spawn.o build/rt/sys_exec_cpa.o build/rt/cxa_atexit_shim.o \
  -o build/build_shadow.exe -Wl,/subsystem:console -lws2_32 -lLLVM-C -L"$LLVM_M/lib" -Lpkg/shadow/llvm/crt

echo "[5] run bootstrap (7-stage self-host)"
build/build_shadow.exe

echo "BOOTSTRAP_OK"
