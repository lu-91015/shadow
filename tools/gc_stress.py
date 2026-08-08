#!/usr/bin/env python3
"""
Shadow 0.4 — GC 压测矩阵（原则 7 / §5.3 验收）

普通测试套件只跑默认 GC 配置，而 GC 的 bug（漏根、误回收、堆损坏）在默认
配置下往往几小时才随机崩一次。本工具把每个 _gc_ 用例在一组极端配置下反复跑，
把"偶发"变成"必现"。

配置维度（环境变量，见 rt/rt_gc.c、rt/rt_core.c）：
    SHADOW_GOGC=N          堆增长 N% 触发（默认 100）
    SHADOW_GC_STRESS=N     每 N 次分配强制 GC，无视堆阈值（等价 Go gcstress）
                           N=1 最灵敏：任何未登记的活引用都会立刻被回收并暴露
    SHADOW_GC_POISON=1     回收时数据区填 0xDD，使"误回收后仍使用"确定性崩溃
    SHADOW_GC_LOG=1        每轮 GC 输出统计
    SHADOW_GC_DEBUG=1      输出阶段标记与一致性校验计数
    SHADOW_GC_VERIFY=2     每轮标记终止后再做一次全量重标记，检出增量漏标即 abort
                           （比"等它踩到毒化内存"更早、更确定地定位屏障缺口）
    SHADOW_GC_INCREMENTAL=0  退回 STW 标记-清扫，用于隔离"是不是增量引入的问题"
    SHADOW_GC_LAZY_SWEEP=0   退回一次性整表清扫（惰性清扫的 A/B 对照组）
    SHADOW_GC_SWEEP_BUDGET=N 固定每次分配清扫 N 个槽（不设则按对象表规模自适应）。
                           N=1 把清扫窗口拉到最长（跨越十万次分配），是检验
                           "清扫窗口内新分配会不会被游标误杀"的最强手段

用法：
    python tools/gc_stress.py              # 默认矩阵
    python tools/gc_stress.py --quick      # 快速档（CI 用）
    python tools/gc_stress.py -k strings   # 只跑名字含 strings 的用例

退出码：0 = 全部通过，1 = 存在失败
"""

import os
import sys
import glob
import time
import argparse
import subprocess

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SHADOW_EXE = os.path.join(REPO_ROOT, "build", "shadow.exe")
CASE_GLOB = os.path.join(REPO_ROOT, "test", "cases", "exe", "*_gc_*.shadow")

# (标签, 环境变量增量)。STRESS=1 最慢也最灵敏，放最后。
FULL_MATRIX = [
    ("default",          {}),
    ("gogc=20",          {"SHADOW_GOGC": "20"}),
    ("gogc=100+poison",  {"SHADOW_GOGC": "100", "SHADOW_GC_POISON": "1"}),
    ("stress=64",        {"SHADOW_GC_STRESS": "64"}),
    ("stress=16+poison", {"SHADOW_GC_STRESS": "16", "SHADOW_GC_POISON": "1"}),
    # 增量正确性专档：每周期重标记比对，漏标即 abort
    ("stress=16+verify", {"SHADOW_GC_STRESS": "16", "SHADOW_GC_VERIFY": "2"}),
    ("verify+budget=1",  {"SHADOW_GC_STRESS": "8", "SHADOW_GC_VERIFY": "2",
                          "SHADOW_GC_MARK_BUDGET": "1"}),
    # STW 回退档：增量出问题时用它二分定位
    ("stw+poison",       {"SHADOW_GC_INCREMENTAL": "0", "SHADOW_GC_STRESS": "16",
                          "SHADOW_GC_POISON": "1"}),
    # 惰性清扫专档（Task #32）
    # 清扫窗口拉到最长 + 毒化：窗口内的新分配一旦被游标误杀，
    # 后续使用必然踩到 0xDD 而确定性崩溃，不必守株待兔。
    ("sweep=1+poison",   {"SHADOW_GC_SWEEP_BUDGET": "1", "SHADOW_GC_STRESS": "64",
                          "SHADOW_GC_POISON": "1"}),
    # 长清扫窗口 + 高频 GC + 漏标检测三重叠加
    ("sweep=32+verify",  {"SHADOW_GC_SWEEP_BUDGET": "32", "SHADOW_GC_STRESS": "16",
                          "SHADOW_GC_VERIFY": "2"}),
    # 一次性清扫对照组：惰性清扫出问题时用它二分定位
    ("lazysweep=0",      {"SHADOW_GC_LAZY_SWEEP": "0", "SHADOW_GC_STRESS": "16",
                          "SHADOW_GC_POISON": "1"}),
    ("stress=1",         {"SHADOW_GC_STRESS": "1"}),
]

QUICK_MATRIX = [
    ("default",          {}),
    ("gogc=20",          {"SHADOW_GOGC": "20"}),
    ("stress=64+poison", {"SHADOW_GC_STRESS": "64", "SHADOW_GC_POISON": "1"}),
    ("stress=16+verify", {"SHADOW_GC_STRESS": "16", "SHADOW_GC_VERIFY": "2"}),
    ("sweep=1+poison",   {"SHADOW_GC_SWEEP_BUDGET": "1", "SHADOW_GC_STRESS": "64",
                          "SHADOW_GC_POISON": "1"}),
]

TIMEOUT = 900


def build_case(src: str) -> str:
    """用自举编译器编译链接用例，返回产物 exe 路径。"""
    exe = src + ".exe"
    p = subprocess.run([SHADOW_EXE, src, "--run"],
                       capture_output=True, text=True, timeout=300)
    if not os.path.exists(exe):
        raise RuntimeError(f"build failed: {(p.stderr or p.stdout).strip()[:200]}")
    return exe


def run_once(exe: str, env_delta: dict):
    env = dict(os.environ)
    # 清掉外部继承的 GC 配置，保证矩阵各档互不污染
    for k in ("SHADOW_GOGC", "SHADOW_GC_STRESS", "SHADOW_GC_POISON",
              "SHADOW_GC_LOG", "SHADOW_GC_DEBUG", "SHADOW_GC_VERIFY",
              "SHADOW_GC_INCREMENTAL", "SHADOW_GC_MARK_BUDGET",
              "SHADOW_GC_BARRIER", "SHADOW_GC_LAZY_SWEEP",
              "SHADOW_GC_SWEEP_BUDGET"):
        env.pop(k, None)
    env.update(env_delta)
    t0 = time.time()
    try:
        p = subprocess.run([exe], capture_output=True, text=True,
                           timeout=TIMEOUT, env=env)
    except subprocess.TimeoutExpired:
        return ("TIMEOUT", "", time.time() - t0)
    out = (p.stdout or "").strip()
    err = (p.stderr or "").strip()
    dt = time.time() - t0
    if p.returncode != 0:
        # rc<0 或 Windows 异常码 → 崩溃；否则是用例自身断言失败
        tag = "CRASH" if (p.returncode < 0 or p.returncode > 0x1000) else "FAIL"
        # SHADOW_GC_VERIFY=2 的 abort 才是最有信息量的那一行，优先展示
        for line in err.splitlines():
            if "verify]" in line and ("FATAL" in line or "missed=" in line):
                return (tag, line.strip(), dt)
        return (tag, (out or err).splitlines()[-1] if (out or err) else f"rc={p.returncode}", dt)
    # 用例约定：出错时打印以 ERR 开头的行
    for line in out.splitlines():
        if line.startswith("ERR"):
            return ("FAIL", line, dt)
    return ("PASS", out.splitlines()[-1] if out else "", dt)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("-k", "--keyword", default=None, help="只跑名字含该关键字的用例")
    ap.add_argument("--quick", action="store_true", help="快速档（CI 用）")
    args = ap.parse_args()

    if not os.path.exists(SHADOW_EXE):
        print(f"ERROR: {SHADOW_EXE} not found — run build.bat first")
        return 1

    cases = sorted(glob.glob(CASE_GLOB))
    if args.keyword:
        cases = [c for c in cases if args.keyword in os.path.basename(c)]
    if not cases:
        print("ERROR: no GC cases matched")
        return 1

    matrix = QUICK_MATRIX if args.quick else FULL_MATRIX
    print(f"=== GC stress matrix: {len(cases)} cases x {len(matrix)} configs ===")

    failures = []
    for src in cases:
        name = os.path.basename(src)[:-len(".shadow")]
        try:
            exe = build_case(src)
        except Exception as e:
            print(f"  [BUILD-FAIL] {name}: {e}")
            failures.append((name, "build", str(e)))
            continue
        for label, delta in matrix:
            status, detail, dt = run_once(exe, delta)
            mark = "PASS" if status == "PASS" else status
            print(f"  [{mark:7}] {name:20} {label:18} {dt:6.1f}s  {detail}")
            if status != "PASS":
                failures.append((name, label, detail))

    print("=" * 60)
    if failures:
        print(f"  FAILURES={len(failures)}")
        for n, l, d in failures:
            print(f"    {n} @ {l}: {d}")
        return 1
    print(f"  ALL PASS ({len(cases)} cases x {len(matrix)} configs)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
