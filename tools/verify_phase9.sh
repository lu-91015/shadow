#!/usr/bin/env bash
# verify_phase9.sh — GC v1 验证链（Phase 9）
# 前提：stage1.ll 已由 s03 重编（含 runtime_lib array 布局 + codegen GC 改造）
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
LLVM_HOME="${LLVM_HOME:-D:\\llvm\\clang+llvm-22.1.0-x86_64-pc-windows-msvc}"
LLC="$LLVM_HOME/bin/llc.exe"
CLANG="$LLVM_HOME/bin/clang.exe"

echo "=== [1] 链接新 stage1 ==="
"$LLC" -O0 -filetype=obj build/stage1.ll -o build/stage1.o || { echo "llc stage1 failed"; exit 1; }
"$CLANG" -O0 build/stage1.o bootstrap/runtime_for_selfhost.o bootstrap/miniz.o \
    -o build/shadow-stage1.exe -Wl,/subsystem:console -lws2_32 -lLLVM-C -L"$LLVM_HOME/lib" \
    || { echo "link stage1 failed"; exit 1; }
echo "stage1 linked"

echo "=== [2] 验证 stage1 编译字符串用例（GC 开启） ==="
PASS=0; FAIL=0
for f in ex_str_join ex_str_split ex_str_contains_index ex_str_replace_trim ex_str_case ex_str_format ex_str_substr_charat; do
    ./build/shadow-stage1.exe test/cases/exe/$f.shadow -o build/_t.ll >/dev/null 2>&1 || { echo "COMPILE FAIL $f"; FAIL=$((FAIL+1)); continue; }
    python tools/link_rt.py build/_t.ll build/_t.exe >/dev/null 2>&1 || { echo "LINK FAIL $f"; FAIL=$((FAIL+1)); continue; }
    out=$(timeout 30 ./build/_t.exe 2>&1); rc=$?
    if [ $rc -eq 0 ]; then PASS=$((PASS+1)); else echo "RUN FAIL $f rc=$rc out=$out"; FAIL=$((FAIL+1)); fi
done
echo "str cases: PASS=$PASS FAIL=$FAIL"

echo "=== [3] GC 正确性用例（SHADOW_GOGC=100 触发） ==="
for f in ex_gc_cycle ex_gc_stress ex_gc_strings; do
    ./build/shadow-stage1.exe test/cases/exe/$f.shadow -o build/_t.ll >/dev/null 2>&1 || { echo "COMPILE FAIL $f"; continue; }
    python tools/link_rt.py build/_t.ll build/_t.exe >/dev/null 2>&1 || { echo "LINK FAIL $f"; continue; }
    out=$(SHADOW_GC_LOG=1 SHADOW_GOGC=100 timeout 60 ./build/_t.exe 2>&1); rc=$?
    echo "--- $f rc=$rc ---"
    echo "$out" | tail -4
done

echo "=== [4] 压测（GC 开启） ==="
./build/shadow-stage1.exe test/cases/exe/ex_str_stress.shadow -o build/_t.ll >/dev/null 2>&1
python tools/link_rt.py build/_t.ll build/_t.exe >/dev/null 2>&1
SHADOW_GOGC=100 timeout 60 ./build/_t.exe; echo "stress EXIT=$?"

echo "=== [5] 自举 stage2 ==="
./build/shadow-stage1.exe src/main.shadow -o build/stage2.ll 2>&1 | tail -2
echo "STAGE2_EXIT=$?"
