"""跨行字符串字面量检查（CRLF 陷阱防回归）。

背景
----
Shadow 词法器把字符串字面量里的**裸换行原样**吃进常量。于是这样写：

    let lines = str_split(src, "
    ");

在 LF 源文件里得到分隔符 "\n"，在 **CRLF 源文件里得到 "\r\n"** —— 同一份源码，
换行风格不同，语义就不同。

2026-08-06 的真实事故：`src/main.shadow`（CRLF）里 `main_sbg_version_of` 用这种写法
切分 shadow.sbg。被解析的 sbg 是 LF 结尾，于是 str_split 只切出 1 行，整个文件被当成
`[package]` 段头，version 永远解析为空 -> SPK 依赖全部退化到 `<mod>@0.0.0` 目录。
IR 里 `@.str.18 = c"\\0D\\0A\\00"` 是唯一的直接证据，排查代价极高。

正确写法：用转义 `"\\n"`。配合 `str_trim`（会剥离 \\r）即可 LF/CRLF 双兼容。

用法
----
    python tools/lint_multiline_str.py [目录...]      # 默认 src

退出码：0 = 干净；1 = 发现 CRLF 文件内的跨行字面量（危险）。
"""
import os
import sys

QUOTE = '"'
SKIP_DIRS = {"build", ".git", "lu_cache", "node_modules"}


def ends_inside_string(line: str) -> bool:
    """这一行结束时是否仍处在未闭合的字符串里（已跳过行注释与转义）。"""
    i = 0
    in_str = False
    n = len(line)
    while i < n:
        c = line[i]
        if in_str:
            if c == "\\":
                i += 2
                continue
            if c == QUOTE:
                in_str = False
            i += 1
            continue
        if c == QUOTE:
            in_str = True
            i += 1
            continue
        if c == "/" and i + 1 < n and line[i + 1] == "/":
            break
        i += 1
    return in_str


def scan(roots):
    files = 0
    danger = []
    warn = []
    for root_dir in roots:
        for root, dirs, names in os.walk(root_dir):
            dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
            for name in names:
                if not name.endswith(".shadow"):
                    continue
                path = os.path.join(root, name)
                files += 1
                raw = open(path, "rb").read()
                is_crlf = b"\r\n" in raw
                for idx, line in enumerate(
                        raw.decode("utf-8", errors="replace").split("\n")):
                    text = line.rstrip("\r")
                    if ends_inside_string(text):
                        rec = (path, idx + 1, text.strip()[:80])
                        (danger if is_crlf else warn).append(rec)
    return files, danger, warn


def main(argv):
    roots = argv[1:] or ["src"]
    files, danger, warn = scan(roots)
    print("扫描 %d 个 .shadow 文件（%s）" % (files, ", ".join(roots)))

    for path, ln, text in warn:
        print("  [warn] LF 文件内跨行字面量  %s:%d  %s" % (path, ln, text))
    for path, ln, text in danger:
        print("  [FAIL] CRLF 文件内跨行字面量  %s:%d  %s" % (path, ln, text))

    if danger:
        print("\n%d 处危险跨行字符串字面量 —— 改用转义 \"\\n\"" % len(danger))
        return 1
    if warn:
        print("\n%d 处跨行字面量（当前 LF，尚安全，仍建议改成转义）" % len(warn))
        return 0
    print("干净：未发现跨行字符串字面量")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
