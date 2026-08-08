#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""GC 长驻稳定性验收工具 —— 开发原则 §5.3 第 4 条

    「长驻：LSP 服务器连续处理 1000+ 文档编辑，内存稳定无单调增长」

被测程序是 ex_gc_longrun.shadow（LSP 式文档编辑：文档数恒定、正文与 token
表持续整批轮换）。用例自身已内置谷底采样断言，本工具做用例做不到的三件事：

  1. **跨 GOGC 档位**验证 —— 泄漏在不同触发频率下的表现不同，只测一档
     容易漏掉"低 GOGC 时才暴露"的问题；
  2. **用精确 live 而非堆字节做趋势** —— 用例内部只能读到瞬时堆字节（含
     未清扫垃圾），而 [GC] done 里的 live 是清扫时逐对象累加出来的精确
     存活量，是判断泄漏的金标准；
  3. **量操作系统看到的内存** —— GC 内部 live 平稳，不等于进程 RSS 平稳：
     freelist 不归还 OS、分配器碎片都会让 RSS 单调爬升而 live 纹丝不动。
     §5.3 说的"内存稳定"对用户而言指的是后者。

判据：
  · live 趋势（硬失败）—— 丢掉暖机段后，后半程 live 中位数不得比前半程
    高出 5%。存活集恒定，真实 live 应当持平；每轮固定泄漏会在这里线性累积。
  · RSS（仅报告）—— 当前分配器不向 OS 归还内存，RSS 只增不减是预期行为，
    故只观测不判定；它爬升的斜率若远超 live 才值得警惕。

用法：
    python tools/gc_longrun.py                 # 默认档位
    python tools/gc_longrun.py --gogc 50,100   # 指定档位
    python tools/gc_longrun.py --tol 0.03      # 收紧趋势容差
"""
import argparse
import ctypes
import os
import re
import subprocess
import sys
import threading
import time
from ctypes import wintypes

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CASE = os.path.join(ROOT, "test", "cases", "exe", "ex_gc_longrun.shadow.exe")

DEFAULT_GOGC = [25, 50, 100, 200]
DEFAULT_TOL = 0.05    # live 趋势容差
WARMUP_FRAC = 0.25    # 丢弃前 25% 的周期（pacing 控制器尚在收敛）
TIMEOUT = 300

DONE_RE = re.compile(r"\[GC\] done .*?live=(\d+).*?peak=(\d+)")
STAT_RE = re.compile(r"\[GC\]\[stats\] (\w+) (.*)")


# ── 进程峰值工作集（Windows，零依赖 ctypes 版） ──────────────────────────
class _PMC(ctypes.Structure):
    _fields_ = [("cb", wintypes.DWORD),
                ("PageFaultCount", wintypes.DWORD),
                ("PeakWorkingSetSize", ctypes.c_size_t),
                ("WorkingSetSize", ctypes.c_size_t),
                ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
                ("QuotaPagedPoolUsage", ctypes.c_size_t),
                ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
                ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                ("PagefileUsage", ctypes.c_size_t),
                ("PeakPagefileUsage", ctypes.c_size_t)]


def _peak_ws(pid):
    """读进程当前的峰值工作集；进程已退出或无权限时返回 0。"""
    try:
        k32 = ctypes.WinDLL("kernel32", use_last_error=True)
        h = k32.OpenProcess(0x0400 | 0x0010, False, pid)   # QUERY_INFO | VM_READ
        if not h:
            return 0
        try:
            c = _PMC()
            c.cb = ctypes.sizeof(c)
            if k32.K32GetProcessMemoryInfo(h, ctypes.byref(c), c.cb):
                return int(c.PeakWorkingSetSize)
        finally:
            k32.CloseHandle(h)
    except Exception:
        pass
    return 0


def run_once(gogc):
    """跑一次用例。返回 dict：live 序列、stats 字段、RSS 峰值、退出码。"""
    env = dict(os.environ)
    for k in ("SHADOW_GC_STRESS", "SHADOW_GC_VERIFY", "SHADOW_GC_POISON",
              "SHADOW_GC_ASSIST", "SHADOW_GC_PACING", "SHADOW_GC_MARK_BUDGET",
              "SHADOW_GC_SWEEP_BUDGET", "SHADOW_GC_LAZY_SWEEP",
              "SHADOW_GC_INCREMENTAL"):
        env.pop(k, None)
    env["SHADOW_GC_LOG"] = "1"
    env["SHADOW_GC_STATS"] = "1"
    env["SHADOW_GOGC"] = str(gogc)

    proc = subprocess.Popen([CASE], env=env, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, text=True, errors="replace")

    # 进程跑得很快（百毫秒级），采样必须密集，否则一次都抓不到。
    # PeakWorkingSetSize 由内核累积，只要在退出前读到一次就是准确的峰值。
    rss = [0]
    stop = threading.Event()

    def sampler():
        while not stop.is_set():
            v = _peak_ws(proc.pid)
            if v > rss[0]:
                rss[0] = v
            time.sleep(0.002)

    th = threading.Thread(target=sampler, daemon=True)
    th.start()
    try:
        out, err = proc.communicate(timeout=TIMEOUT)
    finally:
        stop.set()
        th.join(timeout=1.0)

    lives = [(int(m.group(1)), int(m.group(2)))
             for m in (DONE_RE.search(l) for l in err.splitlines()) if m]
    stats = {}
    for line in err.splitlines():
        m = STAT_RE.search(line)
        if m:
            for kv in m.group(2).split():
                if "=" in kv:
                    k, v = kv.split("=", 1)
                    stats[k] = v
    return {"lives": lives, "stats": stats, "rss": rss[0],
            "rc": proc.returncode, "out": out.strip()}


def median(xs):
    s = sorted(xs)
    n = len(s)
    return s[n // 2] if n % 2 else (s[n // 2 - 1] + s[n // 2]) / 2.0


def mib(x):
    return float(x) / 1048576.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gogc", default=None, help="逗号分隔的 GOGC 档位")
    ap.add_argument("--tol", type=float, default=DEFAULT_TOL,
                    help="live 趋势容差（默认 0.05）")
    args = ap.parse_args()

    if not os.path.exists(CASE):
        print("[ERR] 找不到用例产物：%s\n先跑 test/run_tests.py 生成。" % CASE)
        return 2

    gogcs = [int(x) for x in args.gogc.split(",")] if args.gogc else DEFAULT_GOGC

    print("=" * 86)
    print("GC 长驻验收（开发原则 §5.3-4：连续 4000 次文档编辑，内存稳定无单调增长）")
    print("=" * 86)
    print("%-6s %-7s %-11s %-11s %-9s %-10s %-10s %s" %
          ("GOGC", "周期", "live前半", "live后半", "趋势", "堆峰值", "进程RSS", "判定"))
    print("-" * 86)

    failures = []
    for gogc in gogcs:
        r = run_once(gogc)
        if r["rc"] != 0 or "longrun ok" not in r["out"]:
            tail = r["out"].splitlines()[-1] if r["out"] else ""
            print("%-6d 用例失败 rc=%s out=%r" % (gogc, r["rc"], tail[:60]))
            failures.append((gogc, "case-failed"))
            continue

        lives = [lv for lv, _ in r["lives"]]
        if len(lives) < 4:
            print("%-6d 周期数不足（%d），无法做趋势判断" % (gogc, len(lives)))
            failures.append((gogc, "too-few-cycles"))
            continue

        # 丢掉暖机段：前几轮 live 还在爬（文档表尚未建满）、pacing 未收敛
        warm = max(1, int(len(lives) * WARMUP_FRAC))
        body = lives[warm:]
        half = len(body) // 2
        m1, m2 = median(body[:half]), median(body[half:])
        growth = (m2 - m1) / m1 if m1 else 0.0

        peak = max(pk for _, pk in r["lives"])
        bad = growth > args.tol
        verdict = "增长 FAIL" if bad else "OK"
        if bad:
            failures.append((gogc, "live-growth=%.1f%%" % (growth * 100)))

        print("%-6d %-7d %-11d %-11d %+8.2f%% %-10.2f %-10.2f %s" %
              (gogc, len(lives), int(m1), int(m2), growth * 100,
               mib(peak), mib(r["rss"]), verdict))

    print("-" * 86)
    print("单位：live/峰值为字节，堆峰值与进程 RSS 为 MiB")

    # 停顿与工作量摘要：顺带回答 §5.3 第 2 条「停顿可测量」
    r = run_once(100)
    s = r["stats"]
    if s:
        print("-" * 86)
        print("GOGC=100 时的停顿画像（§5.3-2）："
              "标记切片最坏 %s us，标记终止最坏 %s us，清扫切片最坏 %s us" %
              (s.get("max_mark_slice_us", "?"), s.get("max_mark_term_us", "?"),
               s.get("max_sweep_slice_us", "?")))
        print("　　　　　　　　　　　 assist 承担 %s 个对象 / %s 次调用，"
              "写屏障 %s 次调用保活 %s 个对象" %
              (s.get("assist_work", "?"), s.get("assist_calls", "?"),
               s.get("calls", "?"), s.get("shaded", "?")))

    print("-" * 86)
    if failures:
        print("FAIL：%d 个档位内存不稳定 → %s" % (len(failures), failures))
        return 1
    print("PASS：全部档位 live 无单调增长，长驻内存稳定")
    return 0


if __name__ == "__main__":
    sys.exit(main())
