#!/usr/bin/env python3
"""
tools/link_rt.py — 用 0.5 自研 rt/ 系统调用层链接产物。

用法：
    python tools/link_rt.py <obj.ll> <out.exe>
    将 .ll 汇编为 .o，链接 rt/ 层（纯 C，仅系统库）+ 可选 miniz.o，
    不依赖 0.3 C++ runtime（runtime_for_selfhost.o）。

依赖 LLVM_HOME 环境变量（无内置默认路径）。
"""
import os, sys, subprocess, tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def llvm_bin():
    env = os.environ.get("LLVM_HOME")
    if not env:
        print("LLVM_HOME not set: set LLVM_HOME to your LLVM installation", file=sys.stderr)
        sys.exit(1)
    return os.path.join(env, "bin")


def main():
    if len(sys.argv) < 3:
        print("usage: link_rt.py <input.ll> <out.exe>")
        return 1
    ll_path, exe_path = sys.argv[1], sys.argv[2]
    bin_dir = llvm_bin()
    llc = os.path.join(bin_dir, "llc.exe")
    clang = os.path.join(bin_dir, "clang.exe")
    obj_path = os.path.splitext(exe_path)[0] + ".o"

    r1 = subprocess.run([llc, "-O0", "-filetype=obj", ll_path, "-o", obj_path])
    if r1.returncode != 0:
        print("llc failed")
        return 1

    rt_objs = []
    for f in ("rt_core", "rt_fs", "rt_proc", "rt_time", "rt_err", "rt_extra", "rt_gc", "rt_thread", "rt_zip", "rt_io"):
        p = os.path.join(REPO, "build", "rt", f + ".o")
        if os.path.exists(p):
            rt_objs.append(p)
        else:
            print(f"missing rt obj: {p}")
            return 1

    miniz = os.path.join(REPO, "build", "miniz.o")
    if not os.path.exists(miniz):
        miniz = os.path.join(REPO, "bootstrap", "miniz.o")

    cmd = [clang, "-O0", obj_path] + rt_objs + [miniz, "-o", exe_path,
           "-Wl,/subsystem:console", "-lws2_32"]
    r2 = subprocess.run(cmd)
    if r2.returncode != 0:
        print("link failed")
        return 1
    print(f"OK: {exe_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
