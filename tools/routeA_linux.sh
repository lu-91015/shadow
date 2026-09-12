#!/bin/bash
# Route A (Linux target) —— 由 tools/build_shadow.shadow [8] 经 WSL 调用。
#
# 把 Windows 产出的固定点 build/stage2.ll（== stage3.ll，已收敛）交叉编译/链接为 build/linux/shadow。
# 关键：必须用固定点 stage2.ll，而非单轮 stage1 中间态（未收敛会致 prealloc 漂移）。
#
# 用法（由编排器调用，无需手动）：
#   bash tools/routeA_linux.sh <SHADOW_ROOT_LIN> <LLVM_HOME_LIN>
#
# 链接配方要点（与 Windows 不同，切勿照搬 __cpp_ 重命名）：
#   - Linux 用 runtime_for_selfhost.o + `objcopy --weaken`（弱符号消解 stage2.o 内嵌 std 与 C++ runtime 重名）。
#   - 编译器本体链接 shadow_gc_supplement.o（no-op GC）。
#   - 用户产物 runtime 目录 $L/rt/ 只放真 GC 对象，绝不放入 shadow_gc_supplement.o。
set -u
ROOT="${1:-/mnt/d/shadow/shadow-0.5}"
LLVM_HOME="${2:-/usr/lib/llvm-20}"
L="$ROOT/build/linux"
LLC="$LLVM_HOME/bin/llc"
export LLVM_HOME
mkdir -p "$L" "$L/rt"
cd "$ROOT" || exit 1
export SHADOW_LU_CACHE_DIR="$ROOT/build/.lu_fresh_$$"
mkdir -p "$SHADOW_LU_CACHE_DIR"

# === [0] 守卫：禁止 build/src 影子副本存在 ===
# 实测：build/linux/shadow 与 build/shadow.exe 在没有 build/src 时都能正确加载 src/runtime/runtime_lib.shadow。
# 而 build/src 一旦存在，main.shadow 的 exe_dir/../src/runtime 候选会抢占正确文件，副本陈旧即复现 prealloc 误判 extern。
if [ -e "$ROOT/build/src" ]; then
  echo "=== [0] 删除影子副本 build/src（唯一事实来源=src/runtime）==="
  rm -rf "$ROOT/build/src"
else
  echo "=== [0] OK：无 build/src 影子副本 ==="
fi

echo "=== [1] 交叉编译 build/stage2.ll -> stage2.o (固定点) ==="
"$LLC" -O0 -mtriple=x86_64-unknown-linux-gnu -filetype=obj -o "$L/stage2.o" "$ROOT/build/stage2.ll"
echo "llc_rc=$?  ($(stat -c%s "$L/stage2.o") bytes)"
nm "$L/stage2.o" 2>/dev/null | grep -E 'shadow_array_prealloc|shadow_string_concat_inplace'

echo
echo "=== [2] 全部 runtime .o 从源码现编（源仅在 rt/）==="
clang++ -c "$ROOT/rt/linux/runtime_for_selfhost.cpp" -o "$L/runtime_for_selfhost.o" -I"$ROOT/rt" -I"$LLVM_HOME/include" -std=c++17 -fPIC -O2
objcopy --weaken "$L/runtime_for_selfhost.o"
clang -c "$ROOT/rt/miniz.c" -o "$L/miniz.o" -I"$ROOT/rt" -D_GNU_SOURCE -std=c11 -fPIC -O1
clang -c "$ROOT/rt/rt_zip.c" -o "$L/rt_zip.o" -I"$ROOT/rt" -D_GNU_SOURCE -std=c11 -fPIC -O1
clang -c "$ROOT/rt/linux/sys_exec_cpa.c" -o "$L/sys_exec_cpa.o" -I"$ROOT/rt" -std=c11 -fPIC -O1
clang -c -D_GNU_SOURCE "$ROOT/rt/rt_proc_spawn.c" -o "$L/rt_proc_spawn.o" -I"$ROOT/rt" -std=c11 -fPIC -O1
clang -c -D_GNU_SOURCE "$ROOT/rt/shadow_index.c" -o "$L/shadow_index.o" -I"$ROOT/rt" -std=c11 -fPIC -O1
clang -c "$ROOT/rt/shadow_gc_supplement.c" -o "$L/shadow_gc_supplement.o" -I"$ROOT/rt" -std=c11 -fPIC -O1
echo "rt objects ready: $(ls "$L"/*.o | tr '\n' ' ')"

# 同步「用户产物」runtime 目录：main.shadow 的 Linux 分支按 self_dir/rt/ 取 .o 链接用户 exe，
# 不同步就会链到上一轮陈旧副本。注意不含 shadow_gc_supplement.o（no-op GC 仅供编译器本体，用户产物必须用真 GC）。
cp -f "$L/runtime_for_selfhost.o" "$L/miniz.o" "$L/rt_zip.o" "$L/sys_exec_cpa.o" \
      "$L/rt_proc_spawn.o" "$L/shadow_index.o" "$L/rt/"
rm -f "$L/rt/shadow_gc_supplement.o"
echo "user-product runtime 已同步 -> $L/rt ($(ls -1 "$L/rt"/*.o | wc -l) 个 .o)"

echo
echo "=== [3] 链接 build/linux/shadow（stage2 固定点） ==="
clang++ -no-pie -O1 -fPIC -rdynamic \
  "$L/stage2.o" "$L/runtime_for_selfhost.o" "$L/miniz.o" "$L/sys_exec_cpa.o" \
  "$L/rt_proc_spawn.o" "$L/shadow_index.o" "$L/shadow_gc_supplement.o" \
  -o "$L/shadow" \
  -L"$LLVM_HOME/lib" -Wl,--no-as-needed -lLLVM-20 -lcurl -lpthread -ldl -lm
echo "link_rc=$?"
ls -la "$L/shadow"

echo
echo "=== [4] 端到端验证：matmul prealloc emit ==="
"$L/shadow" "$ROOT/bench/shadow/matmul.shadow" -o "$L/m_new.ll" 2>/dev/null
echo "prealloc define=$(grep -cE '^define.*shadow_array_prealloc' "$L/m_new.ll" 2>/dev/null)  declare=$(grep -cE 'declare.*shadow_array_prealloc' "$L/m_new.ll" 2>/dev/null)"
echo "string_concat_inplace define=$(grep -cE '^define.*shadow_string_concat_inplace' "$L/m_new.ll" 2>/dev/null)  declare=$(grep -cE 'declare.*shadow_string_concat_inplace' "$L/m_new.ll" 2>/dev/null)"

echo
echo "=== [5] 端到端链接+运行 matmul（验证 prealloc 不再 undefined reference） ==="
"$L/shadow" "$ROOT/bench/shadow/matmul.shadow" -o "$L/matmul.ll" 2>/dev/null
echo "compile_rc=$?"
"$LLC" -O0 -filetype=obj -o "$L/matmul.o" "$L/matmul.ll"
echo "llc_rc=$?"
clang++ -no-pie -O1 -fPIC -rdynamic \
  "$L/matmul.o" "$L/runtime_for_selfhost.o" "$L/miniz.o" "$L/sys_exec_cpa.o" \
  "$L/rt_proc_spawn.o" "$L/shadow_index.o" "$L/shadow_gc_supplement.o" \
  -o "$L/matmul_exe" \
  -L"$LLVM_HOME/lib" -Wl,--no-as-needed -lLLVM-20 -lcurl -lpthread -ldl -lm
echo "link_rc=$?"
if [ -x "$L/matmul_exe" ]; then
  "$L/matmul_exe" 2>&1 | head -8
  echo "run_rc=$?"
else
  echo "LINK FAILED (prealloc likely still undefined)"
fi
echo
echo "=== done ==="
