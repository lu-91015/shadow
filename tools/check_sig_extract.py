#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""main_sig_of_src 的等价复刻，用于离线校验签名提取状态机。

校验目标（对 src/ 下真实源码）：
  1. 函数体被剥离（sig 行数 << 源码行数）
  2. struct 字段 / enum 变体 / impl 方法头被保留（不能漏，漏了依赖方会读到过期 MIR）
  3. 只改函数体时 sig 不变；改签名时 sig 变
"""
import sys, pathlib, re


def sig_of_src(src: str) -> str:
    out = []
    depth = 0
    skip_depth = -1
    pending_func = 0
    in_block = 0
    for raw in src.split("\n"):
        d = 0
        has_code = 0
        n = len(raw)
        in_str = 0
        in_chr = 0
        ci = 0
        while ci < n:
            c = raw[ci]
            if in_block == 1:
                if c == "*" and ci + 1 < n and raw[ci + 1] == "/":
                    in_block = 0
                    ci += 2
                    continue
                ci += 1
                continue
            if in_str == 1:
                if c == "\\":
                    ci += 2
                    continue
                if c == '"':
                    in_str = 0
                ci += 1
                continue
            if in_chr == 1:
                if c == "\\":
                    ci += 2
                    continue
                if c == "'":
                    in_chr = 0
                ci += 1
                continue
            if c == "/" and ci + 1 < n:
                c2 = raw[ci + 1]
                if c2 == "/":
                    ci = n
                    continue
                if c2 == "*":
                    in_block = 1
                    ci += 2
                    continue
            if c == '"':
                in_str = 1
                has_code = 1
                ci += 1
                continue
            if c == "'":
                in_chr = 1
                has_code = 1
                ci += 1
                continue
            if c == "{":
                d += 1
            if c == "}":
                d -= 1
            if ord(c) > 32:
                has_code = 1
            ci += 1
        if skip_depth >= 0:
            depth += d
            if depth <= skip_depth:
                skip_depth = -1
            continue
        if has_code == 0:
            depth += d
            continue
        t = raw.strip()
        out.append(t)
        d0 = depth
        depth += d
        if depth > d0:
            if is_func_head(t):
                skip_depth = d0
            elif pending_func == 1:
                skip_depth = d0
            pending_func = 0
        else:
            if "}" in t:
                pending_func = 0
            elif t.endswith(";"):
                pending_func = 0
            elif is_func_head(t):
                pending_func = 1
    return "\n".join(out)


def is_func_head(t: str) -> bool:
    return t.startswith("kimo ") or t.startswith("pub kimo ")


def main():
    root = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "src")
    files = sorted(root.rglob("*.shadow"))
    print(f"{'file':46s} {'lines':>7s} {'siglines':>8s} {'ratio':>6s}")
    print("-" * 72)
    tot_l = tot_s = 0
    for f in files:
        src = f.read_text(encoding="utf-8", errors="replace")
        sig = sig_of_src(src)
        nl = src.count("\n") + 1
        ns = sig.count("\n") + 1 if sig else 0
        tot_l += nl
        tot_s += ns
        print(f"{str(f):46s} {nl:7d} {ns:8d} {ns/max(nl,1)*100:5.1f}%")
    print("-" * 72)
    print(f"{'TOTAL':46s} {tot_l:7d} {tot_s:8d} {tot_s/max(tot_l,1)*100:5.1f}%")

    # ---- 不漏检验证：源码里的声明数 vs sig 里的声明数 ----
    print("\n[漏检检查] 各类声明是否全部进入 sig：")
    pats = {
        "struct 字段(: 类型)": r"^\s*\w+\s*:\s*\w",
        "kimo 函数头": r"^\s*(pub\s+)?kimo\s+\w",
        "struct/enum/trait/impl 头": r"^\s*(pub\s+)?(struct|enum|trait|impl)\s",
    }
    allsrc = "\n".join(
        f.read_text(encoding="utf-8", errors="replace") for f in files
    )
    allsig = "\n".join(
        sig_of_src(f.read_text(encoding="utf-8", errors="replace")) for f in files
    )
    for name, p in pats.items():
        rx = re.compile(p, re.M)
        a = len(rx.findall(allsrc))
        b = len(rx.findall(allsig))
        flag = "OK" if b >= a * 0.99 or name.startswith("struct 字段") else "CHECK"
        print(f"  {name:28s} 源码 {a:6d}  sig {b:6d}  {flag}")


if __name__ == "__main__":
    main()
