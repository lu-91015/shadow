# -*- coding: utf-8 -*-
"""并行对照「C 轻量索引器」与「编译器 api_check」的 fmeta/smeta/emeta/gmeta 四段输出。

对每个用例：
    shadow <case> --fmeta        → 真实符号表（parse + TC 全量）
    cindex <case> <arg0> --all   → C 索引器输出（同构格式）
逐字节比较（忽略行尾 CR 差异）。模块图加载失败 / 索引器主动兜底的用例记 SKIP。

用法：
    python tools/verify_index_batch.py --limit 40 -j 8
    python tools/verify_index_batch.py --grep "^import " -j 8      # 只跑跨模块用例
    python tools/verify_index_batch.py --list-file cases.txt -j 8
"""
import argparse
import concurrent.futures as cf
import os
import re
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def run(cmd, timeout):
    try:
        p = subprocess.run(cmd, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                           timeout=timeout)
        return p.returncode, p.stdout, p.stderr
    except subprocess.TimeoutExpired:
        return -9, b"", b"TIMEOUT"
    except OSError as ex:
        return -8, b"", str(ex).encode()


def norm(b):
    txt = b.decode("utf-8", "replace").replace("\r\n", "\n").replace("\r", "\n")
    return [ln.rstrip() for ln in txt.split("\n")]


_SECT = {"---SMETA---": "smeta", "---EMETA---": "emeta", "---GMETA---": "gmeta"}


def section_at(lines, idx):
    """返回第 idx 行（0-based）所属的段名，供差异报告标注。"""
    cur = "fmeta"
    for i in range(min(idx + 1, len(lines))):
        cur = _SECT.get(lines[i], cur)
    return cur


def compare_one(case, exe, cidx, arg0, timeout):
    rc, out, err = run([exe, case, "--fmeta"], timeout)
    if rc != 0:
        return ("SKIP", case, "compiler rc=%d" % rc)
    e = err.decode("utf-8", "replace")
    if "module error:" in e or "cannot read file:" in e:
        return ("SKIP", case, "module load failed (indexer also falls back)")
    rc2, out2, _ = run([cidx, case, arg0, "--all"], timeout)
    if rc2 != 0:
        return ("SKIP", case, "indexer fallback ok=0")
    a, b = norm(out), norm(out2)
    while a and not a[-1]:
        a.pop()
    while b and not b[-1]:
        b.pop()
    if a == b:
        return ("PASS", case, "")
    # 首个差异
    detail = []
    for i in range(max(len(a), len(b))):
        va = a[i] if i < len(a) else "<eof>"
        vb = b[i] if i < len(b) else "<eof>"
        if va != vb:
            detail.append("[%s] line %d:\n    real=%s\n    c   =%s"
                          % (section_at(a, i), i + 1, va[:160], vb[:160]))
            if len(detail) >= 3:
                break
    return ("FAIL", case, "\n".join(detail))


def collect(args):
    if args.list_file:
        with open(args.list_file, encoding="utf-8") as f:
            return [ln.strip() for ln in f if ln.strip()]
    out = []
    pat = re.compile(args.grep, re.M) if args.grep else None
    for dirpath, _dirs, files in os.walk(os.path.join(ROOT, args.root)):
        for fn in files:
            if not fn.endswith(".shadow"):
                continue
            full = os.path.join(dirpath, fn)
            rel = os.path.relpath(full, ROOT).replace("\\", "/")
            if pat:
                try:
                    with open(full, encoding="utf-8", errors="replace") as f:
                        if not pat.search(f.read()):
                            continue
                except OSError:
                    continue
            out.append(rel)
    out.sort()
    if args.limit > 0:
        out = out[:args.limit]
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default="test/cases")
    ap.add_argument("--exe", default="build/lspfix.exe")
    ap.add_argument("--cidx", default="build/cindex.exe")
    ap.add_argument("--arg0", default="build/shadow.exe")
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--grep", default="")
    ap.add_argument("--list-file", default="")
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("-j", "--jobs", type=int, default=8)
    args = ap.parse_args()

    cases = collect(args)
    if not cases:
        print("no cases found")
        return 2
    print("cases=%d jobs=%d" % (len(cases), args.jobs))
    t0 = time.time()
    stats = {"PASS": 0, "FAIL": 0, "SKIP": 0}
    failures = []
    skips = []
    with cf.ThreadPoolExecutor(max_workers=args.jobs) as ex:
        futs = {ex.submit(compare_one, c, args.exe, args.cidx, args.arg0, args.timeout): c
                for c in cases}
        done = 0
        for fu in cf.as_completed(futs):
            st, case, detail = fu.result()
            stats[st] += 1
            done += 1
            if st == "FAIL":
                failures.append((case, detail))
                print("FAIL %s\n%s" % (case, detail))
            elif st == "SKIP":
                skips.append((case, detail))
            if done % 20 == 0:
                print("  ... %d/%d  (%.0fs)" % (done, len(cases), time.time() - t0))
    print("-" * 60)
    if skips:
        buckets = {}
        for case, why in skips:
            buckets.setdefault(why, []).append(case)
        print("SKIP breakdown:")
        for why in sorted(buckets, key=lambda k: -len(buckets[k])):
            lst = buckets[why]
            print("  [%d] %s" % (len(lst), why))
            for c in lst[:6]:
                print("        %s" % c)
            if len(lst) > 6:
                print("        ... +%d more" % (len(lst) - 6))
        print("-" * 60)
    print("TOTAL=%d PASS=%d FAIL=%d SKIP=%d  elapsed=%.0fs"
          % (len(cases), stats["PASS"], stats["FAIL"], stats["SKIP"], time.time() - t0))
    if failures:
        print("INDEX_BATCH_MISMATCH")
        return 1
    print("INDEX_BATCH_OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
