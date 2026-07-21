#!/usr/bin/env python3
"""AER language server smoke test — drives binary/aer-lsp over raw JSON-RPC/stdio, the same
transport a real editor uses, without needing one running. Proves the transport, diagnostics
(via the real compiler), completion, and definition paths all work end to end.

Usage: python3 tests/lsp_smoke_test.py [path to aer-lsp binary]
"""
import json
import subprocess
import sys

BINARY = sys.argv[1] if len(sys.argv) > 1 else "binary/aer-lsp.exe"

proc = subprocess.Popen([BINARY], stdin=subprocess.PIPE, stdout=subprocess.PIPE, bufsize=0)
failures = 0


def check(cond, what):
    global failures
    print(("PASS: " if cond else "FAIL: ") + what)
    if not cond:
        failures += 1


def send(method, params=None, msg_id=None):
    body = {"jsonrpc": "2.0", "method": method}
    if params is not None:
        body["params"] = params
    if msg_id is not None:
        body["id"] = msg_id
    data = json.dumps(body).encode("utf-8")
    proc.stdin.write(f"Content-Length: {len(data)}\r\n\r\n".encode("ascii") + data)
    proc.stdin.flush()


def recv():
    header = b""
    while not header.endswith(b"\r\n\r\n"):
        b = proc.stdout.read(1)
        if not b:
            raise EOFError("aer-lsp closed stdout unexpectedly")
        header += b
    length = int(header.decode().split("Content-Length:")[1].split("\r\n")[0].strip())
    body = proc.stdout.read(length)
    return json.loads(body)


# 1. initialize
send("initialize", {}, msg_id=1)
resp = recv()
check(resp.get("id") == 1, "initialize responds with matching id")
caps = resp.get("result", {}).get("capabilities", {})
check(caps.get("definitionProvider") is True, "initialize declares definitionProvider")
check("completionProvider" in caps, "initialize declares completionProvider")

send("initialized", {})

# 2. didOpen with a deliberate syntax error -> expect a diagnostic
BAD_SOURCE = "x = 1\ny = (\n"
send("textDocument/didOpen", {"textDocument": {"uri": "file:///bad.aer", "text": BAD_SOURCE}})
diag = recv()
check(diag.get("method") == "textDocument/publishDiagnostics", "didOpen triggers publishDiagnostics")
diags = diag.get("params", {}).get("diagnostics", [])
check(len(diags) > 0, "a real syntax error produces at least one diagnostic")
if diags:
    check("uri" in diag["params"] and diag["params"]["uri"] == "file:///bad.aer", "diagnostic is published for the right document")

# 3. didChange with valid text -> diagnostics clear
GOOD_SOURCE = "function add(a, b):\n    return a + b\n\nprint(add(1, 2))\n"
send("textDocument/didChange", {"textDocument": {"uri": "file:///bad.aer"}, "contentChanges": [{"text": GOOD_SOURCE}]})
diag2 = recv()
diags2 = diag2.get("params", {}).get("diagnostics", [])
check(len(diags2) == 0, "valid text after a fix produces zero diagnostics (the error actually cleared, not just accumulated)")

# 4. completion after "math."
send("textDocument/didOpen", {"textDocument": {"uri": "file:///comp.aer", "text": "import math\nmath.sqrt(4.0)\n"}})
recv()  # publishDiagnostics for this open
send("textDocument/completion", {"textDocument": {"uri": "file:///comp.aer"}, "position": {"line": 1, "character": 5}}, msg_id=2)
comp = recv()
labels = [item["label"] for item in comp.get("result", [])]
check("sqrt" in labels, "completion includes math.sqrt")
check("connect" in labels, "completion includes other modules' functions too (net.connect)")

# 5. go-to-definition on a user-defined function call
send("textDocument/didOpen", {"textDocument": {"uri": "file:///def.aer", "text": GOOD_SOURCE}})
recv()  # publishDiagnostics
# GOOD_SOURCE line 3 (0-based) is "print(add(1, 2))" -- character 6 lands inside "add"
send("textDocument/definition", {"textDocument": {"uri": "file:///def.aer"}, "position": {"line": 3, "character": 7}}, msg_id=3)
defn = recv()
result = defn.get("result")
check(result is not None, "go-to-definition on a call to a real function returns a location, not null")
if result:
    check(result["range"]["start"]["line"] == 0, "go-to-definition points at line 0, where 'function add(...)' is declared")

# 6. completion detail flags a function containing raise as "may fail", and leaves one that
# doesn't with no detail at all.
RAISE_SOURCE = "function risky(n):\n    if n < 0:\n        raise \"bad\"\n    return n\n\nfunction safe(n):\n    return n + 1\n"
send("textDocument/didOpen", {"textDocument": {"uri": "file:///raise.aer", "text": RAISE_SOURCE}})
recv()  # publishDiagnostics
send("textDocument/completion", {"textDocument": {"uri": "file:///raise.aer"}, "position": {"line": 6, "character": 0}}, msg_id=5)
comp2 = recv()
by_label = {item["label"]: item for item in comp2.get("result", [])}
check(by_label.get("risky", {}).get("detail") == "may fail", "a function containing raise is flagged 'may fail' in completion")
check("detail" not in by_label.get("safe", {}), "a function with no raise anywhere gets no such detail")

# 7. shutdown / exit
send("shutdown", {}, msg_id=4)
resp = recv()
check(resp.get("id") == 4, "shutdown responds")
send("exit", {})
proc.wait(timeout=5)
check(proc.returncode == 0, "exit terminates the process cleanly")

print()
if failures == 0:
    print("All LSP smoke tests passed.")
    sys.exit(0)
else:
    print(f"{failures} LSP smoke test(s) FAILED.")
    sys.exit(1)
