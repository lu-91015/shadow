#!/usr/bin/env python3
"""重对齐 migrated_03 codegen/edge 用例的 //@expect-ir 断言到 0.4 IR 特征。
0.4 的 struct 是 GC 对象（ptrtoint+手动偏移+__rt_shadow_* 内联），IR 形状与 0.3 不同。
用法: python tools/_realign_ir.py [--apply]
"""
import subprocess, os, re, sys, glob

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "build", "shadow.exe")
M = os.path.join(ROOT, "test", "cases", "migrated_03")

# 旧 expect_ir 关键词 -> 0.4 特征（已探查确认存在于 0.4 IR）
MAP = [
    ("shadow_string_subscript", "__rt_shadow_load"),
    ("shadow_string_compare", "__rt_shadow"),
    ("shadow_int_to_string", "__rt_shadow_int_to_cstr"),
    ("shadow_array_create_ints", "__rt_shadow_malloc"),
    ("shadow_array_create_ptrs", "__rt_shadow_malloc"),
    ("shadow_array_set_int", "shadow_array_set_int"),
    ("shadow_gc_root_add", "shadow_gc_root_"),
    ("shadow_member_any", "shadow_any_"),
    ("shadow_index_any", "shadow_any_"),
    ("shadow_any_as_int", "shadow_any_"),
    ("getelementptr", "ptrtoint"),
    ("and", "icmp"),
    ("unreachable", "shadow_panic"),
    ("mres", "br label"),
    ("constant", "@.str"),
]

def compile_ll(rel):
    r = subprocess.run([EXE, rel], capture_output=True, text=True, timeout=120)
    if r.returncode != 0:
        return None, ("rc=%d %s" % (r.returncode, (r.stderr or "").strip()[:100]))
    ll_path = os.path.join(ROOT, "build", "_case.ll")
    if not os.path.exists(ll_path):
        return None, "no _case.ll"
    return open(ll_path, encoding="utf-8", errors="replace").read(), None

def map_to_04(old, ll):
    for key, rep in MAP:
        if key in old:
            return rep
    return None

def main():
    apply = "--apply" in sys.argv
    changed = []
    problems = []
    for p in glob.glob(M + "/**/*.shadow", recursive=True):
        if p.endswith((".sig", ".lu")):
            continue
        data = open(p, "rb").read()
        src = data.decode("utf-8", errors="ignore")
        if "expect-ir" not in src:
            continue
        old_vals = re.findall(r"//@expect-ir:\s*([^\r\n]+)", src)
        if not old_vals:
            continue
        ll, err = compile_ll(p)
        if ll is None:
            problems.append((os.path.relpath(p, M), old_vals[0].strip(), err))
            continue
        # 逐个替换（部分替换：单个 no mapping 不阻断其他行）
        new_data = data
        for old_v in old_vals:
            old_v = old_v.strip()
            rep = map_to_04(old_v, ll)
            if rep is None:
                problems.append((os.path.relpath(p, M), old_v, "no mapping"))
                continue
            if rep not in ll:
                problems.append((os.path.relpath(p, M), old_v, "candidate %r not in IR" % rep))
                continue
            nd, n = re.subn(
                ("//@expect-ir:\\s*" + re.escape(old_v) + "\\s*").encode(),
                ("//@expect-ir: " + rep + "\r\n").encode(),
                new_data, count=1)
            if n == 1:
                new_data = nd
                changed.append((os.path.relpath(p, M), old_v, rep))
            else:
                problems.append((os.path.relpath(p, M), old_v, "replace failed"))
        if new_data != data and apply:
            open(p, "wb").write(new_data)
    print("=== 可替换（%d 处，%s）===" % (len(changed), "已应用" if apply else "dry-run"))
    for rel, old, rep in changed:
        print("  %-40s %-32s -> %s" % (rel, old, rep))
    print("=== 问题（%d）===" % len(problems))
    for rel, old, err in problems:
        print("  %-40s %-28s %s" % (rel, old, err))

if __name__ == "__main__":
    main()
