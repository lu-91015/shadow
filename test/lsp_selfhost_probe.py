#!/usr/bin/env python3
"""
Shadow 0.5 — LSP 自举源码探针。

把 0.5 编译器源码自身喂给 LSP（`build/shadow.exe --lsp`），
验证 LSP 能加载整棵 import 图并产生（应接近干净的）诊断。

用法：
    python test/lsp_selfhost_probe.py [目标文件，默认 src/main.shadow] [--def line:col]

--def line:col  额外发一次 textDocument/definition 请求（1-based 行:列），
                打印返回的 Location（验证跨模块导航坐标是否指向正确模块文件）。
"""
import os, sys, json, subprocess

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(REPO, "build", "shadow.exe")

# 解析 [目标文件] 与 [--def line:col] 参数
DEF_POS = None
ARGS = sys.argv[1:]
FILES = []
i = 0
while i < len(ARGS):
    a = ARGS[i]
    if a == "--def":
        if i + 1 < len(ARGS):
            DEF_POS = ARGS[i + 1]
            i = i + 2
            continue
        i = i + 1
        continue
    if a.startswith("--def="):
        DEF_POS = a[len("--def="):]
    elif a.startswith("--"):
        pass
    else:
        FILES.append(a)
    i = i + 1
TARGET = os.path.join(REPO, FILES[0]) if FILES else os.path.join(REPO, "src", "main.shadow")


def win_uri(path):
    # Git Bash 下 abspath 形如 /c/Users/...；转成 Windows 原生 c:/Users/...
    ap = os.path.abspath(path)
    if ap.startswith("/") and len(ap) > 2 and ap[1] == ":" and ap[2] == "/":
        ap = ap[1:]  # /c:/... -> c:/...
    elif ap.startswith("/") and ap[2] == "/":
        # /c/Users/... -> c:/Users/...
        ap = ap[1] + ":" + ap[2:]
    return "file:///" + ap.replace("\\", "/")


def frame(obj):
    body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
    return b"Content-Length: " + str(len(body)).encode() + b"\r\n\r\n" + body


def parse_frames(data):
    frames = []
    buf = data
    while True:
        idx = buf.find(b"\r\n\r\n")
        if idx < 0:
            break
        head = buf[:idx].decode("utf-8", "replace")
        clen = 0
        for line in head.split("\r\n"):
            if line.lower().startswith("content-length:"):
                clen = int(line.split(":", 1)[1].strip())
        body = buf[idx + 4: idx + 4 + clen]
        try:
            frames.append(json.loads(body.decode("utf-8", "replace")))
        except Exception:
            frames.append(None)
        buf = buf[idx + 4 + clen:]
    return frames


def main():
    uri = win_uri(TARGET)
    with open(TARGET, "r", encoding="utf-8") as f:
        text = f.read()
    print(f"[probe] target = {TARGET}")
    print(f"[probe] uri    = {uri}")
    print(f"[probe] bytes  = {len(text)}")
    reqs = [
        {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"capabilities": {}}},
        {"jsonrpc": "2.0", "method": "initialized", "params": {}},
        {"jsonrpc": "2.0", "method": "textDocument/didOpen",
         "params": {"textDocument": {"uri": uri, "languageId": "shadow", "version": 1, "text": text}}},
        # 拉一次诊断（pull 模式），也等待 didOpen 的 push
        {"jsonrpc": "2.0", "id": 2, "method": "textDocument/diagnostic",
         "params": {"textDocument": {"uri": uri}}},
    ]
    # 可选：跨模块 definition 验证（--def line:col，1-based）
    if DEF_POS:
        try:
            dl, dc = [int(x) for x in DEF_POS.split(":")]
        except Exception:
            print(f"[probe] bad --def {DEF_POS} (want line:col)")
            sys.exit(2)
        reqs.append({"jsonrpc": "2.0", "id": 10, "method": "textDocument/definition",
                     "params": {"textDocument": {"uri": uri},
                                "position": {"line": dl - 1, "character": dc - 1}}})
    reqs += [
        {"jsonrpc": "2.0", "id": 3, "method": "shutdown", "params": None},
        {"jsonrpc": "2.0", "method": "exit", "params": None},
    ]
    script = b"".join(frame(m) for m in reqs)
    print(f"[probe] launching {EXE} --lsp ...", flush=True)
    p = subprocess.run([EXE, "--lsp"], input=script, capture_output=True, timeout=1200)
    out = p.stdout
    print(f"[probe] exit={p.returncode} stdout_bytes={len(out)}", flush=True)
    if p.stderr:
        sd = p.stderr.decode("utf-8", "replace")
        # 只打印前若干 DBG 行，便于看崩溃/异常
        print("[probe] stderr (first 40 lines):")
        for ln in sd.splitlines()[:40]:
            print("   ", ln)
    frames = parse_frames(out)
    diags = [f for f in frames if f and f.get("method") == "textDocument/publishDiagnostics"]
    pulled = None
    for f in frames:
        if f and f.get("id") == 2:
            pulled = f.get("result", {})
    # 跨模块 definition 结果（id=10）—— result 可能是单个 Location dict 或 Location[] 数组
    if DEF_POS:
        for f in frames:
            if f and f.get("id") == 10:
                res = f.get("result")
                loc = None
                if isinstance(res, dict):
                    loc = res
                elif isinstance(res, list) and res:
                    loc = res[0]
                if loc:
                    u = loc.get("uri", "")
                    rng = loc.get("range", {}).get("start", {})
                    print(f"[probe] definition -> uri={u}")
                    print(f"[probe] definition -> L{rng.get('line', '?')}:{rng.get('character', '?')}")
                    base = os.path.basename(TARGET)
                    ok = base not in u  # 跨模块定义应指向其他模块文件
                    print(f"[probe] definition cross-module = {ok} (target={base})")
                else:
                    print(f"[probe] definition -> (no result: {res!r})")
                break
        else:
            print("[probe] definition -> (no id=10 frame)")
    # 逐帧打印每个 publishDiagnostics 的错误数，定位是首编还是次编出错
    print(f"[probe] publishDiagnostics frames = {len(diags)}")
    for di, df in enumerate(diags):
        ditems = df.get("params", {}).get("diagnostics", [])
        derrs = [d for d in ditems if d.get("severity") == 1]
        print(f"   frame[{di}] items={len(ditems)} errors={len(derrs)}")
        for d in ditems[:8]:
            rng = d.get("range", {}).get("start", {})
            print(f"      {d.get('code','?'):10} L{rng.get('line','?')}:{rng.get('character','?')} {d.get('message','')[:120]}")
    items = []
    if diags:
        items = diags[0].get("params", {}).get("diagnostics", [])
    if pulled and pulled.get("items"):
        items = pulled["items"]
    print(f"[probe] (legacy) diag items = {len(items)}")
    # 统计错误/警告
    errs = [d for d in items if d.get("severity") == 1]
    warns = [d for d in items if d.get("severity") == 2]
    print(f"[probe] severity errors={len(errs)} warnings={len(warns)}")
    for d in items[:60]:
        rng = d.get("range", {}).get("start", {})
        print(f"   {d.get('code','?'):10} L{rng.get('line','?')}:{rng.get('character','?')} {d.get('message','')[:140]}")
    print("[probe] done.")


if __name__ == "__main__":
    main()
