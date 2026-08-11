#!/usr/bin/env bash
set -e
cd "$(dirname "$0")"
BOOT=bootstrap
# LLVM 工具链路径从环境变量 LLVM_HOME 读取（无内置默认路径）。
: "${LLVM_HOME:?set LLVM_HOME to your LLVM installation (e.g. D:/llvm/clang+llvm-XX)}"
LLVM="$LLVM_HOME"
LLCC="$LLVM/bin/llc.exe"
CLANG="$LLVM/bin/clang++.exe"
CLANG_C="$LLVM/bin/clang.exe"
RT_LIB="$LLVM/lib"
# stage1 冷启动宿主：仓库内已有的 build/shadow.exe（上一轮自举产物）。
HOST=build/shadow.exe

mkdir -p build/rt
# 预编译 rt_zip.o（链接 shadow.exe 时依赖，miniz.h 在 bootstrap/ 下）
# 注意：rt_io.o 不参与 shadow.exe 链接——bootstrap/runtime_for_selfhost.o 已内置
# shadow_stdin_read_line 等 6 个符号，重复链接会 duplicate symbol。
# rt_io.o 只在 [6/7] 编译，供 tools/link_rt.py 链接用户程序时使用。
echo "[0/7] precompile rt_zip.o + shadow_gc_supplement.o"
"$CLANG_C" -O0 -I "$BOOT" -c rt/rt_zip.c -o build/rt/rt_zip.o
# shadow_gc_supplement.o 供编译器本体链接（no-op GC 符号），CI 干净环境必须显式编译
"$CLANG_C" -O0 -c rt/shadow_gc_supplement.c -o build/rt/shadow_gc_supplement.o
# C 轻量索引器（rt/shadow_index.c）：LSP 索引快路径（completion/semanticTokens 等），免全量编译
"$CLANG_C" -O1 -Wall -c rt/shadow_index.c -o build/rt/shadow_index.o

echo "[1/7] stage1: HOST($HOST) compile src/main.shadow"
"$HOST" src/main.shadow -o build/stage1.ll
"$LLCC" -O0 -filetype=obj build/stage1.ll -o build/stage1.o
"$CLANG" -std=c++17 build/stage1.o "$BOOT/runtime_for_selfhost.o" "$BOOT/miniz.o" build/rt/shadow_gc_supplement.o build/rt/rt_zip.o build/rt/shadow_index.o -o build/shadow-stage1.exe -Wl,/subsystem:console -lws2_32 -lLLVM-C -L"$RT_LIB"
echo "    wrote build/shadow-stage1.exe"

echo "[2/7] stage2: shadow-stage1 compile src/main.shadow"
build/shadow-stage1.exe src/main.shadow -o build/stage2.ll
"$LLCC" -O0 -filetype=obj build/stage2.ll -o build/stage2.o
"$CLANG" -std=c++17 build/stage2.o "$BOOT/runtime_for_selfhost.o" "$BOOT/miniz.o" build/rt/shadow_gc_supplement.o build/rt/rt_zip.o build/rt/shadow_index.o -o build/shadow-stage2.exe -Wl,/subsystem:console -lws2_32 -lLLVM-C -L"$RT_LIB"
echo "    wrote build/shadow-stage2.exe"

echo "[3/7] stage3: shadow-stage2 compile src/main.shadow"
build/shadow-stage2.exe src/main.shadow -o build/stage3.ll
echo "    wrote build/stage3.ll"

echo "[4/7] verify fixed-point"
python3 tools/verify_fixed_point.py build/stage2.ll build/stage3.ll

echo "[5/7] final artifact shadow.exe"
cp -f build/shadow-stage2.exe build/shadow.exe
echo "    wrote build/shadow.exe"

# 注：无独立的运行时编译单元。src/runtime/runtime_lib.shadow 作为普通模块
# 直接参与 combined 编译。

echo "[6/7] compile rt/ system-call layer"
# rt_zip 已在 [0/7] 编译，此处不重复。
for f in rt_core rt_fs rt_proc rt_time rt_err rt_extra rt_gc rt_thread rt_io; do
  "$CLANG_C" -O0 -I "$BOOT" -c "rt/$f.c" -o "build/rt/$f.o" || echo "WARN: rt/$f.c compile failed"
done

echo "[7/7] smoke --version"
build/shadow.exe --version || true

echo "done"
