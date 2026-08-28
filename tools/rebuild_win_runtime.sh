#!/bin/bash
# 重建 Windows runtime（runtime_for_selfhost.o → wk.o + user.o）并重链 build/shadow.exe。
# 用法：bash tools/rebuild_win_runtime.sh [stage.o]
#   stage.o 默认 build/stage2.o（自举固定点产物）；可传 build/stage1.o 等。
# 前置：LLVM_HOME 环境变量指向 clang+llvm-22.1.0-x86_64-pc-windows-msvc。
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
LLVM_HOME="${LLVM_HOME:-D:/llvm/clang+llvm-22.1.0-x86_64-pc-windows-msvc}"
BIN="$LLVM_HOME/bin"
STAGE="${1:-build/stage2.o}"
[ -f "$STAGE" ] || { echo "[X] stage not found: $STAGE"; exit 1; }

echo "[1] compile runtime_for_selfhost.cpp (Windows)"
"$BIN/clang++.exe" -O1 -c rt/linux/runtime_for_selfhost.cpp -o bootstrap/runtime_for_selfhost.o \
  -I bootstrap -I build/linux -I "$LLVM_HOME/include" -std=c++17 -D_CRT_SECURE_NO_WARNINGS -w

# wk.o：编译器本体 runtime（全部冲突符号加 __cpp_ 前缀，shadow_sys_exec → blob）
WKREDEFS=""
for s in shadow_any_box_ptr shadow_any_box_string shadow_any_print shadow_any_to_string \
         shadow_any_unbox_ptr shadow_array_get_int shadow_array_get_ptr shadow_array_len \
         shadow_array_pop shadow_array_push_float shadow_array_push_int shadow_array_push_long \
         shadow_array_push_ptr shadow_array_set_int shadow_array_set_ptr shadow_content_hash \
         shadow_gc_poll shadow_gc_register_type shadow_gc_root_range shadow_int_to_str \
         shadow_llvm_set_alwaysinline shadow_println_int shadow_println_str shadow_set_cli_args \
         shadow_string_concat shadow_string_len shadow_sys_args shadow_zip_list shadow_zip_pack \
         shadow_zip_unpack; do
  WKREDEFS="$WKREDEFS --redefine-sym=$s=__cpp_$s"
done
echo "[2] generate wk.o (compiler runtime)"
"$BIN/llvm-objcopy.exe" --redefine-sym=shadow_sys_exec=shadow_sys_exec_blob $WKREDEFS \
  bootstrap/runtime_for_selfhost.o build/rt/runtime_for_selfhost_wk.o

# user.o：用户程序 runtime（除 GC 三符号 gc_poll/root_range/register_type 外加前缀）
USERREDEFS=""
for s in shadow_sys_exec shadow_any_box_ptr shadow_any_box_string shadow_any_print \
         shadow_any_to_string shadow_any_unbox_ptr shadow_array_get_int shadow_array_get_ptr \
         shadow_array_len shadow_array_pop shadow_array_push_float shadow_array_push_int \
         shadow_array_push_long shadow_array_push_ptr shadow_array_set_int shadow_array_set_ptr \
         shadow_content_hash shadow_int_to_str shadow_llvm_set_alwaysinline shadow_println_int \
         shadow_println_str shadow_set_cli_args shadow_string_concat shadow_string_len \
         shadow_sys_args shadow_zip_list shadow_zip_pack shadow_zip_unpack; do
  USERREDEFS="$USERREDEFS --redefine-sym=$s=__cpp_$s"
done
echo "[3] generate user.o (user-program runtime, GC kept)"
"$BIN/llvm-objcopy.exe" $USERREDEFS \
  bootstrap/runtime_for_selfhost.o build/rt/runtime_for_selfhost_user.o

echo "[4] relink build/shadow.exe"
# /Brepro：内容派生的固定时间戳，重链产物才可逐字节比对；仅此次调用关闭 MSYS 路径转换
# （否则 Git Bash 会把 /Brepro 改写成 D:/Git/Brepro）。
MSYS2_ARG_CONV_EXCL='*' "$BIN/clang++.exe" -std=c++17 \
  "$STAGE" build/rt/runtime_for_selfhost_wk.o bootstrap/miniz.o \
  build/rt/shadow_gc_supplement.o build/rt/rt_zip.o build/rt/shadow_index.o \
  build/rt/rt_proc_spawn.o build/rt/sys_exec_cpa.o build/rt/cxa_atexit_shim.o \
  -o build/shadow.exe -Wl,/subsystem:console -Wl,/Brepro -Wl,/stack:8388608 \
  -lws2_32 -lLLVM-C -L"$LLVM_HOME/lib" -Lpkg/shadow/llvm/crt
echo "OK -> build/shadow.exe"
