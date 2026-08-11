# -*- coding: utf-8 -*-
"""对照 C 轻量索引器输出 与 编译器 api_check 真实 fmeta/smeta/emeta。

用法：
    python tools/cmp_index.py build/real_meta.txt build/c_fmeta.txt build/c_smeta.txt build/c_emeta.txt

real_meta.txt 由 `shadow <entry> --fmeta` 产出，三段以 ---SMETA--- / ---EMETA--- 分隔。
"""
import sys
import collections


def load_real(path):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        txt = f.read()
    # 去掉可能的 timing/警告前缀行：从第一条形如 name|digits|digits 的行开始
    seg_f, seg_s, seg_e = txt, "", ""
    if "---SMETA---" in txt:
        seg_f, rest = txt.split("---SMETA---", 1)
        if "---EMETA---" in rest:
            seg_s, seg_e = rest.split("---EMETA---", 1)
        else:
            seg_s = rest
    return [clean(seg_f), clean(seg_s), clean(seg_e)]


def clean(seg):
    out = []
    for ln in seg.replace("\r\n", "\n").replace("\r", "\n").split("\n"):
        ln = ln.rstrip()
        if not ln:
            continue
        if "|" not in ln:
            continue
        head = ln.split("|", 1)[0]
        if not head:
            continue
        out.append(ln)
    return out


def load_c(path):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            return clean(f.read())
    except FileNotFoundError:
        return []


def key_of(line):
    p = line.split("|")
    # name + module（末段）唯一标识；module 段可能不存在
    name = p[0]
    mod = p[8] if len(p) > 8 else ""
    return (name, mod)


def report(tag, real, cmine, fields):
    print("=" * 72)
    print("[%s] real=%d  c=%d  delta=%+d" % (tag, len(real), len(cmine), len(cmine) - len(real)))
    rmap = collections.OrderedDict()
    for ln in real:
        rmap.setdefault(key_of(ln), []).append(ln)
    cmap = collections.OrderedDict()
    for ln in cmine:
        cmap.setdefault(key_of(ln), []).append(ln)

    only_real = [k for k in rmap if k not in cmap]
    only_c = [k for k in cmap if k not in rmap]
    print("  only-in-real: %d   only-in-c: %d" % (len(only_real), len(only_c)))
    for k in only_real[:15]:
        print("    R-ONLY %s" % (rmap[k][0][:150],))
    for k in only_c[:15]:
        print("    C-ONLY %s" % (cmap[k][0][:150],))

    # 字段级差异
    diff_by_field = collections.Counter()
    samples = collections.defaultdict(list)
    common = [k for k in rmap if k in cmap]
    for k in common:
        a = rmap[k][0].split("|")
        b = cmap[k][0].split("|")
        n = max(len(a), len(b))
        for i in range(n):
            va = a[i] if i < len(a) else "<none>"
            vb = b[i] if i < len(b) else "<none>"
            if va != vb:
                fn = fields[i] if i < len(fields) else "f%d" % i
                diff_by_field[fn] += 1
                if len(samples[fn]) < 6:
                    samples[fn].append((k[0], va, vb))
    print("  common=%d  field-diffs:" % len(common))
    if not diff_by_field:
        print("    (none) FIELD_MATCH_OK")
    for fn, cnt in diff_by_field.most_common():
        print("    %-10s %5d" % (fn, cnt))
        for nm, va, vb in samples[fn]:
            print("        %-34s real=%-28s c=%s" % (nm[:34], va[:28], vb[:60]))
    return len(only_real) == 0 and len(only_c) == 0 and not diff_by_field


def main():
    if len(sys.argv) < 5:
        print(__doc__)
        return 2
    real_f, real_s, real_e = load_real(sys.argv[1])
    c_f = load_c(sys.argv[2])
    c_s = load_c(sys.argv[3])
    c_e = load_c(sys.argv[4])
    ff = ["name", "line", "col", "end_line", "end_col", "rettype", "sig", "is_pub", "module"]
    sf = ["name", "line", "col", "end_line", "end_col"]
    ok1 = report("FMETA", real_f, c_f, ff)
    ok2 = report("SMETA", real_s, c_s, sf)
    ok3 = report("EMETA", real_e, c_e, sf)
    print("=" * 72)
    print("RESULT: %s" % ("ALL_MATCH" if (ok1 and ok2 and ok3) else "MISMATCH"))
    return 0 if (ok1 and ok2 and ok3) else 1


if __name__ == "__main__":
    sys.exit(main())
