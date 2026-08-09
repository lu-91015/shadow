#!/bin/bash
# 第二轮：放开 tc_ns_is_valid 死代码闸门后的四阶段验证（含阶段耗时）
cd "C:/Users/cxyu/AppData/Roaming/TRAE SOLO CN/ModularData/ai-agent/work-mode-projects/6a5353846df2d3b188c57699/shadow/shadow-0.5" || exit 1
LLVM="D:/llvm/clang+llvm-22.1.0-x86_64-pc-windows-msvc"
LLC="$LLVM/bin/llc.exe"
CXX="$LLVM/bin/clang++.exe"
RTOBJS="bootstrap/runtime_for_selfhost.o bootstrap/miniz.o build/rt/shadow_gc_supplement.o build/rt/rt_zip.o"

echo "=== [0] 清空 lu_cache ==="
[ -d build/lu_cache ] && mv build/lu_cache "build/_old_lu_$(date +%s)"
mkdir -p build/lu_cache
echo on > build/.timing

echo "=== [1] stage2: build/shadow.exe 编译当前源码（~5.5min）==="
./build/shadow.exe src/main.shadow -o build/vc_stage2.ll 2>&1 | grep -E "TIMING|error"
ls -la build/vc_stage2.ll || { echo "!!! STAGE2 MISSING"; exit 1; }

echo "=== [2] 链接 d4.exe ==="
"$LLC" -O0 -filetype=obj build/vc_stage2.ll -o build/vc_stage2.o || { echo "!!! LLC FAILED"; exit 1; }
"$CXX" build/vc_stage2.o $RTOBJS -o build/vc.exe -Wl,/subsystem:console -lws2_32 -lLLVM-C -L"$LLVM/lib" || { echo "!!! LINK FAILED"; exit 1; }
ls -la build/vc.exe

echo "=== [3] stage3: d4.exe 冷缓存 ==="
T2=$(date +%s)
./build/vc.exe src/main.shadow -o build/vc_stage3.ll 2>&1 | grep -E "TIMING|error"
T3=$(date +%s)
COLD=$((T3-T2))
echo "stage3(cold) wall=${COLD}s"
ls -la build/vc_stage3.ll || { echo "!!! STAGE3 FAILED"; exit 1; }
echo "写入 .lu 数量: $(ls build/lu_cache | wc -l)"

echo "=== [4] stage4: d4.exe 热缓存（TC bodies 应被跳过）==="
T4=$(date +%s)
./build/vc.exe src/main.shadow -o build/vc_stage4.ll 2>&1 | grep -E "TIMING|error"
T5=$(date +%s)
HOT=$((T5-T4))
echo "stage4(hot) wall=${HOT}s"
ls -la build/vc_stage4.ll || { echo "!!! STAGE4 FAILED"; exit 1; }

echo "=== [5] stage5: 再热一次（确认稳定，且 hot 产物可重复）==="
T6=$(date +%s)
./build/vc.exe src/main.shadow -o build/vc_stage5.ll 2>&1 | grep -E "TIMING|error"
T7=$(date +%s)
echo "stage5(hot2) wall=$((T7-T6))s"

rm -f build/.timing
echo "=========================================="
ls -la build/vc_stage2.ll build/vc_stage3.ll build/vc_stage4.ll build/vc_stage5.ll
cmp -s build/vc_stage2.ll build/vc_stage3.ll && echo "[OK]   A. stage2==stage3 自举 fixed-point" || { echo "[FAIL] A. stage2!=stage3"; cmp build/vc_stage2.ll build/vc_stage3.ll | head -3; }
cmp -s build/vc_stage3.ll build/vc_stage4.ll && echo "[OK]   B. stage3==stage4 缓存语义透明（冷==热）" || { echo "[FAIL] B. stage3!=stage4 缓存改变产物"; cmp build/vc_stage3.ll build/vc_stage4.ll | head -3; }
cmp -s build/vc_stage4.ll build/vc_stage5.ll && echo "[OK]   C. stage4==stage5 热运行可重复" || echo "[FAIL] C. 热运行不稳定"
echo "D. 耗时 cold=${COLD}s hot=${HOT}s"
echo "=========================================="
