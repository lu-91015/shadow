import subprocess, time, statistics, sys, os

WS = r"D:\shadow\shadow-0.5"

# 全量基准表（shadow 名 -> shadow exe -> go exe）
ALL = {
    "nbody":         ("bench/shadow/nbody.shadow.exe",         "bench/go/nbody.exe"),
    "spectral_norm": ("bench/shadow/spectral_norm.shadow.exe", "bench/go/spectral_norm.exe"),
    "fannkuch":      ("bench/shadow/fannkuch.shadow.exe",      "bench/go/fannkuch.exe"),
    "binary_tree":   ("bench/shadow/binary_tree.shadow.exe",   "bench/go/binary_tree.exe"),
    "merge_sort":    ("bench/shadow/merge_sort.shadow.exe",    "bench/go/merge_sort.exe"),
    "quicksort":     ("bench/shadow/quicksort.shadow.exe",     "bench/go/quicksort.exe"),
    "fib":           ("bench/shadow/fib.shadow.exe",           "bench/go/fib.exe"),
    "prime_sieve":   ("bench/shadow/prime_sieve.shadow.exe",   "bench/go/prime_sieve.exe"),
    "matmul":        ("bench/shadow/matmul.shadow.exe",        "bench/go/matmul.exe"),
    "sum_loop":      ("bench/shadow/sum_loop.shadow.exe",      "bench/go/sum_loop.exe"),
    "str_concat":    ("bench/shadow/str_concat.shadow.exe",    "bench/go/str_concat.exe"),
    "str_reverse":   ("bench/shadow/str_reverse.shadow.exe",   "bench/go/str_reverse.exe"),
    "string_find":   ("bench/shadow/string_find.shadow.exe",   "bench/go/string_find.exe"),
    "array_push":    ("bench/shadow/array_push.shadow.exe",    "bench/go/array_push.exe"),
    "ackermann":     ("bench/shadow/ackermann.shadow.exe",     "bench/go/ackermann.exe"),
    "float_pi":      ("bench/shadow/float_pi.shadow.exe",      "bench/go/float_pi.exe"),
}

# 命令行第一个参数若为 --shadow-suffix=XXX 可指定 shadow exe 后缀（默认 .shadow.exe）
# 其余参数为基准名；不传基准名则跑全量
suffix = ".shadow.exe"
pos = []
for a in sys.argv[1:]:
    if a.startswith("--shadow-suffix="):
        suffix = a.split("=", 1)[1]
    else:
        pos.append(a)

if len(pos) > 0:
    progs = [(k, f"bench/shadow/{k}{suffix}", ALL[k][1]) for k in pos if k in ALL]
else:
    progs = [(k, f"bench/shadow/{k}{suffix}", v[1]) for k, v in ALL.items()]


def time_it(exe, n=5):
    ts = []
    for _ in range(n):
        t0 = time.perf_counter()
        subprocess.run(exe, cwd=WS, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        t1 = time.perf_counter()
        ts.append((t1 - t0) * 1000.0)
    ts.sort()
    return statistics.median(ts)

print(f"{'benchmark':<14} {'shadow(ms)':>10} {'go(ms)':>10} {'shadow/go':>10}")
print("-" * 50)
for name, s_exe, g_exe in progs:
    s = time_it(os.path.join(WS, s_exe))
    g = time_it(os.path.join(WS, g_exe))
    ratio = s / g if g > 0 else float('inf')
    print(f"{name:<14} {s:>10.1f} {g:>10.1f} {ratio:>10.2f}")
