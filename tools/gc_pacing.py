#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""GC pacing 验收工具 —— 开发原则 §5.3 第 3 条

    「触发：GOGC 模型下堆峰值 ≈ (1 + GOGC/100) × 存活堆」

这条验收标准此前无法验证：所有 GC 用例的存活集都只有几十 KB，远在最小堆
阈值（4MB）之下，`next` 恒等于 4194304 —— GOGC 比例模型根本没进入工作区。
ex_gc_pacing.shadow 专为此建，把存活堆顶到 4MB 以上。

本工具在多个 GOGC 档位上跑该用例，从 [GC] done 日志抽取 live 与 peak，
核对实测比值 peak/live 是否落在目标比 (1+GOGC/100) 的容差带内。

判定口径（重要）：
  · **超标是硬失败** —— 峰值击穿 goal 意味着 GOGC 承诺的内存上限失效，
    程序的实际内存占用不再可预测，这是 pacing 的根本失职；
  · **欠标只是次优** —— 比目标省内存不会出错，但意味着 GC 跑得比必要的勤，
    白白付出 CPU。容忍但要报出来，欠得太多同样说明控制器没调好。
故容差是不对称的：上界紧（+8%），下界松（-25%）。

用法：
    python tools/gc_pacing.py                # 默认档位
    python tools/gc_pacing.py --gogc 50,100  # 指定档位
    python tools/gc_pacing.py --strict       # 欠标也算失败（调参时用）
"""
import argparse
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CASE = os.path.join(ROOT, "test", "cases", "exe", "ex_gc_pacing.shadow.exe")

DEFAULT_GOGC = [25, 50, 100, 200, 400]
TOL_OVER = 0.08    # 超标容差：目标比的 +8%
TOL_UNDER = 0.25   # 欠标容差：目标比的 -25%
TIMEOUT = 180

DONE_RE = re.compile(r"\[GC\] done .*?live=(\d+).*?goal=(\d+).*?peak=(\d+)")


def run_once(gogc, extra_env=None):
    """跑一次用例，返回 (live, goal, peak, cycles, stdout_tail)。"""
    env = dict(os.environ)
    for k in ("SHADOW_GC_STRESS", "SHADOW_GC_VERIFY", "SHADOW_GC_POISON",
              "SHADOW_GC_ASSIST", "SHADOW_GC_PACING", "SHADOW_GC_MARK_BUDGET",
              "SHADOW_GC_SWEEP_BUDGET", "SHADOW_GC_LAZY_SWEEP"):
        env.pop(k, None)
    env["SHADOW_GC_LOG"] = "1"
    env["SHADOW_GOGC"] = str(gogc)
    if extra_env:
        env.update(extra_env)

    proc = subprocess.run([CASE], env=env, capture_output=True,
                          text=True, errors="replace", timeout=TIMEOUT)
    lines = [m for m in (DONE_RE.search(l) for l in proc.stderr.splitlines()) if m]
    if not lines:
        return None, None, None, 0, proc.stdout.strip()[-200:], proc.returncode
    # 取最后一个周期：此时 pacing 控制器已收敛，前几轮是暖机
    last = lines[-1]
    live, goal, peak = (int(last.group(i)) for i in (1, 2, 3))
    return live, goal, peak, len(lines), proc.stdout.strip()[-200:], proc.returncode


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gogc", default=None, help="逗号分隔的 GOGC 档位")
    ap.add_argument("--strict", action="store_true", help="欠标也判失败")
    args = ap.parse_args()

    if not os.path.exists(CASE):
        print("[ERR] 找不到用例产物：%s\n先跑 test/run_tests.py 生成。" % CASE)
        return 2

    gogcs = ([int(x) for x in args.gogc.split(",")] if args.gogc else DEFAULT_GOGC)

    print("=" * 78)
    print("GC pacing 验收（开发原则 §5.3-3：堆峰值 ≈ (1 + GOGC/100) × 存活堆）")
    print("=" * 78)
    print("%-6s %-9s %-11s %-11s %-9s %-9s %-7s %s" %
          ("GOGC", "目标比", "live", "peak", "实测比", "偏差", "周期", "判定"))
    print("-" * 78)

    failures = []
    for gogc in gogcs:
        live, goal, peak, cycles, out, rc = run_once(gogc)
        if live is None:
            print("%-6d %s" % (gogc, "无 GC 日志 —— 用例没跑起来？out=%r rc=%s" % (out, rc)))
            failures.append((gogc, "no-gc-log"))
            continue
        if rc != 0 or not out.startswith("pacing ok"):
            print("%-6d %s" % (gogc, "用例失败 rc=%s out=%r" % (rc, out)))
            failures.append((gogc, "case-failed"))
            continue

        target = 1.0 + gogc / 100.0
        actual = peak / live if live else 0.0
        dev = (actual - target) / target

        if dev > TOL_OVER:
            verdict, bad = "超标 FAIL", True
        elif dev < -TOL_UNDER:
            verdict, bad = "欠标 WARN", args.strict
        else:
            verdict, bad = "OK", False
        if bad:
            failures.append((gogc, verdict))

        print("%-6d %-9.2f %-11d %-11d %-9.2f %+8.1f%% %-7d %s" %
              (gogc, target, live, peak, actual, dev * 100, cycles, verdict))

    print("-" * 78)
    if failures:
        print("FAIL：%d 个档位不达标 → %s" % (len(failures), failures))
        return 1
    print("PASS：全部档位堆峰值落在 (1+GOGC/100)×live 的容差带内")
    return 0


if __name__ == "__main__":
    sys.exit(main())
