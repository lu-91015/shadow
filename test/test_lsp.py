#!/usr/bin/env python3
"""
Shadow 0.4 — LSP 服务器端到端测试（Phase 14）。
参照 0.3 test_lsp.py 协议：把 JSON-RPC 帧拼成字节流灌入 `shadow --lsp` 的 stdin，
解析 stdout 的 Content-Length 帧后按 id 断言。

用法：
    python test/test_lsp.py [shadow.exe 路径]
"""
import os, sys, json, subprocess, tempfile, shutil

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(REPO, "build", "shadow.exe")
if len(sys.argv) > 1:
    EXE = sys.argv[1]

DEMO = """dsb main;

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


def run_lsp_long(messages, timeout=1500):
    """长超时版本：self-host 场景加载整棵编译器源码 import 图。
    实测：单次全量加载 ~9-14 分钟；didOpen+diagnostic+definition 三次编译需预留充足余量。"""
    script = b"".join(frame(m) for m in messages)
    p = subprocess.run([EXE, "--lsp"], input=script, capture_output=True, timeout=timeout)
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
    # didOpen 已即时化（推空诊断，不再同步全量编译）。真实诊断由 pull
    # (textDocument/diagnostic, id=2) 触发惰性编译后产出。
    # didOpen 推送的 publishDiagnostics 允许为空——这是即时化后的预期行为。
    r = by_id(frames)
    pull = r.get("2", {}).get("result", {})
    items = pull.get("items", []) if pull else []
    if not items:
        ok = False
        print("  [FAIL] textDocument/diagnostic pull returns items")
    else:
        joined = json.dumps(items)
        if "SH-TC001" not in joined:
            ok = False
            print(f"  [FAIL] pull diagnostics contain SH-TC001 (got {joined[:150]})")
        else:
            print(f"  [PASS] bad doc pull diagnostics: {len(items)} items")
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


def scenario_multimodule():
    """严格态：跨模块符号解析（import 另一模块并调用其 pub 函数）。
    验证 LSP 走 main_load_combined 加载模块图后，跨模块符号可被正确解析
    （不报 undefined function）。"""
    d = tempfile.mkdtemp(prefix="shadow_lsp_mm_")
    try:
        with open(os.path.join(d, "geom.shadow"), "w") as f:
            f.write("dsb geom;\n\npub kimo area(r: int) -> int {\n    return r * r;\n}\n")
        MAIN = ("dsb main;\nimport geom;\n\n"
                "kimo main() -> int {\n    let a = area(3);\n    println(a);\n    return 0;\n}\n")
        uri = "file:///" + d.replace("\\", "/") + "/main.shadow"
        reqs = [
            {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}},
            {"jsonrpc": "2.0", "method": "textDocument/didOpen",
             "params": {"textDocument": {"uri": uri, "languageId": "shadow", "version": 1, "text": MAIN}}},
            # didOpen 不再同步全量编译（推空诊断）；用 pull diagnostic 触发惰性编译验证跨模块解析
            {"jsonrpc": "2.0", "id": 2, "method": "textDocument/diagnostic",
             "params": {"textDocument": {"uri": uri}}},
            {"jsonrpc": "2.0", "method": "exit", "params": None},
        ]
        frames = run_lsp(reqs)
        r = by_id(frames)
        ok = True
        # didOpen 推空诊断（清残留），不应含错误
        diags = notifications(frames, "textDocument/publishDiagnostics")
        if not diags:
            ok = False
            print("  [FAIL] multimodule: no publishDiagnostics pushed on didOpen")
        else:
            oitems = diags[0].get("params", {}).get("diagnostics", [])
            ojoined = json.dumps(oitems)
            if "undefined function" in ojoined or "module error" in ojoined:
                ok = False
                print(f"  [FAIL] multimodule: didOpen diag unexpected: {ojoined[:200]}")
        # pull diagnostic（id=2）触发惰性全量编译 → 验证跨模块 'area' 已解析
        pulled = r.get("2", {}).get("result", {})
        items = pulled.get("items", []) if pulled else []
        joined = json.dumps(items)
        if "undefined function" in joined:
            ok = False
            print(f"  [FAIL] multimodule: cross-module symbol unresolved: {joined[:200]}")
        elif "cannot read module" in joined or "module error" in joined:
            ok = False
            print(f"  [FAIL] multimodule: module load failed: {joined[:200]}")
        else:
            print(f"  [PASS] multimodule: cross-module 'area' resolved/clean ({len(items)} diag items)")
        return ok
    finally:
        shutil.rmtree(d, ignore_errors=True)


def scenario_selfhost():
    """最大测试集：加载 0.5 编译器自身源码树（src/main.shadow 及其全部 import）。
    断言：
    1. 二次编译（pull diagnostic）无 prelude 假错误（tc_prelude_registered 跨调用残留修复）
    2. 跨模块 definition：对 main.shadow 里的 lsp_run() 调用点，应指向 lsp.shadow 而非 main.shadow
       （combined 坐标 → 模块内行号 + 真实路径 映射修复；含 dsb 行偏移补偿）。
    依赖真实源码布局：main.shadow 的 --lsp 分支调用 lsp_run()（行号随源码变化）。
    """
    main_path = os.path.join(REPO, "src", "main.shadow")
    if not os.path.exists(main_path):
        print("  [SKIP] selfhost: src/main.shadow not found")
        return True
    with open(main_path, "r", encoding="utf-8") as f:
        text = f.read()
    uri = "file:///" + main_path.replace("\\", "/")
    # 定位 main.shadow 中 lsp_run( 调用点（main() 的 --lsp 分支），取标识符起始列
    call_line = None
    for ln_no, ln in enumerate(text.splitlines(), start=1):
        if "lsp_run(" in ln:
            call_line = ln_no
            break
    if call_line is None:
        print("  [SKIP] selfhost: lsp_run( call site not found in src/main.shadow")
        return True
    col = text.splitlines()[call_line - 1].find("lsp_run") + 1  # 1-based
    reqs = [
        {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}},
        {"jsonrpc": "2.0", "method": "textDocument/didOpen",
         "params": {"textDocument": {"uri": uri, "languageId": "shadow", "version": 1, "text": text}}},
        {"jsonrpc": "2.0", "id": 2, "method": "textDocument/diagnostic",
         "params": {"textDocument": {"uri": uri}}},
        {"jsonrpc": "2.0", "id": 3, "method": "textDocument/definition",
         "params": {"textDocument": {"uri": uri},
                    "position": {"line": call_line - 1, "character": col - 1}}},
        {"jsonrpc": "2.0", "method": "exit", "params": None},
    ]
    try:
        frames = run_lsp_long(reqs)
    except subprocess.TimeoutExpired:
        print("  [FAIL] selfhost: LSP timeout (1500s)")
        return False
    ok = True

    def check(cond, name):
        nonlocal ok
        if not cond:
            ok = False
            print(f"  [FAIL] selfhost: {name}")

    # 1. 二次编译诊断（pull id=2）—— prelude 残留修复
    r = by_id(frames)
    pulled = r.get("2", {}).get("result", {})
    items = pulled.get("items", []) if pulled else []
    errs = [d for d in items if d.get("severity") == 1]
    check(len(errs) == 0, f"selfhost diagnostic clean (got {len(errs)} errors: "
                          f"{[d.get('message','')[:80] for d in errs[:5]]})")
    # didOpen push 也应干净（首编）
    for nf in notifications(frames, "textDocument/publishDiagnostics"):
        pitems = nf.get("params", {}).get("diagnostics", [])
        perrs = [d for d in pitems if d.get("severity") == 1]
        check(len(perrs) == 0, f"selfhost didOpen push clean (got {len(perrs)} errors)")
        break
    # 2. 跨模块 definition：lsp_run 定义在 src/lsp/lsp.shadow
    dfn = r.get("3", {}).get("result")
    if not dfn:
        check(False, "definition has result")
        return ok
    loc = dfn[0] if isinstance(dfn, list) else dfn
    duri = loc.get("uri", "")
    dline = loc.get("range", {}).get("start", {}).get("line")  # 0-based
    check("lsp.shadow" in duri, f"definition uri -> lsp.shadow (got {duri})")
    check("main.shadow" not in duri, "definition uri not main.shadow")
    # lsp_run 定义在 lsp.shadow 的 pub kimo lsp_run() 行（0-based；含 dsb 行补偿后应精确命中）
    lsp_path = os.path.join(REPO, "src", "lsp", "lsp.shadow")
    with open(lsp_path, "r", encoding="utf-8") as f:
        lsp_lines = f.read().splitlines()
    def_line = None
    for ln_no, ln in enumerate(lsp_lines, start=1):
        if ln.strip().startswith("pub kimo lsp_run"):
            def_line = ln_no - 1  # 0-based
            break
    if def_line is not None:
        check(dline == def_line, f"definition line == lsp_run decl ({dline} vs {def_line})")
    return ok


def main():
    print(f"== LSP e2e tests ({EXE}) ==")
    total = passed = 0
    for name, fn in [("basic", scenario_basic), ("diagnostics", scenario_diagnostics),
                     ("incremental", scenario_incremental), ("rename", scenario_rename),
                     ("multimodule", scenario_multimodule), ("selfhost", scenario_selfhost)]:
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
