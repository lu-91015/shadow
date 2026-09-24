#!/bin/bash
# 快速重建：应用 rt/linux/runtime_for_selfhost.cpp 与 src/codegen/codegen.shadow 改动。
# 1) 从源码重编 runtime .o 并派生 wk.o / user.o（含新增 shadow_hashmap_insert_float/ptr）
# 2) 用当前 build/shadow.exe（上一轮自举产物）编 src/main.shadow -> stage1 -> 链接为新 shadow.exe
LLVM_HOME="${LLVM_HOME:-D:/llvm/clang+llvm-22.1.0-x86_64-pc-windows-msvc}"
LLVM_M="$(cygpath -u "$LLVM_HOME" 2>/dev/null || echo "$LLVM_HOME")"
BIN="$LLVM_M/bin"
RT_LIB="$LLVM_HOME/lib"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
set -e
export PATH="$BIN:$PATH"

RT_O=build/rt/runtime_for_selfhost.o
echo "[1] runtime .o from cpp"
clang++.exe -O2 -c rt/linux/runtime_for_selfhost.cpp -o "$RT_O" \
  -I rt -I "$LLVM_HOME/include" -std=c++17 -D_CRT_SECURE_NO_WARNINGS -w

# 编译器本体 runtime（wk）：与 build_shadow.shadow [0.4/7] 一致的 objcopy 前缀
WK=""
for s in shadow_sys_exec shadow_any_box_ptr shadow_any_box_string shadow_any_print \
         shadow_any_to_string shadow_any_unbox_ptr shadow_array_get_int shadow_array_get_ptr \
         shadow_array_len shadow_array_pop shadow_array_push_float shadow_array_push_int \
         shadow_array_push_long shadow_array_push_ptr shadow_array_set_int shadow_array_set_ptr \
         shadow_content_hash shadow_gc_poll shadow_gc_register_type shadow_gc_root_range \
         shadow_int_to_str shadow_llvm_set_alwaysinline shadow_println_int shadow_println_str \
         shadow_set_cli_args shadow_string_concat shadow_string_len shadow_sys_args \
         shadow_zip_list shadow_zip_pack shadow_zip_unpack; do
  WK="$WK --redefine-sym=$s=__cpp_$s"
done
echo "[2] wk.o"
llvm-objcopy.exe --redefine-sym=shadow_sys_exec=shadow_sys_exec_blob $WK \
  "$RT_O" build/rt/runtime_for_selfhost_wk.o

# 用户程序 runtime（user）：GC 三符号保留原名，其余加 __cpp_ 前缀
US=""
for s in shadow_sys_exec shadow_any_box_ptr shadow_any_box_string shadow_any_print \
         shadow_any_to_string shadow_any_unbox_ptr shadow_array_get_int shadow_array_get_ptr \
         shadow_array_len shadow_array_pop shadow_array_push_float shadow_array_push_int \
         shadow_array_push_long shadow_array_push_ptr shadow_array_set_int shadow_array_set_ptr \
         shadow_content_hash shadow_int_to_str shadow_llvm_set_alwaysinline shadow_println_int \
         shadow_println_str shadow_set_cli_args shadow_string_concat shadow_string_len \
         shadow_sys_args shadow_zip_list shadow_zip_pack shadow_zip_unpack; do
  US="$US --redefine-sym=$s=__cpp_$s"
done
echo "[3] user.o"
llvm-objcopy.exe $US "$RT_O" build/rt/runtime_for_selfhost_user.o

echo "[4] compile src/main.shadow -> stage1"
build/shadow.exe src/main.shadow -o build/stage1.ll
llc.exe -O2 -filetype=obj build/stage1.ll -o build/stage1.o

echo "[5] link new shadow.exe"
MSYS2_ARG_CONV_EXCL='*' clang++.exe -std=c++17 \
  build/stage1.o build/rt/runtime_for_selfhost_wk.o build/rt/miniz.o \
  build/rt/shadow_gc_supplement.o build/rt/rt_zip.o build/rt/shadow_index.o \
  build/rt/rt_proc_spawn.o build/rt/sys_exec_cpa.o build/rt/cxa_atexit_shim.o \
  -o build/shadow.exe.new -Wl,/subsystem:console -Wl,/Brepro -Wl,/stack:8388608 \
  -lws2_32 -lLLVM-C -L"$RT_LIB" -Lpkg/shadow/llvm/crt
echo "OK -> build/shadow.exe.new"