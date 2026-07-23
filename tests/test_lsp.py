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

if __name__ == "__main__":
    test_parameter_provenance()
    test_hover_depth()
    test_completion()
    test_signature_help()
    print("All LSP tests passed!")
    sys.exit(0)
