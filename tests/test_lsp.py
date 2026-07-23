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
        self.notify("exit")
        self.proc.terminate()
        self.proc.wait()

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

if __name__ == "__main__":
    test_parameter_provenance()
    test_hover_depth()
    print("All LSP hover tests passed!")
