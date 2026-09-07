#!/usr/bin/env python3
"""Functional checks for the ioma HTTP layer: python3 tests/smoke.py [port].

Talks to tests/server.c (make check builds and runs it): GET /, /health, /whoami, /users/:id, POST /echo, ...
"""
import socket
import sys
import time

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8080


def connect(timeout=3):
    # back-to-back runs leave thousands of sockets in TIME-WAIT; an ephemeral-port hiccup is
    # retried a few times instead of failing the suite
    for attempt in range(20):
        try:
            s = socket.create_connection(("127.0.0.1", PORT))
            break
        except OSError:
            if attempt == 19:
                raise
            time.sleep(0.05)
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

# route parameter + percent-decoded query parameter
st, hd, body = get("/users/42?fields=a%20b+c&x=1")
results.append(check("GET /users/:id -> route param + decoded query param", st == 200 and body == b"user 42 fields=a b c\n"))
st, hd, body = get("/users/42/extra")
results.append(check("  extra segment does not match the pattern", st == 404))

# a body far larger than the reply buffer streams: chunked on HTTP/1.1 (http.client decodes it)
import http.client
hc = http.client.HTTPConnection("127.0.0.1", int(sys.argv[1]), timeout=10)
hc.request("GET", "/stream?n=5000")
r = hc.getresponse(); data = r.read(); hc.close()
results.append(check("GET /stream -> chunked stream, every line arrives",
                     r.status == 200 and r.getheader("transfer-encoding") == "chunked"
                     and data.count(b"\n") == 5000 and data.endswith(b"line 5000 of 5000\n")))
# the same on HTTP/1.0: no length known, so it streams until close
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=10)
s.send(b"GET /stream?n=3000 HTTP/1.0\r\nHost: x\r\n\r\n")
raw = b""
while True:
    chunk = s.recv(65536)
    if not chunk: break
    raw += chunk
s.close()
head, _, body = raw.partition(b"\r\n\r\n")
results.append(check("GET /stream on HTTP/1.0 -> until close, no length, all lines",
                     b"connection: close" in head and b"content-length" not in head
                     and b"chunked" not in head and body.count(b"\n") == 3000))

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
    good &= st == 200 and body == b"ok" and hd.get("connection") != "close"
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

# ── the body is read on demand ──────────────────────────────────────────────────────────
# a handler that ignores the body (here the 404 fallback) still leaves the connection in sync
s = connect()
s.send(b"POST /nope HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhelloGET /health HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
raw = b""
while True:                      # both replies may arrive in one packet: read to the close
    chunk = s.recv(65536)
    if not chunk: break
    raw += chunk
s.close()
results.append(check("unread body drained, pipelined next request served",
                     raw.startswith(b"HTTP/1.1 404") and raw.count(b"HTTP/1.1 200") == 1 and raw.endswith(b"ok")))

# a 1 MB upload streamed through a 4 KB loop (the request buffer is 16 KB), then chunked
hc = http.client.HTTPConnection("127.0.0.1", int(sys.argv[1]), timeout=10)
hc.request("POST", "/upload", body=b"x" * 1048576)
r = hc.getresponse(); data = r.read()
results.append(check("POST /upload 1 MB streamed in", r.status == 200 and data == b"1048576 bytes\n"))
def gen():
    for _ in range(64):
        yield b"y" * 4096
hc.request("POST", "/upload", body=gen(), encode_chunked=True)
r = hc.getresponse(); data = r.read()
results.append(check("POST /upload 256 KB chunked streamed in", r.status == 200 and data == b"262144 bytes\n"))
hc.close()

# a body too large to read whole is answered 413
s = connect()
s.send(b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 100000\r\n\r\n" + b"z" * 100000)
st, hd, body = read_response(s)
s.close()
results.append(check("ioma_body on a 100 KB body -> 413", st == 413))

# an ignored body past the drain limit: the reply is served and says close
s = connect()
s.send(b"POST /nope HTTP/1.1\r\nHost: x\r\nContent-Length: 2097152\r\n\r\n")
try:
    s.sendall(b"w" * 2097152)
except OSError:
    pass
try:
    st, hd, body = read_response(s)
except Exception:
    st, hd, body = None, {}, b""
s.close()
results.append(check("ignored 2 MB body -> reply served with Connection: close", st == 404 and hd.get("connection") == "close"))

print("all passed" if all(results) else "FAILURES")
sys.exit(0 if all(results) else 1)
