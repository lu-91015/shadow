#!/bin/bash
# 缓存正确性的「真实程序」验收：全量套件连跑两遍。
#   第 1 遍：默认行为（启动归档清空 lu_cache）→ 约 900 用例各自冷编译，末尾写自己的 .lu
#            （键 = djb2(用例路径)，每用例唯一，并行写不同键无冲突）
#   第 2 遍：必须加 --no-clean-cache，否则 run_tests.py 启动就把第 1 遍的缓存归档清空，
#            两遍都变冷跑 → 根本验不到热命中（run_tests.py:278-288）。
# 判据：两遍 PASS/FAIL/CRASH 完全一致，且第 2 遍无 retry。
# 若闭包编号漂移 / 泛型实例化丢失 / MIR 队列错位 中任一仍存在，第 2 遍必炸。
#
# 🔴 假绿防护：run_case 对非 xfail 的 FAIL 会「归档清空 lu_cache 后重试一次」
#    （run_tests.py:152-164），重试通过则记 PASS(msg="retry")。
#    这会把「缓存导致的失败」洗成 PASS。
#    ⚠️ PASS 分支只 print("[PASS] {rel}") **不打印 msg** → 日志里看不到 "retry" 字样，
#       唯一可靠信号是 build/_lu_retry_* 目录新增数（每次重试 os.rename 造一个）。
#       注意其名用秒级时间戳，同秒并发重试会撞名（OSError 被吞）→ 计数是**下界**，
#       只要 >0 就说明发生过重试，须逐个排查。
#    旁证：第 2 遍若发生重试，lu_cache 被清空，结束时 .lu 数量会显著少于第 1 遍。
cd "C:/Users/cxyu/AppData/Roaming/TRAE SOLO CN/ModularData/ai-agent/work-mode-projects/6a5353846df2d3b188c57699/shadow/shadow-0.5" || exit 1
PY="C:/Users/cxyu/.workbuddy/binaries/python/versions/3.13.12/python.exe"
EXE="$(cygpath -w "$(pwd)/build/vc.exe")"
export SHADOW_EXE="$EXE"
echo "SHADOW_EXE=$SHADOW_EXE"
[ -f build/vc.exe ] || { echo "!!! build/vc.exe 不存在，先跑 tools/verify_bootstrap_cache.sh"; exit 1; }

R0=$(ls -d build/_lu_retry_* 2>/dev/null | wc -l)

echo "=== 套件第 1 遍（冷：清空缓存起步，末尾写缓存）==="
T0=$(date +%s)
"$PY" test/run_tests.py -j 8 > build/suite_pass1.log 2>&1
T1=$(date +%s)
tail -12 build/suite_pass1.log
echo "PASS1 elapsed=$((T1-T0))s"
R1=$(ls -d build/_lu_retry_* 2>/dev/null | wc -l)
echo "第1遍 retry 目录新增: $((R1-R0))（>0 表示首遍已有瞬时失败）"
LU1=$(ls build/lu_cache 2>/dev/null | wc -l)
echo "第1遍结束 .lu 数量: $LU1"

echo ""
echo "=== 套件第 2 遍（热：--no-clean-cache 全命中）==="
T2=$(date +%s)
"$PY" test/run_tests.py -j 8 --no-clean-cache > build/suite_pass2.log 2>&1
T3=$(date +%s)
tail -12 build/suite_pass2.log
echo "PASS2 elapsed=$((T3-T2))s"
R2=$(ls -d build/_lu_retry_* 2>/dev/null | wc -l)
echo "第2遍 retry 目录新增: $((R2-R1))"
LU2=$(ls build/lu_cache 2>/dev/null | wc -l)
echo "第2遍结束 .lu 数量: $LU2（应 ≈ 第1遍的 $LU1；显著变少=中途被重试清空）"

echo "=========================================="
# 汇总行形如 "  PASS=923 FAIL=0 CRASH=0 SKIP=2 TOTAL=925"（有前导空格，勿用 ^ 锚定）
S1=$(grep -E "PASS=[0-9]+ FAIL=" build/suite_pass1.log | tail -1 | sed 's/^ *//')
S2=$(grep -E "PASS=[0-9]+ FAIL=" build/suite_pass2.log | tail -1 | sed 's/^ *//')
echo "第1遍: $S1"
echo "第2遍: $S2"
[ "$S1" = "$S2" ] && echo "[OK]   A. 两遍统计一致" || echo "[FAIL] A. 两遍统计不一致 → 缓存改变了真实程序行为"
[ "$((R2-R1))" -eq 0 ] && echo "[OK]   B. 第2遍无 retry（无被掩盖的缓存失败）" || echo "[FAIL] B. 第2遍发生 $((R2-R1)) 次 retry → 有缓存失败被重试洗白，须逐个排查"
echo "C. 耗时 pass1=$((T1-T0))s pass2=$((T3-T2))s"
echo "=========================================="
