#!/bin/bash
# Linux 基准（仅基准）：shadow --exe vs Go。修正 .exe 后缀在 Linux 无法直接执行的问题。
set -u
export PATH=/usr/local/go/bin:/usr/local/bin:/usr/bin:/bin:/usr/lib/llvm-20/bin
export LLVM_HOME=/usr/lib/llvm-20
ROOT=/mnt/d/shadow/shadow-0.5
cd "$ROOT"
SHADOW_EXE="$ROOT/build/linux/shadow"
benches="sum_loop fib matmul str_concat array_push quicksort float_pi string_find prime_sieve ackermann str_reverse"
# 编译 shadow 基准（--exe 默认 -O2），并把 *.shadow.exe 改名为无后缀以便 Linux 直接执行
for b in $benches; do
  "$SHADOW_EXE" "$ROOT/bench/shadow/$b.shadow" --exe >/dev/null 2>&1
  [ -f "$ROOT/bench/shadow/$b.shadow.exe" ] && mv -f "$ROOT/bench/shadow/$b.shadow.exe" "$ROOT/bench/shadow/$b.linux"
done
# 编译 Go 基准
cd "$ROOT/bench/go"
for b in $benches; do go build -o "$b" "$b.go" 2>&1 | tail -1; done
cd "$ROOT"
{
  echo "bench shadow_ms go_ms shadow/go"
  for b in $benches; do
    EXE="$ROOT/bench/shadow/$b.linux"
    [ -x "$EXE" ] || EXE="$ROOT/bench/shadow/$b"
    sb=999999
    for i in 1 2 3; do t0=$(date +%s%N); "$EXE" >/dev/null 2>&1; t1=$(date +%s%N); s=$(( (t1-t0)/1000000 )); [ "$s" -lt "$sb" ] && sb=$s; done
    gb=999999
    for i in 1 2 3; do t0=$(date +%s%N); "$ROOT/bench/go/$b" >/dev/null 2>&1; t1=$(date +%s%N); g=$(( (t1-t0)/1000000 )); [ "$g" -lt "$gb" ] && gb=$g; done
    r=$(python3 -c "print('%.2f' % ($sb/$gb))")
    echo "$b $sb $gb $r"
  done
} > build/_linux_bench.log 2>&1
echo "BENCH_DONE"
