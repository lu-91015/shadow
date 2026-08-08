#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""增量构建缓存：src 树 hash → stage1.ll 复用。
未变更源码时秒出（跳过 s03 6 分钟）；变更时跑 s03 并缓存新产物。
用法: python tools/cached_build.py
"""
import hashlib
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "src")
BUILD = os.path.join(ROOT, "build")
CACHE = os.path.join(BUILD, ".bootstrap-cache")


def src_hash():
    h = hashlib.md5()
    for root, _, files in os.walk(SRC):
        for f in sorted(files):
            if not (f.endswith(".shadow") or f.endswith(".py")):
                continue
            p = os.path.join(root, f)
            h.update(f.encode("utf-8"))
            with open(p, "rb") as fh:
                h.update(fh.read())
    return h.hexdigest()


def main():
    h = src_hash()
    stage1 = os.path.join(BUILD, "stage1.ll")
    cached = os.path.join(CACHE, h, "stage1.ll")
    if os.path.exists(cached):
        shutil.copy(cached, stage1)
        print("[cached] stage1.ll (src hash %s)" % h[:8])
        return 0
    s03 = os.path.join(ROOT, "bootstrap", "s03.exe")
    os.makedirs(os.path.dirname(cached), exist_ok=True)
    r = subprocess.run(
        [s03, os.path.join(SRC, "main.shadow"), "-o", stage1],
        cwd=ROOT, timeout=600,
    )
    if r.returncode != 0:
        return r.returncode
    shutil.copy(stage1, cached)
    print("[built] stage1.ll cached under %s" % h[:8])
    return 0


if __name__ == "__main__":
    sys.exit(main())
