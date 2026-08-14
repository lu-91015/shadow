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
# stage1 冷启动宿主：默认用上一轮自举产物 build/shadow.exe（增量重建）。
# 本轮修复（模块级 let x="" 退化 any / spawn void 非法 IR）已落在 src/ 工作树。
# 关键：0.5 编译器自身的模块级全局【全部带显式类型注解】（src 下 ^let 无一处裸字面量推断），
# 故当前 build/shadow.exe 即便自身带旧 bug，也能把【已修复的】src 正确编译成无 bug 的下一轮二进制——
# 自举死循环已被打破，直接用默认 HOST 即可（无需 0.4，0.4 还会因循环 import 拒编 main.shadow）。
# 仅当需要从外部引导器冷启动时才覆盖：HOST="<其他 shadow.exe>" bash build_shadow.sh
: "${HOST:=build/shadow.exe}"

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
# rt_proc_spawn.o 提供 shadow_proc_launch/poll/reap（LSP 异步编译 worker 的底层非阻塞 spawn），
# 必须链接进编译器本体（stage1/stage2 宿主）。它与 rt_proc.c 分离：rt_proc.c 的 shadow_sys_exec/
# shadow_exit/shadow_env_get 已在 bootstrap/runtime_for_selfhost.o 定义，整编 rt_proc.o 会 duplicate。
"$CLANG_C" -O0 -I "$BOOT" -c rt/rt_proc_spawn.c -o build/rt/rt_proc_spawn.o

# [FIX] 自举运行时 shadow_sys_exec 改用 CreateProcessA，正确处理含空格路径
# （原 runtime_for_selfhost.o 用 _popen/cmd /c，仓库路径含空格如 "TRAE SOLO CN"
#  时所有子进程启动失败：'C:/.../TRAE' is not recognized）。
# 把 rt/rt_proc.c 的 CreateProcessA 版 shadow_sys_exec 编成独立 .o，再用
# llvm-objcopy 把 blob 内的 shadow_sys_exec 改名为 shadow_sys_exec_blob
# （定义+内部引用一起改，内部调用者行为不变、无回归），让本文件强符号覆盖。
echo "[0.4/7] patch runtime: CreateProcessA shadow_sys_exec"
"$CLANG_C" -O0 -I "$BOOT" -c bootstrap/sys_exec_cpa.c -o build/rt/sys_exec_cpa.o
"$LLVM/bin/llvm-objcopy.exe" --redefine-sym=shadow_sys_exec=shadow_sys_exec_blob "$BOOT/runtime_for_selfhost.o" build/rt/runtime_for_selfhost_wk.o

echo "[1/7] stage1: HOST($HOST) compile src/main.shadow"
"$HOST" src/main.shadow -o build/stage1.ll
"$LLCC" -O0 -filetype=obj build/stage1.ll -o build/stage1.o
"$CLANG" -std=c++17 build/stage1.o build/rt/runtime_for_selfhost_wk.o "$BOOT/miniz.o" build/rt/shadow_gc_supplement.o build/rt/rt_zip.o build/rt/shadow_index.o build/rt/rt_proc_spawn.o build/rt/sys_exec_cpa.o -o build/shadow-stage1.exe -Wl,/subsystem:console -lws2_32 -lLLVM-C -L"$RT_LIB"
echo "    wrote build/shadow-stage1.exe"

echo "[2/7] stage2: shadow-stage1 compile src/main.shadow"
build/shadow-stage1.exe src/main.shadow -o build/stage2.ll
"$LLCC" -O0 -filetype=obj build/stage2.ll -o build/stage2.o
"$CLANG" -std=c++17 build/stage2.o build/rt/runtime_for_selfhost_wk.o "$BOOT/miniz.o" build/rt/shadow_gc_supplement.o build/rt/rt_zip.o build/rt/shadow_index.o build/rt/rt_proc_spawn.o build/rt/sys_exec_cpa.o -o build/shadow-stage2.exe -Wl,/subsystem:console -lws2_32 -lLLVM-C -L"$RT_LIB"
echo "    wrote build/shadow-stage2.exe"

echo "[3/7] stage3: shadow-stage2 compile src/main.shadow"
build/shadow-stage2.exe src/main.shadow -o build/stage3.ll
echo "    wrote build/stage3.ll"

echo "[4/7] verify fixed-point"
python tools/verify_fixed_point.py build/stage2.ll build/stage3.ll

echo "[5/7] final artifact shadow.exe"
cp -f build/shadow-stage2.exe build/shadow.exe
echo "    wrote build/shadow.exe"

# 注：无独立的运行时编译单元。src/runtime/runtime_lib.shadow 作为普通模块
# 直接参与 combined 编译。

echo "[6/7] compile rt/ system-call layer"
# rt_zip 已在 [0/7] 编译，此处不重复。
for f in rt_core rt_fs rt_proc rt_proc_spawn rt_time rt_err rt_extra rt_gc rt_thread rt_io; do
  "$CLANG_C" -O0 -I "$BOOT" -c "rt/$f.c" -o "build/rt/$f.o" || echo "WARN: rt/$f.c compile failed"
done

echo "[7/7] smoke --version"
build/shadow.exe --version || true

echo "done"
