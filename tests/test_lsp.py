#!/usr/bin/env python3
import json
import subprocess
import sys
import time

class LspClient:
    def __init__(self, cmd=['./vn', 'lsp']):
        self.proc = subprocess.Popen(
            cmd,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            bufsize=0
        )
        self.msg_id = 0

    def send(self, obj):
        body = json.dumps(obj).encode('utf-8')
        header = f"Content-Length: {len(body)}\r\n\r\n".encode('ascii')
        self.proc.stdin.write(header + body)
        self.proc.stdin.flush()

    def receive(self):
        line = self.proc.stdout.readline()
        if not line:
            return None
        while line and not line.startswith(b"Content-Length:"):
            line = self.proc.stdout.readline()
        if not line:
            return None
        length = int(line.split(b":")[1].strip())
        # read empty line
        blank = self.proc.stdout.readline()
        # read body
        body = self.proc.stdout.read(length)
        return json.loads(body.decode('utf-8'))

    def call(self, method, params=None):
        self.msg_id += 1
        req_id = self.msg_id
        req = {
            "jsonrpc": "2.0",
            "id": req_id,
            "method": method
        }
        if params is not None:
            req["params"] = params
        self.send(req)
        while True:
            res = self.receive()
            if res and res.get("id") == req_id:
                return res

    def notify(self, method, params=None):
        req = {
            "jsonrpc": "2.0",
            "method": method
        }
        if params is not None:
            req["params"] = params
        self.send(req)

    def close(self):
        if self.proc and self.proc.poll() is None:
            try:
                self.notify("exit")
            except Exception:
                pass
            try:
                if self.proc.stdin:
                    self.proc.stdin.close()
            except Exception:
                pass
            try:
                self.proc.wait(timeout=1.0)
            except Exception:
                try:
                    self.proc.kill()
                    self.proc.wait()
                except Exception:
                    pass
            if self.proc.stdout:
                try: self.proc.stdout.close()
                except Exception: pass
            if self.proc.stderr:
                try: self.proc.stderr.close()
                except Exception: pass

def test_parameter_provenance():
    client = LspClient()
    res = client.call("initialize", {"capabilities": {}})
    assert res and "result" in res

    doc_uri = "file:///test_params.vn"
    code = "fn foo(a, b: int) -> int { return a + b; }\nfn plain(x, y) { return x; }\n"
    client.notify("textDocument/didOpen", {
        "textDocument": {
            "uri": doc_uri,
            "languageId": "varian",
            "version": 1,
            "text": code
        }
    })

    # Hover on foo
    hover_foo = client.call("textDocument/hover", {
        "textDocument": {"uri": doc_uri},
        "position": {"line": 0, "character": 3}
    })
    val_foo = hover_foo["result"]["contents"]["value"]
    assert "fn foo(a, b: int) -> int" in val_foo, f"Unexpected hover for foo: {val_foo}"

    # Hover on plain
    hover_plain = client.call("textDocument/hover", {
        "textDocument": {"uri": doc_uri},
        "position": {"line": 1, "character": 3}
    })
    val_plain = hover_plain["result"]["contents"]["value"]
    assert "fn plain(x, y)" in val_plain, f"Unexpected hover for plain: {val_plain}"

    client.close()

def test_hover_depth():
    client = LspClient()
    res = client.call("initialize", {"capabilities": {}})
    assert res and "result" in res

    doc_uri = "file:///test_hover.vn"
    code = (
        "/// Calculate area\n"
        "fn area(w: int, h: int) -> int { return w * h; }\n"
        "struct Point { x: int, y: int }\n"
        "fn main() {\n"
        "    let a = area(10, 20);\n"
        "    let p = Point { x: 1, y: 2 };\n"
        "}\n"
    )
    client.notify("textDocument/didOpen", {
        "textDocument": {
            "uri": doc_uri,
            "languageId": "varian",
            "version": 1,
            "text": code
        }
    })

    # 1. Docstring test on declaration
    hover_area = client.call("textDocument/hover", {
        "textDocument": {"uri": doc_uri},
        "position": {"line": 1, "character": 3}
    })
    val_area = hover_area["result"]["contents"]["value"]
    assert "Calculate area" in val_area, f"Docstring missing: {val_area}"

    # 2. Call site hover
    hover_call = client.call("textDocument/hover", {
        "textDocument": {"uri": doc_uri},
        "position": {"line": 4, "character": 12}
    })
    val_call = hover_call["result"]["contents"]["value"]
    assert "fn area(w: int, h: int) -> int" in val_call, f"Call signature missing: {val_call}"

    # 3. Struct literal hover lists fields
    hover_struct = client.call("textDocument/hover", {
        "textDocument": {"uri": doc_uri},
        "position": {"line": 5, "character": 14}
    })
    val_struct = hover_struct["result"]["contents"]["value"]
    assert "struct Point {" in val_struct and "x" in val_struct, f"Struct fields missing: {val_struct}"
    client.close()

def test_completion():
    client = LspClient()
    res = client.call("initialize", {"capabilities": {}})
    assert res and "result" in res

    doc_uri = "file:///test_comp.vn"
    code = (
        "use \"math\"\n"
        "fn calculate(width: int, height: int) -> int {\n"
        "    let msg = \"hello\";\n"
        "    msg.\n"
        "    calculate(\n"
        "}\n"
    )
    client.notify("textDocument/didOpen", {
        "textDocument": {
            "uri": doc_uri,
            "languageId": "varian",
            "version": 1,
            "text": code
        }
    })

    # 1. Module completion after use "
    print("Testing use completion...")
    comp_use = client.call("textDocument/completion", {
        "textDocument": {"uri": doc_uri},
        "position": {"line": 0, "character": 5}
    })
    items_use = [item["label"] for item in comp_use["result"]["items"]]
    assert "math" in items_use or "json" in items_use, f"Module completion failed: {comp_use}"
    print("Use completion OK")

    # 2. Dot completion on string
    print("Testing dot completion...")
    comp_dot = client.call("textDocument/completion", {
        "textDocument": {"uri": doc_uri},
        "position": {"line": 3, "character": 8}
    })
    items_dot = [item["label"] for item in comp_dot["result"]["items"]]
    assert "len" in items_dot or "push" in items_dot, f"Dot completion failed: {comp_dot}"
    print("Dot completion OK")

    # 3. Call parameter completion
    print("Testing call completion...")
    comp_call = client.call("textDocument/completion", {
        "textDocument": {"uri": doc_uri},
        "position": {"line": 4, "character": 14}
    })
    items_call = [item["label"] for item in comp_call["result"]["items"]]
    assert "width" in items_call and "height" in items_call, f"Parameter completion failed: {comp_call}"
    print("Call completion OK")

    # 4. Statement completion
    print("Testing stmt completion...")
    comp_stmt = client.call("textDocument/completion", {
        "textDocument": {"uri": doc_uri},
        "position": {"line": 1, "character": 0}
    })
    items_stmt = {item["label"]: item for item in comp_stmt["result"]["items"]}
    assert "let" in items_stmt and "detail" in items_stmt["let"], f"Statement completion failed: {comp_stmt}"
    print("Stmt completion OK")

    client.close()

def test_signature_help():
    print("Testing signature help...")
    client = LspClient()
    res = client.call("initialize", {"capabilities": {}})
    assert res and "result" in res

    doc_uri = "file:///test_sig.vn"
    code = (
        "/// Calculate area\n"
        "fn area(width: int, height: int) -> int { return width * height; }\n"
        "fn main() {\n"
        "    let a = area(10, 20);\n"
        "}\n"
    )
    client.notify("textDocument/didOpen", {
        "textDocument": {
            "uri": doc_uri,
            "languageId": "varian",
            "version": 1,
            "text": code
        }
    })

    # 1. Parameter 0 (before comma)
    sig0 = client.call("textDocument/signatureHelp", {
        "textDocument": {"uri": doc_uri},
        "position": {"line": 3, "character": 17}
    })
    assert sig0 and "result" in sig0 and sig0["result"] is not None, f"Signature help failed: {sig0}"
    assert sig0["result"]["activeParameter"] == 0, f"Expected param 0, got {sig0['result']['activeParameter']}"
    params0 = sig0["result"]["signatures"][0]["parameters"]
    assert len(params0) == 2 and params0[0]["label"] == "width: int"

    # 2. Parameter 1 (after comma)
    sig1 = client.call("textDocument/signatureHelp", {
        "textDocument": {"uri": doc_uri},
        "position": {"line": 3, "character": 21}
    })
    assert sig1 and "result" in sig1 and sig1["result"] is not None, f"Signature help failed: {sig1}"
    assert sig1["result"]["activeParameter"] == 1, f"Expected param 1, got {sig1['result']['activeParameter']}"

    print("Signature help OK")
    client.close()

def test_inlay_hints():
    print("Testing inlay hints...")
    client = LspClient()
    res = client.call("initialize", {"capabilities": {}})
    assert res and "result" in res

    doc_uri = "file:///test_inlay.vn"
    code = (
        "fn area(width: int, height: int) -> int { return width * height; }\n"
        "fn main() {\n"
        "    let w = 10;\n"
        "    let h = 20;\n"
        "    let a = area(w, h);\n"
        "}\n"
    )
    client.notify("textDocument/didOpen", {
        "textDocument": {
            "uri": doc_uri,
            "languageId": "varian",
            "version": 1,
            "text": code
        }
    })

    hints = client.call("textDocument/inlayHint", {
        "textDocument": {"uri": doc_uri},
        "range": {
            "start": {"line": 0, "character": 0},
            "end": {"line": 10, "character": 0}
        }
    })
    assert hints and "result" in hints and isinstance(hints["result"], list), f"Inlay hints failed: {hints}"
    labels = [h["label"] for h in hints["result"]]
    assert "width:" in labels and "height:" in labels, f"Expected width: and height:, got {labels}"
    print("Inlay hints OK")
    client.close()

def test_symbols():
    print("Testing document & workspace symbols...")
    client = LspClient()
    res = client.call("initialize", {"capabilities": {}})
    assert res and "result" in res

    doc_uri = "file:///test_sym.vn"
    code = (
        "struct Point {\n"
        "    x: int,\n"
        "    y: int\n"
        "}\n"
        "impl Point { fn area(self) -> int { return self.x * self.y; } }\n"
        "fn calculate_distance(p1, p2) { return 0; }\n"
    )
    client.notify("textDocument/didOpen", {
        "textDocument": {
            "uri": doc_uri,
            "languageId": "varian",
            "version": 1,
            "text": code
        }
    })

    # Document symbols
    doc_syms = client.call("textDocument/documentSymbol", {
        "textDocument": {"uri": doc_uri}
    })
    assert doc_syms and "result" in doc_syms and isinstance(doc_syms["result"], list)
    names = [s["name"] for s in doc_syms["result"]]
    assert "Point" in names and "calculate_distance" in names, f"Expected Point & calculate_distance, got {names}"

    # Check hierarchy: Point should have children (x, y, area)
    point_sym = next(s for s in doc_syms["result"] if s["name"] == "Point")
    assert "children" in point_sym and point_sym["children"] is not None
    child_names = [c["name"] for c in point_sym["children"]]
    assert "x" in child_names and "y" in child_names and "area" in child_names, f"Expected x, y, area in Point children, got {child_names}"
    assert point_sym["selectionRange"]["end"]["character"] - point_sym["selectionRange"]["start"]["character"] == len("Point")
    x_sym = next(s for s in point_sym["children"] if s["name"] == "x")
    assert x_sym["selectionRange"]["start"] == {"line": 1, "character": 4}, x_sym

    # Workspace symbols
    ws_syms = client.call("workspace/symbol", {
        "query": "calc"
    })
    assert ws_syms and "result" in ws_syms and isinstance(ws_syms["result"], list)
    ws_names = [s["name"] for s in ws_syms["result"]]
    assert "calculate_distance" in ws_names, f"Expected calculate_distance in workspace symbols, got {ws_names}"

    print("Symbols OK")
    client.close()

def test_lsp_resilience():
    print("Testing incomplete, multiline, UTF-16, and large LSP inputs...")
    client = LspClient()
    assert "result" in client.call("initialize", {"capabilities": {}})

    incomplete_uri = "file:///incomplete.vn"
    incomplete_code = (
        "fn calculate(width: int, height: int) -> int { return width * height; }\n"
        "fn broken() {\n"
        "    let emoji = \"😀\"; calculate(\n"
        "        other(1),\n"
    )
    client.notify("textDocument/didOpen", {"textDocument": {
        "uri": incomplete_uri, "languageId": "varian", "version": 1, "text": incomplete_code
    }})

    completion = client.call("textDocument/completion", {
        "textDocument": {"uri": incomplete_uri},
        "position": {"line": 3, "character": 0},
    })
    labels = {item["label"] for item in completion["result"]["items"]}
    assert {"width", "height"} <= labels, labels
    uri = "file:///" + ("deep/" * 260) + "resilience.vn"
    fields = "\n".join(f"    field_{i}: int," for i in range(60))
    code = f"let emoji = \"😀\"; struct Large {{\n{fields}\n}}\nfn calculate() {{ return 1; }}\n"
    client.notify("textDocument/didOpen", {"textDocument": {
        "uri": uri, "languageId": "varian", "version": 1, "text": code
    }})
    symbols = client.call("textDocument/documentSymbol", {"textDocument": {"uri": uri}})
    large = next(item for item in symbols["result"] if item["name"] == "Large")
    expected_col = len('let emoji = "😀"; struct '.encode("utf-16-le")) // 2
    assert large["selectionRange"]["start"] == {"line": 0, "character": expected_col}, large
    assert len(large["children"]) == 60
    assert large["children"][-1]["name"] == "field_59"

    workspace = client.call("workspace/symbol", {"query": "calculate"})
    match = next(item for item in workspace["result"] if item["name"] == "calculate")
    assert match["location"]["uri"] == uri

    refused = client.call("textDocument/rename", {
        "textDocument": {"uri": uri},
        "position": {"line": 62, "character": 3},
        "newName": "let",
    })
    assert refused.get("error", {}).get("code") == -32602, refused
    client.close()

if __name__ == "__main__":
    test_parameter_provenance()
    test_hover_depth()
    test_completion()
    test_signature_help()
    test_inlay_hints()
    test_symbols()
    test_lsp_resilience()
    print("All LSP tests passed!")
    sys.exit(0)
