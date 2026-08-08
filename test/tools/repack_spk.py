#!/usr/bin/env python3
"""重新打包测试夹具 .spk（ZIP）。

背景
----
仓库里 `core.autocrlf=true` 且长期缺少 .gitattributes，git 把二进制 .spk
当作文本做了 LF<->CRLF 转换，把 deflate 流打坏了（Python zlib 报
"invalid stored block lengths"，miniz 侧表现为 shadow_zip_unpack 解出 0 字节
文件并返回 -1，最终冒充成 "module error: ... not found in shadow.sbg [deps]"）。
shadow-0.3 的原件同样已损坏，所以 exe-spk / exe-spk-native 两例在 0.3 也跑不通。

每个用例目录都保留了 `_spk_src/`（打包前的原始素材，逐字节与包内条目一致），
本脚本据此确定性地重建 .spk。配合仓库根的 .gitattributes（`*.spk binary`）
即可根治。

用法：python test/tools/repack_spk.py [--check]
  --check  只校验现有 .spk 是否可解压，不重写
"""
import os
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))  # shadow-0.4/

# (spk 输出路径, 素材目录) —— 素材目录内的相对路径即 zip 内路径
TARGETS = [
    ("test/cases/migrated_03/exe-spk/mylib.spk",
     "test/cases/migrated_03/exe-spk/_spk_src"),
    ("test/cases/migrated_03/exe-spk-native/native_lib.spk",
     "test/cases/migrated_03/exe-spk-native/_spk_src"),
]

# 固定时间戳，保证重复打包字节一致（便于 diff / 缓存内容戳稳定）
FIXED_DATE = (1980, 1, 1, 0, 0, 0)


def collect(src_dir):
    """返回 [(zip内路径, 磁盘绝对路径)]，按 zip 内路径升序（确定性）。"""
    out = []
    for dirpath, _dirnames, filenames in os.walk(src_dir):
        for fn in filenames:
            full = os.path.join(dirpath, fn)
            rel = os.path.relpath(full, src_dir).replace("\\", "/")
            out.append((rel, full))
    out.sort(key=lambda t: t[0])
    return out


def check_one(spk):
    try:
        z = zipfile.ZipFile(spk)
        names = z.namelist()
        for n in names:
            z.read(n)  # 真正解压，才能发现坏 deflate 流
        return True, names
    except Exception as e:  # noqa: BLE001 - 任何解压异常都算坏包
        return False, str(e)


def main():
    check_only = "--check" in sys.argv
    bad = 0
    for spk_rel, src_rel in TARGETS:
        spk = os.path.join(ROOT, spk_rel)
        src = os.path.join(ROOT, src_rel)
        ok, info = check_one(spk) if os.path.exists(spk) else (False, "missing")
        if check_only:
            print(("OK   " if ok else "BAD  ") + spk_rel + "  " + str(info))
            if not ok:
                bad += 1
            continue
        if ok:
            print("SKIP " + spk_rel + " (已可正常解压)")
            continue
        if not os.path.isdir(src):
            print("FAIL " + spk_rel + " 缺少素材目录 " + src_rel)
            bad += 1
            continue
        entries = collect(src)
        with zipfile.ZipFile(spk, "w", zipfile.ZIP_DEFLATED) as z:
            for rel, full in entries:
                zi = zipfile.ZipInfo(rel, date_time=FIXED_DATE)
                zi.compress_type = zipfile.ZIP_DEFLATED
                zi.external_attr = 0o644 << 16
                with open(full, "rb") as f:
                    z.writestr(zi, f.read())
        ok2, info2 = check_one(spk)
        print(("REPACK " if ok2 else "FAIL   ") + spk_rel + "  " + str(info2))
        if not ok2:
            bad += 1
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
