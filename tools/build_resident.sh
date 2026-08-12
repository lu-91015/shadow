#!/usr/bin/env bash
# 自举接力脚本：假设 stage1 已由现有 build/shadow.exe 编译出 build/r_stage2.ll。
# 负责：链接 -> r.exe -> stage3/4 自举固定点比对 -> 部署为 build/shadow.exe。
# 用法（在 shadow-0.5 仓库根目录执行）：bash tools/build_resident.sh
set -u
cd "$(dirname "$0")/.."
LLVM="D:/llvm/clang+llvm-22.1.0-x86_64-pc-windows-msvc"
export PATH="$LLVM/bin:$PATH"
LLVM_HOME="$(cygpath -w "$LLVM")"

echo "== [link] precompile shadow_index.o =="
"$LLVM/bin/clang.exe" -O1 -Wall -c rt/shadow_index.c -o build/rt/shadow_index.o || exit 1

if [ ! -s build/r_stage2.ll ]; then
    echo "[X] stage2 IR missing"
    ls -la build/r_stage2.ll 2>/dev/null
    tail -40 build/r_stage2.log 2>/dev/null
    exit 1
fi
echo "== [link] llc r_stage2.ll =="
"$LLVM/bin/llc" -O0 -filetype=obj build/r_stage2.ll -o build/r.o || exit 1
echo "== [link] clang++ -> build/r.exe =="
"$LLVM/bin/clang++" build/r.o \
    bootstrap/runtime_for_selfhost.o bootstrap/miniz.o \
    build/rt/shadow_gc_supplement.o build/rt/rt_zip.o build/rt/shadow_index.o \
    build/rt/rt_proc_spawn.o \
    -o build/r.exe -Wl,/subsystem:console -lws2_32 -lLLVM-C -L"$LLVM/lib" || exit 1
ls -la build/r.exe
REXE="$(cygpath -w "$PWD/build/r.exe")"

echo "== [stage3] r.exe compiles src -> r_stage3.ll =="
"$REXE" src/main.shadow -o build/r_stage3.ll > build/r_stage3.log 2>&1; echo "STAGE3_RC=$?"
if [ ! -s build/r_stage3.ll ]; then echo "[X] stage3 IR empty"; tail -40 build/r_stage3.log; exit 1; fi

echo "== [cmp] stage2 vs stage3 =="
if cmp -s build/r_stage2.ll build/r_stage3.ll; then echo "FIXEDPOINT_OK stage2==stage3"; else echo "[X] stage2 != stage3"; exit 1; fi

echo "== [stage4] r.exe compiles src -> r_stage4.ll (reuse .lu cache) =="
"$REXE" src/main.shadow -o build/r_stage4.ll > build/r_stage4.log 2>&1; echo "STAGE4_RC=$?"
if [ ! -s build/r_stage4.ll ]; then echo "[X] stage4 IR empty"; tail -40 build/r_stage4.log; exit 1; fi

echo "== [cmp] stage3 vs stage4 =="
if cmp -s build/r_stage3.ll build/r_stage4.ll; then echo "FIXEDPOINT_OK stage3==stage4"; else echo "[X] stage3 != stage4"; exit 1; fi

echo "== [deploy] r.exe -> build/shadow.exe =="
cp build/r.exe build/shadow.exe
ls -la build/shadow.exe
echo "BUILD_DEPLOYED"
