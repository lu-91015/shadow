#!/usr/bin/env python3
"""Java import 规则验证：针对重建后的 build/shadow.exe 跑全套场景。
用法：python tools/_verify_java_imports.py [EXE_PATH]
"""
import os, sys, subprocess, shutil, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "shadow.exe")

def w(d, name, content):
    p = os.path.join(d, name)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "w", encoding="utf-8") as f:
        f.write(content)
    return p

def run(exe, case, args, cwd):
    r = subprocess.run([exe, case] + args, cwd=cwd, capture_output=True, text=True, timeout=180)
    out = (r.stdout or "") + (r.stderr or "")
    return r.returncode, out.strip().replace("\n", " | ")[:300]

def main():
    if not os.path.exists(EXE):
        print("EXE NOT FOUND:", EXE); sys.exit(2)
    tmp = tempfile.mkdtemp(prefix="javimp_")
    print("workdir:", tmp, "exe:", EXE)
    results = []
    def check(tag, expect_rc, case, args):
        rc, out = run(EXE, case, args, tmp)
        ok = (rc == expect_rc)
        results.append((ok, tag, expect_rc, rc, out))
        print(("OK  " if ok else "FAIL") + f" {tag} expect_rc={expect_rc} rc={rc} {out}")

    # ---- A: 路径模型 + 同名消歧 + FQN ----
    w(tmp, "a/b/c.shadow", "dsb c;\npub kimo d() -> int { return 1; }\n")
    w(tmp, "a/b/d.shadow", "dsb d;\npub kimo d() -> int { return 2; }\n")
    w(tmp, "m_ambig.shadow",
       "dsb m_ambig;\nimport a.b.c;\nimport a.b.d;\nkimo main() -> int { return d(); }\n")
    w(tmp, "m_fqn.shadow",
       "dsb m_fqn;\nimport a.b.c;\nimport a.b.d;\nkimo main() -> int { return a.b.c.d(); }\n")
    w(tmp, "m_single.shadow",
       "dsb m_single;\nimport a.b.c;\nkimo main() -> int { return d(); }\n")
    check("A1 路径 a.b.c -> a/b/c.shadow (single import, d() OK)", 0, "m_single.shadow", ["--strict"])
    check("A2 FQN a.b.c.d() 消歧 OK", 0, "m_fqn.shadow", ["--strict"])
    check("A3 两个同名 d 写简名 -> ambiguous 报错", 1, "m_ambig.shadow".replace(".shadow",".shadow"), ["--strict"])  # placeholder
    # 修正 A3：用真实文件
    # (上面占位会被下面覆盖)
    # ---- B: 循环 import ----
    w(tmp, "cyc_a.shadow", "dsb cyc_a;\nimport cyc_b;\npub kimo a_f() -> int { return 1; }\n")
    w(tmp, "cyc_b.shadow", "dsb cyc_b;\nimport cyc_a;\npub kimo b_f() -> int { return 2; }\n")
    w(tmp, "cyc_main.shadow", "dsb cyc_main;\nimport cyc_a;\nkimo main() -> int { return a_f(); }\n")
    check("B1 循环 import a<->b 严格态允许", 0, "cyc_main.shadow", ["--strict"])
    # ---- C: 同目录 package-private ----
    w(tmp, "pkg/f1.shadow", "dsb f1;\nkimo pkg_priv() -> int { return 7; }\npub kimo f1_pub() -> int { return pkg_priv(); }\n")
    w(tmp, "pkg/f2.shadow", "dsb f2;\npub kimo f2_use() -> int { return pkg_priv(); }\n")
    w(tmp, "pkg_main.shadow", "dsb pkg_main;\nimport pkg.f1;\nimport pkg.f2;\nkimo main() -> int { return f2_use(); }\n")
    check("C1 同目录私有符号跨文件可见 (package-private)", 0, "pkg_main.shadow", ["--strict"])
    # ---- D: 通配符不递归 ----
    w(tmp, "a/b/x.shadow", "dsb x;\npub kimo xf() -> int { return 1; }\n")
    w(tmp, "a/b/c/y.shadow", "dsb y;\npub kimo yf() -> int { return 2; }\n")
    w(tmp, "m_wc.shadow", "dsb m_wc;\nimport a.b.*;\nkimo main() -> int { return xf(); }\n")
    w(tmp, "m_wc2.shadow", "dsb m_wc2;\nimport a.b.*;\nkimo main() -> int { return yf(); }\n")
    check("D1 import a.b.* 命中直接子 a.b.x (xf)", 0, "m_wc.shadow", ["--strict"])
    check("D2 import a.b.* 不递归 a.b.c.y (yf 应未定义)", 1, "m_wc2.shadow", ["--strict"])
    # ---- E: dsb 强制 == 文件名 ----
    w(tmp, "bad_dsb.shadow", "dsb wrongname;\nkimo main() -> int { return 0; }\n")
    check("E1 dsb 名 != 文件名 严格态报错", 1, "bad_dsb.shadow", ["--strict"])
    w(tmp, "good_dsb.shadow", "dsb good_dsb;\nkimo main() -> int { return 0; }\n")
    check("E2 dsb 名 == 文件名 严格态通过", 0, "good_dsb.shadow", ["--strict"])
    # ---- F: 已有迁移用例 tc_import_nested ----
    migrated = os.path.join(ROOT, "test", "cases", "migrated_03")
    tcn = os.path.join(migrated, "tc", "tc_import_nested.shadow")
    if os.path.exists(tcn):
        rc, out = run(EXE, tcn, ["--strict"], os.path.join(migrated, "tc"))
        ok = (rc == 0)
        results.append((ok, "F1 tc_import_nested (strict, 嵌套目录)", 0, rc, out))
        print(("OK  " if ok else "FAIL") + f" F1 tc_import_nested expect_rc=0 rc={rc} {out}")

    # 汇总
    nf = sum(1 for r in results if not r[0])
    print(f"\n=== {len(results)-nf}/{len(results)} PASS, {nf} FAIL ===")
    shutil.rmtree(tmp, ignore_errors=True)
    sys.exit(1 if nf else 0)

if __name__ == "__main__":
    main()
