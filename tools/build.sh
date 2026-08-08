#!/usr/bin/env bash
# build.sh —— build.bat 的 Bash 等价物（供 MSYS/Git Bash 环境使用）。
# build.bat 在 GBK 代码页下会被 UTF-8 中文注释噎住，这里提供一条可靠的替代路径。
# 引导链与 build.bat 完全一致：bootstrap/s03.exe → stage1 → stage2 → stage3 → fixed-point
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

BOOT=bootstrap
LLVM_HOME="${LLVM_HOME:-D:/llvm/clang+llvm-22.1.0-x86_64-pc-windows-msvc}"
LLVM_BIN="$LLVM_HOME/bin"
LLVM_LIB="$LLVM_HOME/lib"
LLCC="$LLVM_BIN/llc.exe"
CLANGXX="$LLVM_BIN/clang++.exe"
CLANG_C="$LLVM_BIN/clang.exe"
PY="${PY:-python}"

# 编译器自身的源文件路径必须是 Windows 风格绝对路径：
# 解析器对正斜杠/相对路径的处理有 bug，会误报 missing 'dsb'。
MAIN_SRC="$(cygpath -w "$ROOT/src/main.shadow")"

die() { echo "ERROR: $*" >&2; exit 1; }

[ -f "$BOOT/s03.exe" ] || die "bootstrap/s03.exe not found. Freeze the 0.3 host first."
mkdir -p build build/rt

link_stage() {  # $1=obj  $2=exe
    "$CLANGXX" -std=c++17 "$1" "$BOOT/runtime_for_selfhost.o" "$BOOT/miniz.o" \
        build/rt/shadow_gc_supplement.o build/rt/rt_zip.o -o "$2" \
        -Wl,/subsystem:console -lws2_32 -lLLVM-C -L"$LLVM_LIB"
}

echo "[0/7] compile rt/ system-call layer"
for f in rt_core rt_fs rt_proc rt_time rt_err rt_extra rt_gc rt_thread rt_io; do
    "$CLANG_C" -O0 -c "rt/$f.c" -o "build/rt/$f.o" || echo "WARN: rt/$f.c compile failed"
done
"$CLANG_C" -O0 -Ibootstrap -c rt/rt_zip.c -o build/rt/rt_zip.o || echo "WARN: rt/rt_zip.c compile failed"

echo "[1/7] stage1: bootstrap/s03.exe compile src/main.shadow"
"$BOOT/s03.exe" "$MAIN_SRC" -o build/stage1.ll   || die "stage1 compile failed"
"$LLCC" -O0 -filetype=obj build/stage1.ll -o build/stage1.o || die "stage1 llc failed"
link_stage build/stage1.o build/shadow-stage1.exe || die "stage1 link failed"

echo "[2/7] stage2: shadow-stage1.exe compile src/main.shadow"
build/shadow-stage1.exe "$MAIN_SRC" -o build/stage2.ll || die "stage2 compile failed"
"$LLCC" -O0 -filetype=obj build/stage2.ll -o build/stage2.o || die "stage2 llc failed"
link_stage build/stage2.o build/shadow-stage2.exe || die "stage2 link failed"

echo "[3/7] stage3: shadow-stage2.exe compile src/main.shadow"
build/shadow-stage2.exe "$MAIN_SRC" -o build/stage3.ll || die "stage3 compile failed"

echo "[4/7] verify fixed-point (stage2.ll == stage3.ll)"
"$PY" tools/verify_fixed_point.py build/stage2.ll build/stage3.ll || die "fixed-point verification failed"

echo "[5/7] emit final artifact: build/shadow.exe"
cp -f build/shadow-stage2.exe build/shadow.exe || die "copy failed"

# 注：0.3 时代的 "emit runtime cache" 步骤已移除。0.4 架构下
# src/runtime/runtime_lib.shadow 作为普通模块直接参与 combined 编译，
# 产出的 runtime.o 从未被任何链接行消费。

echo "[6/7] smoke: shadow.exe --version"
build/shadow.exe --version || echo "WARN: --version not implemented yet - ignored"

echo "[7/7] running test suite..."
"$PY" test/run_tests.py
rc=$?
echo
echo "=== DONE: build/shadow.exe (tests rc=$rc) ==="
exit $rc
