#!/usr/bin/env bash
# 等待 stage2 IR 生成完毕后，llc + clang 链接出 build/lspfix.exe
# 链接含 C 轻量索引器（rt/shadow_index.c → build/rt/shadow_index.o），供 LSP 索引快路径调用。
set -u
cd "$(dirname "$0")/.."
LLVM="D:/llvm/clang+llvm-22.1.0-x86_64-pc-windows-msvc"

echo "[0/3] precompile shadow_index.o (C light indexer)"
"$LLVM/bin/clang.exe" -O1 -Wall -c rt/shadow_index.c -o build/rt/shadow_index.o || exit 1

echo "[1/3] waiting for stage2 compile to finish ..."
for _ in $(seq 1 240); do
    if ! tasklist 2>/dev/null | grep -qi "shadow.exe"; then break; fi
    sleep 5
done

if [ ! -s build/lspfix_stage2.ll ]; then
    echo "[X] stage2 IR missing/empty"
    ls -la build/lspfix_stage2.ll 2>/dev/null
    tail -40 build/lspfix_stage2.log 2>/dev/null
    exit 1
fi
ls -la build/lspfix_stage2.ll
tail -5 build/lspfix_stage2.log 2>/dev/null

echo "[2/3] llc ..."
"$LLVM/bin/llc" -O0 -filetype=obj build/lspfix_stage2.ll -o build/lspfix.o || exit 1

echo "[3/3] clang++ link ..."
"$LLVM/bin/clang++" build/lspfix.o \
    bootstrap/runtime_for_selfhost.o bootstrap/miniz.o \
    build/rt/shadow_gc_supplement.o build/rt/rt_zip.o \
    build/rt/shadow_index.o \
    -o build/lspfix.exe -Wl,/subsystem:console -lws2_32 -lLLVM-C -L"$LLVM/lib" || exit 1

ls -la build/lspfix.exe
echo "BUILD_OK"
