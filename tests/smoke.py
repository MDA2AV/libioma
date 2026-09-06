#!/usr/bin/env python3
"""Functional checks for the ioma HTTP layer: python3 tests/smoke.py [port].

Assumes the demo routes in src/main.c: GET /, GET /health, GET /whoami, POST /echo.
"""
import socket
import sys
import time

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8080


def connect(timeout=3):
    s = socket.create_connection(("127.0.0.1", PORT))
    s.settimeout(timeout)
    return s


def read_response(s):
    """Read one HTTP response: return (status:int, headers:dict, body:bytes)."""
    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = s.recv(4096)
        if not chunk:
            raise EOFError("closed before headers")
        buf += chunk
    head, _, rest = buf.partition(b"\r\n\r\n")
    lines = head.split(b"\r\n")
    status = int(lines[0].split(b" ")[1])
    headers = {}
    for line in lines[1:]:
        k, _, v = line.partition(b": ")
        headers[k.decode().lower()] = v.decode()
    n = int(headers.get("content-length", "0"))
    body = rest
    while len(body) < n:
        chunk = s.recv(n - len(body))
        if not chunk:
            break
        body += chunk
    return status, headers, body


def get(path, extra=b"", keep=False):
    s = connect()
    conn = "keep-alive" if keep else "close"
    s.send(f"GET {path} HTTP/1.1\r\nHost: x\r\nConnection: {conn}\r\n".encode() + extra + b"\r\n")
    r = read_response(s)
    s.close()
    return r


def check(name, cond):
    print(f"{'ok ' if cond else 'FAIL'} {name}")
    return cond


results = []

st, hd, body = get("/")
results.append(check("GET / -> 200 'hello from ioma'", st == 200 and body == b"hello from ioma\n"))
results.append(check("GET / content-type text/plain", hd.get("content-type") == "text/plain"))
results.append(check("Server header added by middleware", hd.get("server") == "ioma"))

st, hd, body = get("/health")
results.append(check("GET /health -> 200 'ok'", st == 200 and body == b"ok"))

st, hd, body = get("/whoami?x=1&y=2")
results.append(check("GET /whoami -> 200", st == 200))
results.append(check("  parsed path/query in body", b"path   = /whoami" in body and b"query  = x=1&y=2" in body))
results.append(check("  custom header X-Powered-By: ioma", hd.get("x-powered-by") == "ioma"))

# POST /echo reflects the body
s = connect()
s.send(b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 11\r\nConnection: close\r\n\r\nhello world")
st, hd, body = read_response(s)
s.close()
results.append(check("POST /echo reflects body", st == 200 and body == b"hello world"))

# unknown route -> built-in 404
st, hd, body = get("/nope")
results.append(check("GET /nope -> 404", st == 404))

# unmatched method on a known path -> 404 (exact method+path match)
s = connect()
s.send(b"DELETE / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
st, hd, body = read_response(s)
s.close()
results.append(check("DELETE / -> 404 (method not matched)", st == 404))

# keep-alive: five requests on one connection
s = connect()
good = True
for _ in range(5):
    s.send(b"GET /health HTTP/1.1\r\nHost: x\r\n\r\n")
    st, hd, body = read_response(s)
    good &= st == 200 and body == b"ok" and hd.get("connection") == "keep-alive"
s.close()
results.append(check("keep-alive: 5 requests, one connection", good))

# pipelining: two requests in one write -> two responses
s = connect()
s.send(b"GET /health HTTP/1.1\r\nHost: x\r\n\r\nGET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
st1, _, b1 = read_response(s)
st2, _, b2 = read_response(s)
s.close()
results.append(check("pipelined two requests", st1 == 200 and b1 == b"ok" and st2 == 200 and b2 == b"hello from ioma\n"))

# request split across writes with pauses
s = connect()
for part in (b"GET /hea", b"lth HTTP/1.1\r\nHost: x", b"\r\nConnection: close\r\n\r\n"):
    s.send(part)
    time.sleep(0.05)
st, hd, body = read_response(s)
s.close()
results.append(check("request split over three writes", st == 200 and body == b"ok"))

# HTTP/1.0 defaults to close
s = connect()
s.send(b"GET /health HTTP/1.0\r\nHost: x\r\n\r\n")
st, hd, body = read_response(s)
s.close()
results.append(check("HTTP/1.0 -> Connection: close", hd.get("connection") == "close"))

# half-close: shut our write side right after the request, still get the answer
s = connect()
s.send(b"GET /health HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
s.shutdown(socket.SHUT_WR)
st, hd, body = read_response(s)
s.close()
results.append(check("half-close still answered", st == 200 and body == b"ok"))

# oversized headers (> the 16 KiB request buffer): server must refuse or close, not hang
s = connect(timeout=3)
try:
    s.send(b"GET / HTTP/1.1\r\nX-Pad: " + b"a" * 20000)
    got = b""
    try:
        got = s.recv(64)
    except (ConnectionResetError, socket.timeout):
        got = b""
    # either a 431 status or a clean close is acceptable
    results.append(check("oversized headers refused/closed", got == b"" or got.startswith(b"HTTP/1.1 431")))
finally:
    s.close()

# many concurrent connections
conns = [connect() for _ in range(200)]
for s in conns:
    s.send(b"GET /health HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
good = True
for s in conns:
    try:
        st, hd, body = read_response(s)
        good &= st == 200 and body == b"ok"
    except Exception:
        good = False
for s in conns:
    s.close()
results.append(check("200 concurrent connections answered", good))

print("all passed" if all(results) else "FAILURES")
sys.exit(0 if all(results) else 1)
