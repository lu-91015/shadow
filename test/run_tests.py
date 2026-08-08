#!/usr/bin/env python3
"""
Shadow 0.4 — test runner for the SELF-HOSTED compiler (build/shadow.exe).

影子 0.4 测试运行器。驱动 build/shadow.exe（0.4 自举编译器），
复用 0.3 的三类断言形态：
  tc        编译通过（rc==0）
  tc-fail   编译干净拒绝（rc!=0 且非崩溃）
  codegen   .ll 非空（rc==0）
  exe*      编译链接运行，stdout 与 //@expect 比对
  check/ranges/diagjson/typeat  查询类 stdout 子串断言

用法：
    python test/run_tests.py             # 全部用例
    python test/run_tests.py -k struct    # 关键字过滤
    python test/run_tests.py --list       # 列出用例
"""

import os, sys, subprocess, argparse, glob, time, uuid
from concurrent.futures import ThreadPoolExecutor, as_completed

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(SCRIPT_DIR)
# 部分 IO 用例（cg_file_io / cg_dir_path_io 等）源码里写死了「相对仓库根」的路径
# （如 "test/_s03_l4_file_io_tmp.txt"）。若从 test/ 目录启动运行器，被测 exe 继承
# cwd=test/ 后会去找 test/test/... → 误报 FAIL。统一锚定仓库根，保证从任何目录
# 启动结果一致。
os.chdir(REPO_ROOT)
SHADOW_EXE = os.environ.get("SHADOW_EXE") or os.path.join(REPO_ROOT, "build", "shadow.exe")
CASES_DIR = os.path.join(SCRIPT_DIR, "cases")

FOLDER_MODE = {
    "tc": "tc",
    "tc-fail": "tc-fail",
    "ast": "ast",
    "codegen": "codegen",
    "edge": "codegen",
    "exe": "exe",
    "exe-tc": "exe",
    "exe-codegen": "exe",
    "exe-edge": "exe",
    "exe-ast": "exe",
    "exe-extra": "exe",
    "exe-sbg": "exe",
    "exe-spk": "exe",
    "exe-spk-native": "exe",
    "exe-sbg-version": "exe",
    "tc-fail-sbg-version": "tc-fail",
    "check": "check",
    "ranges": "ranges",
    "diagjson": "diagjson",
    "typeat": "typeat",
}

QUERY_FLAG = {
    "check": "--check",
    "ranges": "--ranges",
    "diagjson": "--diag-json",
}

CRASH_CODES = {139, 3221225477, -1073741819}


def parse_directives(path):
    d = {"mode": None, "desc": "", "expect": [], "expect_stderr": None,
         "expect_ir": None, "skip": None, "xfail": None, "timeout": 30,
         "pos": None, "env": {}, "args": []}
    with open(path, "r", encoding="utf-8") as f:
        lines = f.readlines()
    for line in lines:
        s = line.strip()
        if not (s.startswith("//@") or s.startswith("#@")):
            if not s.startswith("//") and not s.startswith("#"):
                break
            continue
        body = s[3:].strip()
        if ":" not in body:
            continue
        k, _, v = body.partition(":")
        k = k.strip().lower()
        v = v.strip()
        if k == "mode":
            d["mode"] = v
        elif k == "desc":
            d["desc"] = v
        elif k == "expect":
            d["expect"].append(v)
        elif k == "expect-stderr":
            d["expect_stderr"] = v
        elif k == "expect-ir":
            d["expect_ir"] = v
        elif k == "skip":
            d["skip"] = v
        elif k == "xfail":
            d["xfail"] = v
        elif k == "pos":
            parts = v.split()
            if len(parts) >= 2:
                try:
                    d["pos"] = (int(parts[0]), int(parts[1]))
                except ValueError:
                    pass
        elif k == "env":
            # //@env: K=V K2=V2 —— 注入到子进程环境。
            # 供 GC 用例在压测/自检配置下跑（如 SHADOW_GC_STRESS=8 SHADOW_GC_VERIFY=1）。
            for item in v.split():
                ek, _, ev = item.partition("=")
                if ek:
                    d["env"][ek] = ev
        elif k == "timeout":
            try:
                d["timeout"] = int(v)
            except ValueError:
                pass
        elif k == "args":
            # //@args: --strict --foo  —— 追加到 shadow.exe 命令行（空格分隔）
            for a in v.split():
                if a:
                    d["args"].append(a)
    return d


def discover(root):
    cases = []
    for dirpath, _, filenames in os.walk(root):
        for fn in sorted(filenames):
            if not fn.endswith(".shadow") or fn.startswith("_"):
                continue
            full = os.path.join(dirpath, fn)
            rel = os.path.relpath(full, SCRIPT_DIR)
            mode = FOLDER_MODE.get(os.path.basename(dirpath))
            # 目录名不在 FOLDER_MODE 时，回退读取文件内 `//@mode:` 指令（支持子目录归组，如 strict_access/）
            if mode is None:
                dm = parse_directives(full).get("mode")
                mode = FOLDER_MODE.get(dm) if dm else None
            if mode is None:
                continue
            cases.append((full, rel, mode))
    cases.sort(key=lambda c: c[1])
    return cases


def run_case(case):
    status, rel, msg = run_case_inner(case)
    d = parse_directives(case[0])
    # 瞬时失败自动复跑一次：全量长跑环境下（Defender/杀软扫描高峰、资源竞争）
    # 子进程可能瞬时异常退出（rc!=0 且 stderr 空 / llc.exe error），短时复跑即 PASS。
    # 复跑仍 FAIL 才是稳定失败（真 bug）。不适用于 xfail 用例（其 FAIL 是预期）。
    # ⚠️ 重试前必须归档清空 lu_cache：首跑中途失败可能已写入「部分模块」缓存
    #   （多模块用例部分命中）→ 重试在部分命中状态下编译会稳定失败；
    #   清空后重试 = 干净全量编译 = 与无缓存基线一致。
    if not d.get("xfail") and status == "FAIL":
        lu_dir = os.path.join(REPO_ROOT, "build", "lu_cache")
        if os.path.isdir(lu_dir):
            bak = os.path.join(REPO_ROOT, "build", f"_lu_retry_{int(time.time())}")
            try:
                os.rename(lu_dir, bak)
            except OSError:
                pass
            os.makedirs(lu_dir, exist_ok=True)
        s2, r2, m2 = run_case_inner(case)
        if s2 == "PASS":
            return ("PASS", rel, "retry")
        status, msg = s2, m2
    # xfail：预期失败（已知 bug）。FAIL/CRASH -> PASS；PASS -> FAIL（unexpected pass，应移除标记）
    if d.get("xfail"):
        if status in ("FAIL", "CRASH"):
            return ("PASS", rel, "xfail (known bug)")
        if status == "PASS":
            return ("FAIL", rel, "UNEXPECTED PASS - remove //@xfail")
    return (status, rel, msg)


def run_case_inner(case):
    full, rel, mode = case
    d = parse_directives(full)
    if d.get("skip"):
        return ("SKIP", rel, d["skip"])
    cmd = [SHADOW_EXE, full]
    # UUID 后缀隔离临时产物：并行模式下多个 worker 同时写同名文件会互相覆盖/半写。
    # codegen 的 .ll 检查后即删除；exe 的 .exe/.obj 由编译器按用例名生成（天然唯一）。
    uid = uuid.uuid4().hex[:8]
    if mode == "codegen":
        cmd += ["-o", os.path.join(REPO_ROOT, "build", f"_case_{uid}.ll")]
    elif mode == "exe":
        cmd += ["--run"]
    elif mode == "check":
        cmd += [QUERY_FLAG["check"]]
    elif mode == "ranges":
        cmd += [QUERY_FLAG["ranges"]]
    elif mode == "diagjson":
        cmd += [QUERY_FLAG["diagjson"]]
    elif mode == "typeat" and d.get("pos"):
        cmd += ["--type-at"]
        cmd += [full, str(d["pos"][0]), str(d["pos"][1])]
    if d.get("args"):
        cmd += d["args"]
    child_env = None
    if d.get("env"):
        child_env = dict(os.environ)
        child_env.update(d["env"])
    try:
        p = subprocess.run(cmd, capture_output=True, text=True,
                           timeout=d.get("timeout", 30), env=child_env)
    except subprocess.TimeoutExpired:
        return ("TIMEOUT", rel, "")
    rc = p.returncode
    out = (p.stdout or "")
    err = (p.stderr or "")

    if rc in CRASH_CODES:
        return ("CRASH", rel, err.strip()[:120])

    if mode == "tc":
        ok = (rc == 0)
        return ("PASS" if ok else "FAIL", rel, err.strip()[:120] if not ok else "")
    if mode == "tc-fail":
        ok = (rc != 0)
        # 若用例声明 expect 错误子串，做子串断言（0.3 语义：expect 值按字面 \n 拆分逐部分匹配）
        if ok and d["expect"]:
            joined = out + err
            for exp in d["expect"]:
                for part in exp.split("\\n"):
                    if part and part not in joined:
                        return ("FAIL", rel, f"missing stderr {part!r}")
        return ("PASS" if ok else "FAIL", rel, "" if ok else "expected rejection but rc==0")
    if mode == "codegen":
        if rc != 0:
            return ("FAIL", rel, err.strip()[:120])
        ll_path = os.path.join(REPO_ROOT, "build", f"_case_{uid}.ll")
        if os.path.exists(ll_path) and os.path.getsize(ll_path) > 0:
            if d["expect_ir"]:
                content = open(ll_path, encoding="utf-8", errors="replace").read()
                if d["expect_ir"] not in content:
                    return ("FAIL", rel, f"missing IR {d['expect_ir']!r}")
            return ("PASS", rel, "")
        return ("FAIL", rel, "no .ll produced")
    if mode == "exe":
        if rc != 0:
            return ("FAIL", rel, (err or out).strip()[:120])
        for exp in d["expect"]:
            for part in exp.split("\\n"):
                if part and part not in out:
                    return ("FAIL", rel, f"stdout missing {part!r} in {out!r}")
        if d["expect_stderr"] and d["expect_stderr"] not in err:
            return ("FAIL", rel, f"stderr missing {d['expect_stderr']!r}")
        return ("PASS", rel, "")
    # query modes
    if rc != 0:
        return ("FAIL", rel, (err or out).strip()[:120])
    for exp in d["expect"]:
        for part in exp.split("\\n"):
            if part and part not in out:
                return ("FAIL", rel, f"stdout missing {part!r}")
    return ("PASS", rel, "")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-k", "--keyword", default=None)
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--no-clean-cache", action="store_true",
                    help="不清空 build/lu_cache（默认每次测试前归档清空，避免跨轮缓存积累干扰）")
    ap.add_argument("-j", "--jobs", type=int, default=min(os.cpu_count() or 4, 8),
                    help=f"并行 worker 数（默认 {min(os.cpu_count() or 4, 8)}=CPU 核数上限 8；-j 1 或 --serial 回退串行）。并行时临时产物按 UUID 隔离")
    ap.add_argument("--serial", action="store_true", help="强制串行（等价 -j 1）")
    ap.add_argument("paths", nargs="*")
    args = ap.parse_args()

    if not os.path.exists(SHADOW_EXE):
        print(f"ERROR: {SHADOW_EXE} not found. Run build.bat first.")
        return 1

    # 铁律：每次测试前清空 .lu 缓存（归档而非删除，跨轮不积累）。
    # 背景：全量长跑环境下用例编译会持续写 build/lu_cache（main_mod_in_src 设计为
    # 用户代码同样缓存），跨轮积累后可能命中旧 MIR（泛型特化缺失等）或触发
    # Defender 扫描高峰 → 瞬时失败。每轮干净起点 + run_case 自动重试 = 稳定验收。
    if not args.list and not args.no_clean_cache:
        lu_dir = os.path.join(REPO_ROOT, "build", "lu_cache")
        if os.path.isdir(lu_dir):
            import time as _t
            bak = os.path.join(REPO_ROOT, "build", f"_lu_bak_{int(_t.time())}")
            try:
                os.rename(lu_dir, bak)
                print(f"  [cache] archived build/lu_cache -> {os.path.basename(bak)}")
            except OSError:
                pass
            os.makedirs(lu_dir, exist_ok=True)

    cases = discover(CASES_DIR)
    if args.paths:
        cases = [c for c in cases if any(p in c[1] for p in args.paths)]
    if args.keyword:
        cases = [c for c in cases if args.keyword in c[1]]
    if args.list:
        for _, rel, mode in cases:
            print(f"{mode:12s} {rel}")
        return 0

    total = passed = failed = crashed = skipped = 0
    jobs = args.jobs if not args.serial else 1
    if jobs <= 1:
        results = [(c, run_case(c)) for c in cases]
    else:
        # 并行模式（0.3 run_tests_03.py 同款）：结果按原始顺序存储，完成后顺序打印。
        # 临时产物已按 UUID 隔离；lu_cache 并发写不同键文件无冲突；偶发竞态由 run_case
        # 自动重试兜底（0.3 注释明说：并行时 .sig/.spk 缓存竞态导致偶发失败 → 重试）。
        results = [None] * len(cases)
        with ThreadPoolExecutor(max_workers=jobs) as pool:
            futs = {}
            for idx, c in enumerate(cases):
                futs[pool.submit(run_case, c)] = idx
            for fut in as_completed(futs):
                idx = futs[fut]
                try:
                    results[idx] = (cases[idx], fut.result())
                except Exception as e:  # 用例级异常不应中断整轮
                    results[idx] = (cases[idx], ("FAIL", cases[idx][1], f"runner error: {e}"))
    for c, (status, rel, msg) in results:
        total += 1
        if status == "PASS":
            passed += 1
            print(f"  [PASS] {rel}")
        elif status == "SKIP":
            skipped += 1
            print(f"  [SKIP] {rel} ({msg})")
        elif status == "CRASH":
            crashed += 1
            print(f"  [CRASH] {rel} {msg}")
        else:
            failed += 1
            print(f"  [FAIL] {rel} {msg}")

    print("=" * 60)
    print(f"  PASS={passed} FAIL={failed} CRASH={crashed} SKIP={skipped} TOTAL={total}")
    return 1 if (failed or crashed) else 0


if __name__ == "__main__":
    sys.exit(main())
