#!/usr/bin/env python3
"""
Shadow 0.4 — fixed-point 自举校验工具（原则 2 / §4.1）

用法：
    python verify_fixed_point.py <stage2.ll> <stage3.ll>

校验规则：stage2.ll 与 stage3.ll 必须逐字节相等。
含义：stage2（由 stage1 编译的编译器）与 stage3（由 stage2 编译的编译器）
编译同一份源码产生完全相同的 LLVM IR —— 编译器达到固定点，即自举成功。

退出码：0 = PASS，1 = FAIL，2 = 用法错误
"""

import sys


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: python verify_fixed_point.py <stage2.ll> <stage3.ll>")
        return 2

    p2, p3 = sys.argv[1], sys.argv[2]
    try:
        with open(p2, "rb") as f:
            b2 = f.read()
        with open(p3, "rb") as f:
            b3 = f.read()
    except OSError as e:
        print(f"ERROR: cannot read file: {e}")
        return 2

    if b2 == b3:
        print(f"PASS: fixed-point reached ({len(b2)} bytes identical)")
        return 0

    print(f"FAIL: fixed-point NOT reached")
    print(f"  stage2.ll: {len(b2)} bytes")
    print(f"  stage3.ll: {len(b3)} bytes")
    # 定位首个差异位置
    n = min(len(b2), len(b3))
    for i in range(n):
        if b2[i] != b3[i]:
            print(f"  first diff at byte offset {i}")
            lo = max(0, i - 32)
            hi = min(n, i + 32)
            print(f"  stage2: {b2[lo:hi]!r}")
            print(f"  stage3: {b3[lo:hi]!r}")
            break
    else:
        if len(b2) != len(b3):
            print(f"  one is a prefix of the other (lengths differ)")
    return 1


if __name__ == "__main__":
    sys.exit(main())
