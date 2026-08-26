#!/bin/bash
# Linux 运行脚本：测试套件 + 基准（shadow --exe vs Go），串行。
# 由 WSL 调用： wsl bash /mnt/d/shadow/shadow-0.5/tools/_linux_run.sh
set -u
# 干净 PATH，避免 Windows PATH 里的括号 (x86) 破坏 shell
export PATH=/usr/local/go/bin:/usr/local/bin:/usr/bin:/bin:/usr/lib/llvm-20/bin
export LLVM_HOME=/usr/lib/llvm-20
ROOT=/mnt/d/shadow/shadow-0.5
cd "$ROOT"
SHADOW_EXE="$ROOT/build/linux/shadow"

echo "=== [1/2] Linux basic tests (-j 8) ==="
SHADOW_EXE="$SHADOW_EXE" python3 test/run_tests.py -j 8 > build/_linux_tests.log 2>&1
echo "LINUX_TESTS_RC=$?" >> build/_linux_tests.log

echo "=== [2/2] Linux benchmarks (shadow --exe vs Go) ==="
benches="sum_loop fib matmul str_concat array_push quicksort float_pi string_find prime_sieve ackermann str_reverse"
# 编译 shadow 基准（--exe 默认 -O2）
for b in $benches; do
  "$SHADOW_EXE" "$ROOT/bench/shadow/$b.shadow" --exe >/dev/null 2>&1
done
# 编译 Go 基准
cd "$ROOT/bench/go"
for b in $benches; do go build -o "$b" "$b.go" 2>&1 | tail -1; done
cd "$ROOT"

{
  echo "bench shadow_ms go_ms shadow/go"
  for b in $benches; do
    # 定位 shadow 产出的可执行文件（命名可能因平台而异）
    EXE=""
    for cand in "$ROOT/bench/shadow/$b.shadow.exe" "$ROOT/bench/shadow/$b" "$ROOT/bench/shadow/${b}.exe"; do
      [ -x "$cand" ] && EXE="$cand" && break
    done
    [ -z "$EXE" ] && { echo "$b ERROR(no-shadow-exe)"; continue; }
    # shadow 计时（取 3 次最优，毫秒）
    sbest=999999
    for i in 1 2 3; do
      t0=$(date +%s%N); "$EXE" >/dev/null 2>&1; t1=$(date +%s%N)
      s=$(( (t1 - t0) / 1000000 ))
      [ "$s" -lt "$sbest" ] && sbest=$s
    done
    # go 计时
    gbest=999999
    for i in 1 2 3; do
      t0=$(date +%s%N); "$ROOT/bench/go/$b" >/dev/null 2>&1; t1=$(date +%s%N)
      g=$(( (t1 - t0) / 1000000 ))
      [ "$g" -lt "$gbest" ] && gbest=$g
    done
    ratio=$(awk "BEGIN{printf \"%.2f\", $sbest/$gbest}")
    echo "$b $sbest $gbest $ratio"
  done
} > build/_linux_bench.log 2>&1

echo "ALL_DONE"
