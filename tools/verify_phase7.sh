#!/usr/bin/env bash
# tools/verify_phase7.sh — Phase 7-8 验证链
# 1) 检查 stage1.ll 含 0.4 runtime 字符串函数
# 2) llc + 链接 → shadow-stage1.exe
# 3) shadow-stage1.exe 编译字符串用例 → .ll
# 4) llc + rt 层链接 → 运行 → 校验输出
set -u
ROOT="C:/Users/cxyu/AppData/Roaming/TRAE SOLO CN/ModularData/ai-agent/work-mode-projects/6a5353846df2d3b188c57699/shadow/shadow-0.4"
LLVM_HOME="D:/llvm/clang+llvm-22.1.0-x86_64-pc-windows-msvc"
LLCC="$LLVM_HOME/bin/llc.exe"
CLANG="$LLVM_HOME/bin/clang.exe"
BOOT="$ROOT/bootstrap"
BUILD="$ROOT/build"

echo "=== [1] stage1.ll 检查（0.4 runtime 字符串函数） ==="
grep -c "define.*shadow_sjoin\|define.*shadow_ssplit\|define.*shadow_format" "$BUILD/stage1.ll" || echo "!! stage1.ll 缺少 0.4 字符串函数"
grep -c "define.*shadow_string_join" "$BUILD/stage1.ll" || true

echo "=== [2] llc + 链接 shadow-stage1.exe ==="
"$LLCC" -O0 -filetype=obj "$BUILD/stage1.ll" -o "$BUILD/stage1.o" || { echo "llc failed"; exit 1; }
"$CLANG" -std=c++17 "$BUILD/stage1.o" "$BOOT/runtime_for_selfhost.o" "$BOOT/miniz.o" \
    -o "$BUILD/shadow-stage1.exe" -Wl,/subsystem:console -lws2_32 -lLLVM-C -L"$LLVM_HOME/lib" \
    || { echo "link failed"; exit 1; }
echo "OK: shadow-stage1.exe"

echo "=== [3] 编译字符串用例（str_join 等） ==="
cd "$ROOT"
./build/shadow-stage1.exe test/cases/exe/ex_str_join.shadow -o build/_t_join.ll 2>&1 | tail -5
grep -c "shadow_sjoin" build/_t_join.ll || echo "!! 产物 .ll 无 shadow_sjoin 调用"
grep -c "define.*shadow_sjoin" build/_t_join.ll || echo "!! 产物 .ll 无 shadow_sjoin 定义（runtime 注入失败）"

echo "=== [4] llc + rt 层链接 + 运行 ==="
"$LLCC" -O0 -filetype=obj build/_t_join.ll -o build/_t_join.o || { echo "llc2 failed"; exit 1; }
python "$ROOT/tools/link_rt.py" build/_t_join.ll build/_t_join.exe || { echo "link2 failed"; exit 1; }
OUT=$(./build/_t_join.exe 2>&1)
echo "--- output ---"
echo "$OUT"
echo "--- check ---"
if echo "$OUT" | grep -q "a-b-c"; then echo "PASS: str_join 输出正确"; else echo "FAIL: str_join 输出错误"; fi
