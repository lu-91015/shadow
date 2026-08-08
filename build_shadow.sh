#!/usr/bin/env bash
set -e
cd "$(dirname "$0")"
BOOT=bootstrap
LLVM="D:/llvm/clang+llvm-22.1.0-x86_64-pc-windows-msvc"
LLCC="$LLVM/bin/llc.exe"
CLANG="$LLVM/bin/clang++.exe"
CLANG_C="$LLVM/bin/clang.exe"
RT_LIB="$LLVM/lib"

# 预编译 rt_zip.o（链接 shadow.exe 时依赖，miniz.h 在 bootstrap/ 下）
# 注意：rt_io.o 不参与 shadow.exe 链接——bootstrap/runtime_for_selfhost.o 已内置
# shadow_stdin_read_line 等 6 个符号，重复链接会 duplicate symbol。
# rt_io.o 只在 [6.5/8] 编译，供 tools/link_rt.py 链接用户程序时使用。
mkdir -p build/rt
"$CLANG_C" -O0 -I "$BOOT" -c rt/rt_zip.c -o build/rt/rt_zip.o

echo "[1/7] stage1: s03 compile src/main.shadow"
"$BOOT/s03.exe" src/main.shadow -o build/stage1.ll
"$LLCC" -O0 -filetype=obj build/stage1.ll -o build/stage1.o
"$CLANG" -std=c++17 build/stage1.o "$BOOT/runtime_for_selfhost.o" "$BOOT/miniz.o" build/rt/shadow_gc_supplement.o build/rt/rt_zip.o -o build/shadow-stage1.exe -Wl,/subsystem:console -lws2_32 -lLLVM-C -L"$RT_LIB"
echo "    wrote build/shadow-stage1.exe"

echo "[2/7] stage2: shadow-stage1 compile src/main.shadow"
build/shadow-stage1.exe src/main.shadow -o build/stage2.ll
"$LLCC" -O0 -filetype=obj build/stage2.ll -o build/stage2.o
"$CLANG" -std=c++17 build/stage2.o "$BOOT/runtime_for_selfhost.o" "$BOOT/miniz.o" build/rt/shadow_gc_supplement.o build/rt/rt_zip.o -o build/shadow-stage2.exe -Wl,/subsystem:console -lws2_32 -lLLVM-C -L"$RT_LIB"
echo "    wrote build/shadow-stage2.exe"

echo "[3/7] stage3: shadow-stage2 compile src/main.shadow"
build/shadow-stage2.exe src/main.shadow -o build/stage3.ll
echo "    wrote build/stage3.ll"

echo "[4/7] verify fixed-point"
python3 tools/verify_fixed_point.py build/stage2.ll build/stage3.ll

echo "[5/7] final artifact shadow.exe"
cp -f build/shadow-stage2.exe build/shadow.exe
echo "    wrote build/shadow.exe"

# 注：0.3 时代的 [6/8] "runtime cache"（--emit-runtime-cache → runtime.ll/.sig/.lu）已移除。
# 0.4 架构下 src/runtime/runtime_lib.shadow 作为普通模块直接参与 combined 编译，
# 无独立运行时编译单元；生成的 runtime.o 从未被任何链接行消费，属死步骤。

echo "[6/7] compile rt/ system-call layer"
mkdir -p build/rt
for f in rt_core rt_fs rt_proc rt_time rt_err rt_extra rt_gc rt_thread rt_zip rt_io; do
  "$CLANG_C" -O0 -I "$BOOT" -c "rt/$f.c" -o "build/rt/$f.o" || echo "WARN: rt/$f.c compile failed"
done

echo "[7/7] smoke --version"
build/shadow.exe --version || true

echo "done"
