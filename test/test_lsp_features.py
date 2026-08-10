#!/usr/bin/env python3
"""
Shadow 0.5 — 三个 LSP 编辑器特性回归测试：
  1. inlayHint：自动推断类型后加空格（`: string ` 而非 `: stringm`）
  2. hover：内部函数（非 pub）显示用法 + 标注「（内部函数）」
  3. 模块限定补全 `c.` 列出模块成员；signatureHelp 显示注释（函数级 + paras 参数级）
"""
import os, sys, json, subprocess, tempfile, shutil

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(REPO, "build", "shadow.exe")
if len(sys.argv) > 1:
    EXE = sys.argv[1]


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


def run_lsp(messages, timeout=120):
    script = b"".join(frame(m) for m in messages)
    p = subprocess.run([EXE, "--lsp"], input=script, capture_output=True, timeout=timeout)
    return parse_frames(p.stdout)


def scenario_inlay_hint():
    """let m = "abc"; 的 inlayHint label 必须是 `: string `（带尾随空格）。"""
    # dsb 名必须与文件名一致（SH-MOD001），否则编译中断、元数据为空
    DOC = """dsb ih;

kimo main() -> int {
    let m = "abc";
    return 0;
}
"""
    URI = "file:///C:/demo/ih.shadow"
    reqs = [
        {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}},
        {"jsonrpc": "2.0", "method": "initialized", "params": {}},
        {"jsonrpc": "2.0", "method": "textDocument/didOpen",
         "params": {"textDocument": {"uri": URI, "languageId": "shadow", "version": 1, "text": DOC}}},
        {"jsonrpc": "2.0", "id": 2, "method": "textDocument/inlayHint",
         "params": {"textDocument": {"uri": URI}, "range": {"start": {"line": 0, "character": 0},
                                                            "end": {"line": 10, "character": 0}}}},
        {"jsonrpc": "2.0", "method": "exit", "params": None},
    ]
    frames = run_lsp(reqs)
    r = by_id(frames)
    res = r.get("2", {}).get("result")
    ok = True
    if not isinstance(res, list) or len(res) == 0:
        ok = False
        print(f"  [FAIL] inlayHint: empty result ({res!r})")
        return ok
    # 找到标注字符串类型的 hint（label 含 'string'）
    hit = None
    for h in res:
        lbl = h.get("label", "")
        if "string" in lbl:
            hit = lbl
            break
    if hit is None:
        ok = False
        print(f"  [FAIL] inlayHint: no 'string' label in {res}")
        return ok
    # 尾随空格修复：label 应为 ': string '（末尾有空格，不与变量名黏连）
    if hit != ": string ":
        ok = False
        print(f"  [FAIL] inlayHint: label spacing wrong (got {hit!r}, want ': string ')")
    else:
        print(f"  [PASS] inlayHint: label={hit!r}")
    return ok


def scenario_hover_internal():
    """DEMO 中 dist2 是 kimo（非 pub）内部函数，hover 应含「（内部函数）」标记。"""
    DEMO = """dsb hov;

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
    URI = "file:///C:/demo/hov.shadow"
    reqs = [
        {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}},
        {"jsonrpc": "2.0", "method": "textDocument/didOpen",
         "params": {"textDocument": {"uri": URI, "languageId": "shadow", "version": 1, "text": DEMO}}},
        # dist2 调用点：println(dist2(p)); → line 13（0-based），dist2 位于 col 12-16
        {"jsonrpc": "2.0", "id": 2, "method": "textDocument/hover",
         "params": {"textDocument": {"uri": URI}, "position": {"line": 13, "character": 13}}},
        {"jsonrpc": "2.0", "method": "exit", "params": None},
    ]
    frames = run_lsp(reqs)
    r = by_id(frames)
    # ensure_ascii=False：否则中文被转义成 \uXXXX，断言「（内部函数）」必然误判
    hov = json.dumps(r.get("2", {}).get("result", {}), ensure_ascii=False)
    ok = True
    if "fn dist2" not in hov:
        ok = False
        print(f"  [FAIL] hover: no signature for dist2 ({hov[:120]})")
    if "（内部函数）" not in hov:
        ok = False
        print(f"  [FAIL] hover: internal-function marker missing ({hov[:160]})")
    else:
        print("  [PASS] hover: shows signature + （内部函数）")
    return ok


def scenario_module_completion_and_signature():
    """严格态多模块：import c；输入 c. 触发模块成员补全；c.c( 触发带注释的 signatureHelp。"""
    d = tempfile.mkdtemp(prefix="shadow_lsp_feat_")
    try:
        with open(os.path.join(d, "c.shadow"), "w", encoding="utf-8") as f:
            f.write(
                "dsb c;\n\n"
                "// 计算两数之和\n"
                "// paras m: 第一个整数\n"
                "// paras n: 第二个整数\n"
                "pub kimo c(m: int, n: int) -> int{\n"
                "    return 1;\n"
                "}\n\n"
                "// 无参内部函数\n"
                "pub kimo b() -> int{\n"
                "    return 2;\n"
                "}\n"
            )
        MAIN = ("dsb main;\nimport c;\n\n"
                "kimo main() -> int {\n"
                "    let a = c.c(;\n"
                "    return 0;\n"
                "}\n")
        uri = "file:///" + d.replace("\\", "/") + "/main.shadow"
        reqs = [
            {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}},
            {"jsonrpc": "2.0", "method": "textDocument/didOpen",
             "params": {"textDocument": {"uri": uri, "languageId": "shadow", "version": 1, "text": MAIN}}},
            # 补全：光标紧跟 `c.` 之后 → character = len("    let a = c.") = 14
            # （传 13 时光标停在 '.' 前，prefix 只有 "c"，会退化成普通顶层补全）
            {"jsonrpc": "2.0", "id": 2, "method": "textDocument/completion",
             "params": {"textDocument": {"uri": uri}, "position": {"line": 4, "character": 14}}},
            # signatureHelp：光标紧跟 `c.c(` 之后（'(' 位于索引 15）
            {"jsonrpc": "2.0", "id": 3, "method": "textDocument/signatureHelp",
             "params": {"textDocument": {"uri": uri}, "position": {"line": 4, "character": 15}}},
            {"jsonrpc": "2.0", "method": "exit", "params": None},
        ]
        frames = run_lsp(reqs, timeout=120)
        r = by_id(frames)
        ok = True

        # 1) 补全应包含 c 与 b（模块 c 的成员）
        # 断言必须看 items[].label —— 直接子串匹配整个 JSON 会被 "isIncomplete" 里的 c 误命中
        cres = r.get("2", {}).get("result", {}) or {}
        clabels = [it.get("label", "") for it in (cres.get("items") or [])]
        if "c" not in clabels or "b" not in clabels:
            ok = False
            print(f"  [FAIL] completion: c./b not listed ({clabels})")
        else:
            print(f"  [PASS] completion: c.c / c.b listed ({clabels})")

        # 2) signatureHelp：label 含签名；documentation 含函数级注释；
        #    parameters 含 m:int/n:int 且带 paras 参数级文档
        sh = r.get("3", {}).get("result", {})
        sigs = sh.get("signatures", [])
        if not sigs:
            ok = False
            print(f"  [FAIL] signatureHelp: no signatures ({sh})")
        else:
            sig0 = sigs[0]
            label = sig0.get("label", "")
            doc = json.dumps(sig0.get("documentation", {}), ensure_ascii=False)
            params = sig0.get("parameters", [])
            plabels = [p.get("label", "") for p in params]
            pall = json.dumps(params, ensure_ascii=False)
            if "fn c(m: int, n: int) -> int" not in label:
                ok = False
                print(f"  [FAIL] signatureHelp: label wrong ({label})")
            if "计算两数之和" not in doc:
                ok = False
                print(f"  [FAIL] signatureHelp: func-level doc missing ({doc[:200]})")
            if "m: int" not in plabels or "n: int" not in plabels:
                ok = False
                print(f"  [FAIL] signatureHelp: param labels wrong ({plabels})")
            if "第一个整数" not in pall:
                ok = False
                print(f"  [FAIL] signatureHelp: paras param doc missing ({pall[:300]})")
            if ok:
                print("  [PASS] signatureHelp: label + func doc + paras param doc all present")
        return ok
    finally:
        shutil.rmtree(d, ignore_errors=True)


def scenario_value_completion():
    """值成员补全：let p = Point{...}; p. 列出 struct 字段 x / y（编译器 type-at-cursor 解析）。"""
    DOC = """dsb val;

struct Point {
    x: int;
    y: int;
}

kimo main() -> int {
    let p = Point { x: 3, y: 4 };
    let z = p.;
    return 0;
}
"""
    URI = "file:///C:/demo/val.shadow"
    # 第 9 行（0-based）: "    let z = p." ，光标在 '.' 之后 → character = len("    let z = p.") = 14
    reqs = [
        {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}},
        {"jsonrpc": "2.0", "method": "textDocument/didOpen",
         "params": {"textDocument": {"uri": URI, "languageId": "shadow", "version": 1, "text": DOC}}},
        {"jsonrpc": "2.0", "id": 2, "method": "textDocument/completion",
         "params": {"textDocument": {"uri": URI}, "position": {"line": 9, "character": 14}}},
        {"jsonrpc": "2.0", "method": "exit", "params": None},
    ]
    frames = run_lsp(reqs, timeout=120)
    r = by_id(frames)
    vres = r.get("2", {}).get("result", {}) or {}
    vlabels = [it.get("label", "") for it in (vres.get("items") or [])]
    ok = True
    if "x" not in vlabels or "y" not in vlabels:
        ok = False
        print(f"  [FAIL] value-completion: Point fields x/y not listed ({vlabels})")
    else:
        print(f"  [PASS] value-completion: p. lists struct fields ({vlabels})")
    return ok


def main():
    print(f"== LSP feature tests ({EXE}) ==")
    total = passed = 0
    for name, fn in [("inlay_hint", scenario_inlay_hint),
                     ("hover_internal", scenario_hover_internal),
                     ("module_completion_sighelp", scenario_module_completion_and_signature),
                     ("value_completion", scenario_value_completion)]:
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
    print(f"== LSP feature tests: PASS={passed}/{total} ==")
    return 0 if passed == total else 1


if __name__ == "__main__":
    sys.exit(main())
