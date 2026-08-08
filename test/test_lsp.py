#!/usr/bin/env python3
"""
Shadow 0.4 — LSP 服务器端到端测试（Phase 14）。
参照 0.3 test_lsp.py 协议：把 JSON-RPC 帧拼成字节流灌入 `shadow --lsp` 的 stdin，
解析 stdout 的 Content-Length 帧后按 id 断言。

用法：
    python test/test_lsp.py [shadow.exe 路径]
"""
import os, sys, json, subprocess

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(REPO, "build", "shadow.exe")
if len(sys.argv) > 1:
    EXE = sys.argv[1]

DEMO = """dsb demo;

struct Point {
    x: int;
    y: int;
}

kimo dist2(p: Point) -> int {
    return p.x * p.x + p.y * p.y;
}

kimo main() -> int {
    let p = Point { x: 3, y: 4 };
    println(dist2(p));
    return 0;
}
"""

BAD = """dsb bad;

kimo main() -> int {
    let x: int = no_such_function_here();
    return 0;
}
"""

DEMO_URI = "file:///C:/demo/main.shadow"
BAD_URI = "file:///C:/demo/bad.shadow"


def frame(obj):
    body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
    return b"Content-Length: " + str(len(body)).encode() + b"\r\n\r\n" + body


def parse_frames(data):
    """解析 stdout 字节流中的 JSON-RPC 帧，返回 (dict, raw_bytes) 列表。"""
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
            frames.append((json.loads(body.decode("utf-8", "replace")), body))
        except Exception:
            frames.append((None, body))
        buf = buf[idx + 4 + clen:]
    return frames


def by_id(frames):
    out = {}
    for f, _ in frames:
        if f is not None and "id" in f:
            out[str(f["id"])] = f
    return out


def notifications(frames, method):
    return [f for f, _ in frames if f is not None and f.get("method") == method]


def run_lsp(messages):
    script = b"".join(frame(m) for m in messages)
    p = subprocess.run([EXE, "--lsp"], input=script, capture_output=True, timeout=60)
    return parse_frames(p.stdout)


def scenario_basic():
    """生命周期 + hover + definition + completion + 诊断推送。"""
    reqs = [
        {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"capabilities": {}}},
        {"jsonrpc": "2.0", "method": "initialized", "params": {}},
        {"jsonrpc": "2.0", "method": "textDocument/didOpen",
         "params": {"textDocument": {"uri": DEMO_URI, "languageId": "shadow", "version": 1, "text": DEMO}}},
        {"jsonrpc": "2.0", "id": 2, "method": "textDocument/hover",
         "params": {"textDocument": {"uri": DEMO_URI}, "position": {"line": 13, "character": 13}}},
        {"jsonrpc": "2.0", "id": 3, "method": "textDocument/definition",
         "params": {"textDocument": {"uri": DEMO_URI}, "position": {"line": 13, "character": 13}}},
        {"jsonrpc": "2.0", "id": 4, "method": "textDocument/completion",
         "params": {"textDocument": {"uri": DEMO_URI}, "position": {"line": 13, "character": 12}}},
        {"jsonrpc": "2.0", "id": 5, "method": "textDocument/documentSymbol",
         "params": {"textDocument": {"uri": DEMO_URI}}},
        {"jsonrpc": "2.0", "id": 6, "method": "textDocument/semanticTokens/full",
         "params": {"textDocument": {"uri": DEMO_URI}}},
        {"jsonrpc": "2.0", "id": 7, "method": "textDocument/formatting",
         "params": {"textDocument": {"uri": DEMO_URI}, "options": {}}},
        {"jsonrpc": "2.0", "id": 8, "method": "shutdown", "params": None},
        {"jsonrpc": "2.0", "method": "exit", "params": None},
    ]
    frames = run_lsp(reqs)
    r = by_id(frames)
    ok = True

    def check(cond, name):
        nonlocal ok
        if not cond:
            ok = False
            print(f"  [FAIL] {name}")

    # initialize：capabilities 必须含 hoverProvider
    init = r.get("1", {})
    caps = init.get("result", {}).get("capabilities", {})
    check(caps.get("hoverProvider") is True, "initialize.capabilities.hoverProvider")
    check(caps.get("definitionProvider") is True, "initialize.capabilities.definitionProvider")
    check(caps.get("textDocumentSync") == 2, "initialize.textDocumentSync==2")
    # hover：dist2 调用处返回签名
    hov = r.get("2", {})
    hov_val = json.dumps(hov.get("result", {}))
    check("fn dist2" in hov_val or "dist2" in hov_val, f"hover contains dist2 ({hov_val[:120]})")
    # definition：dist2 定义位置 line=7（0-based）
    dfn = r.get("3", {})
    dfn_res = dfn.get("result")
    if isinstance(dfn_res, dict):
        sl = dfn_res.get("range", {}).get("start", {}).get("line")
        check(sl == 7, f"definition line==7 (got {sl})")
    else:
        check(False, f"definition is dict (got {dfn_res!r})")
    # completion：包含 dist2 和 Point
    comp = json.dumps(r.get("4", {}).get("result", {}))
    check("dist2" in comp, "completion contains dist2")
    check("Point" in comp, "completion contains Point")
    check("len" in comp, "completion contains builtin len")
    # documentSymbol：包含 dist2 和 Point
    dsym = json.dumps(r.get("5", {}).get("result", {}))
    check("dist2" in dsym, "documentSymbol contains dist2")
    check("Point" in dsym, "documentSymbol contains Point")
    # semanticTokens：data 非空
    st = r.get("6", {}).get("result", {})
    check(isinstance(st.get("data"), list) and len(st["data"]) > 0, "semanticTokens data non-empty")
    # formatting：返回 TextEdit（newText 含重新缩进）
    fmt = r.get("7", {}).get("result")
    check(isinstance(fmt, list) and len(fmt) > 0 and "newText" in fmt[0], "formatting TextEdit")
    # didOpen 推送了 publishDiagnostics（无错误）
    diags = notifications(frames, "textDocument/publishDiagnostics")
    check(len(diags) >= 1, "publishDiagnostics pushed on didOpen")
    if diags:
        items = diags[0].get("params", {}).get("diagnostics", [])
        check(len(items) == 0, f"demo diagnostics empty (got {len(items)})")
    # shutdown → null
    check(r.get("8", {}).get("result") is None, "shutdown result null")
    return ok


def scenario_diagnostics():
    """语法/类型错误 → publishDiagnostics 非空。"""
    reqs = [
        {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}},
        {"jsonrpc": "2.0", "method": "textDocument/didOpen",
         "params": {"textDocument": {"uri": BAD_URI, "languageId": "shadow", "version": 1, "text": BAD}}},
        {"jsonrpc": "2.0", "id": 2, "method": "textDocument/diagnostic",
         "params": {"textDocument": {"uri": BAD_URI}}},
        {"jsonrpc": "2.0", "method": "exit", "params": None},
    ]
    frames = run_lsp(reqs)
    ok = True
    diags = notifications(frames, "textDocument/publishDiagnostics")
    if not diags or not diags[0].get("params", {}).get("diagnostics"):
        ok = False
        print("  [FAIL] bad doc should produce diagnostics")
    else:
        items = diags[0]["params"]["diagnostics"]
        joined = json.dumps(items)
        if "SH-TC001" not in joined:
            ok = False
            print(f"  [FAIL] diagnostics contain SH-TC001 (got {joined[:150]})")
        else:
            print(f"  [PASS] bad doc diagnostics: {len(items)} items")
    # pull 模式
    r = by_id(frames)
    pull = r.get("2", {}).get("result", {})
    if not pull.get("items"):
        ok = False
        print("  [FAIL] textDocument/diagnostic pull returns items")
    return ok


def scenario_incremental():
    """增量 didChange：修改内容后诊断/查询反映新状态。"""
    reqs = [
        {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}},
        {"jsonrpc": "2.0", "method": "textDocument/didOpen",
         "params": {"textDocument": {"uri": DEMO_URI, "languageId": "shadow", "version": 1, "text": DEMO}}},
        # 把 `println(dist2(p));` 改为 `println(dist2(p)); let q = 1;` —— 增量 patch
        {"jsonrpc": "2.0", "method": "textDocument/didChange",
         "params": {"textDocument": {"uri": DEMO_URI, "version": 2},
                    "contentChanges": [{"range": {"start": {"line": 15, "character": 0},
                                                  "end": {"line": 15, "character": 0}},
                                        "text": "    // hello\n"}]}},
        {"jsonrpc": "2.0", "id": 2, "method": "textDocument/hover",
         "params": {"textDocument": {"uri": DEMO_URI}, "position": {"line": 13, "character": 13}}},
        {"jsonrpc": "2.0", "method": "exit", "params": None},
    ]
    frames = run_lsp(reqs)
    r = by_id(frames)
    ok = True
    hov = json.dumps(r.get("2", {}).get("result", {}))
    if "dist2" not in hov:
        ok = False
        print(f"  [FAIL] incremental hover still works ({hov[:100]})")
    return ok


def scenario_rename():
    """rename：替换所有引用。"""
    reqs = [
        {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}},
        {"jsonrpc": "2.0", "method": "textDocument/didOpen",
         "params": {"textDocument": {"uri": DEMO_URI, "languageId": "shadow", "version": 1, "text": DEMO}}},
        {"jsonrpc": "2.0", "id": 2, "method": "textDocument/rename",
         "params": {"textDocument": {"uri": DEMO_URI},
                    "position": {"line": 13, "character": 13},
                    "newName": "dist3"}},
        {"jsonrpc": "2.0", "method": "exit", "params": None},
    ]
    frames = run_lsp(reqs)
    r = by_id(frames)
    res = r.get("2", {}).get("result", {})
    ok = True
    edits = res.get("changes", {}).get(DEMO_URI, [])
    if not edits:
        ok = False
        print(f"  [FAIL] rename produces edits ({json.dumps(res)[:150]})")
    else:
        for ed in edits:
            if ed.get("newText") != "dist3":
                ok = False
                print(f"  [FAIL] rename newText=dist3 (got {ed.get('newText')})")
    return ok


def main():
    print(f"== LSP e2e tests ({EXE}) ==")
    total = passed = 0
    for name, fn in [("basic", scenario_basic), ("diagnostics", scenario_diagnostics),
                     ("incremental", scenario_incremental), ("rename", scenario_rename)]:
        total += 1
        try:
            ok = fn()
        except Exception as e:
            ok = False
            print(f"  [EXC] {name}: {e}")
        if ok:
            passed += 1
            print(f"[PASS] {name}")
        else:
            print(f"[FAIL] {name}")
    print(f"== LSP e2e: PASS={passed}/{total} ==")
    return 0 if passed == total else 1


if __name__ == "__main__":
    sys.exit(main())
