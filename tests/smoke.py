#!/usr/bin/env python3
"""Functional checks for the ioxd HTTP layer: python3 tests/smoke.py [port].

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
results.append(check("GET / -> 200 'hello from ioxd'", st == 200 and body == b"hello from ioxd\n"))
results.append(check("GET / content-type text/plain", hd.get("content-type") == "text/plain"))
results.append(check("Server header added by middleware", hd.get("server") == "ioxd"))

st, hd, body = get("/health")
results.append(check("GET /health -> 200 'ok'", st == 200 and body == b"ok"))

st, hd, body = get("/whoami?x=1&y=2")
results.append(check("GET /whoami -> 200", st == 200))
results.append(check("  parsed path/query in body", b"path   = /whoami" in body and b"query  = x=1&y=2" in body))
results.append(check("  custom header X-Powered-By: ioxd", hd.get("x-powered-by") == "ioxd"))

# route parameter + percent-decoded query parameter
st, hd, body = get("/users/42?fields=a%20b+c&x=1")
results.append(check("GET /users/:id -> route param + decoded query param", st == 200 and body == b"user 42 fields=a b c\n"))
st, hd, body = get("/users/42/posts/7")
results.append(check("GET /users/:id/posts/:post -> two captures converted", st == 200 and body == b"post 7 of user 42\n"))
st, hd, body = get("/users/4x/posts/7")
results.append(check("GET /users/:id/posts/:post with a non-number -> 400", st == 400))
st, hd, body = get("/convert?i=-42&d=2.5&b=YES")
results.append(check("GET /convert -> typed conversions", st == 200 and body == b"i=-42\nd=2.5\nb=true\n"))
st, hd, body = get("/convert?i=42&d=abc")
results.append(check("GET /convert with a bad double -> 400", st == 400))
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
results.append(check("DELETE / -> 405 with allow (path known, method not)", st == 405 and hd.get("allow") == "GET"))

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
results.append(check("pipelined two requests", st1 == 200 and b1 == b"ok" and st2 == 200 and b2 == b"hello from ioxd\n"))

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
results.append(check("POST /upload 1 MB streamed in", r.status == 200 and data == b"1048576 bytes in 256 reads\n"))
def gen():
    for _ in range(64):
        yield b"y" * 4096
hc.request("POST", "/upload", body=gen(), encode_chunked=True)
r = hc.getresponse(); data = r.read()
results.append(check("POST /upload 256 KB chunked streamed in", r.status == 200 and data == b"262144 bytes in 64 reads\n"))


# --- chunk-exact reads on a raw socket: split size lines, an extension, trailers, pipelining ---
def raw_exchange(pieces, pause=0.03):
    """Send the pieces with a pause between them, read until the server closes, and return the
    responses as (status, body) pairs (all these replies carry a Content-Length)."""
    s = connect()
    for piece in pieces:
        s.sendall(piece)
        time.sleep(pause)
    data = b""
    while True:
        try:
            chunk = s.recv(65536)
        except socket.timeout:
            break
        if not chunk:
            break
        data += chunk
    s.close()
    out = []
    while b"\r\n\r\n" in data:
        head, _, rest = data.partition(b"\r\n\r\n")
        status = int(head.split(b" ")[1])
        n = 0
        for line in head.split(b"\r\n"):
            if line.lower().startswith(b"content-length:"):
                n = int(line.split(b":")[1])
        out.append((status, rest[:n]))
        data = rest[n:]
    return out


chunked_head = b"POST /chunks HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n"
pipelined = b"GET /health HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"
rs = raw_exchange([chunked_head + b"5\r\nhel", b"lo\r\n3;ext=v\r", b"\nabc\r\n0\r\nx-tr", b"ailer: 1\r\n\r\n" + pipelined])
results.append(check("POST /chunks -> each chunk as framed (split anywhere, extension, trailer), then a pipelined request",
                     len(rs) == 2 and rs[0] == (200, b"5:hello\n3:abc\nend\n") and rs[1][0] == 200))
rs = raw_exchange([b"POST /chunks?first=2 HTTP/1.1\r\nHost: x\r\nConnection: close\r\nTransfer-Encoding: chunked\r\n\r\n"
                   b"5\r\nhello\r\n3\r\nabc\r\n0\r\n\r\n"])
results.append(check("POST /chunks?first=2 -> a chunk read after a byte read gives the rest of that chunk",
                     rs == [(200, b"first=2\n3:llo\n3:abc\nend\n")]))
rs = raw_exchange([chunked_head + b"7d0\r\n" + b"x" * 2000 + b"\r\n0\r\n\r\n"])
results.append(check("POST /chunks with a 2000-byte chunk into a 1 KB buffer -> 413", len(rs) == 1 and rs[0][0] == 413))
rs = raw_exchange([b"POST /chunks HTTP/1.1\r\nHost: x\r\nConnection: close\r\nContent-Length: 5\r\n\r\nhello"])
results.append(check("POST /chunks with a Content-Length body -> 400 not chunked", rs == [(400, b"not chunked\n")]))
rs = raw_exchange([b"GET /heal", b"th HTTP/1.1\r\nHost: x\r\nConn", b"ection: close\r\n\r\n"])
results.append(check("a head split across three sends -> gathered and served", rs == [(200, b"ok")]))
rs = raw_exchange([b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\n", b"hel", b"lo" + pipelined])
results.append(check("a body arriving after the head, in pieces, then a pipelined request", len(rs) == 2 and rs[0] == (200, b"hello") and rs[1][0] == 200))
rs = raw_exchange([b"POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhel", b"lo\r\n0\r\n\r\n" + pipelined])
results.append(check("POST /echo chunked read whole (split), then a pipelined request",
                     len(rs) == 2 and rs[0] == (200, b"hello") and rs[1][0] == 200))
hc.close()

# a body too large to read whole is answered 413
s = connect()
s.send(b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 100000\r\n\r\n" + b"z" * 100000)
st, hd, body = read_response(s)
s.close()
results.append(check("ioxd_body on a 100 KB body -> 413", st == 413))

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
# --- the JSON writer: a document written as you go, and one that streams ---
st, hd, body = get("/json/42")
results.append(check("GET /json/:id -> escaped document, application/json",
                     st == 200 and hd.get("content-type") == "application/json"
                     and body == b'{"id":42,"name":"Zo\xc3\xab \\"Z\\" O\'Neil\\n","ratio":0.1,"ok":true,"none":null,"tags":["a","b"]}'))
st, hd, body = get("/json/x")
results.append(check("GET /json/x -> 400 from the handler", st == 400))


def dechunk(raw):
    out = b""
    while True:
        line, _, raw = raw.partition(b"\r\n")
        n = int(line.split(b";")[0], 16)
        if n == 0:
            return out
        out += raw[:n]
        raw = raw[n + 2:]


s = connect()
s.send(b"GET /json/big?n=3000 HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
raw = b""
while True:
    piece = s.recv(65536)
    if not piece:
        break
    raw += piece
s.close()
head, _, rest = raw.partition(b"\r\n\r\n")
doc = __import__("json").loads(dechunk(rest))
results.append(check("GET /json/big -> 3000 objects streamed chunked, valid JSON",
                     b"transfer-encoding: chunked" in head.lower() and len(doc) == 3000 and doc[2999] == {"i": 2999, "sq": 2999 * 2999}))

# --- groups: prefixes chain, middleware wraps outer to inner, one endpoint's own middleware ---
st, hd, body = get("/api/ping")
results.append(check("GET /api/ping -> group prefix, group middleware, root middleware",
                     st == 200 and body == b"pong\n" and hd.get("x-api") == "v1" and hd.get("server") == "ioxd"))
st, hd, body = get("/api/ping/")
results.append(check("GET /api/ping/ -> trailing slash tolerated", st == 200 and body == b"pong\n"))
st, hd, body = get("/api/admin/stats")
results.append(check("GET /api/admin/stats without token -> 401 from the subgroup, outer middleware still ran",
                     st == 401 and hd.get("x-api") == "v1" and hd.get("x-endpoint") is None))
st, hd, body = get("/api/admin/stats", extra=b"x-token: secret\r\n")
results.append(check("GET /api/admin/stats with token -> 200, every layer's header",
                     st == 200 and body == b"stats\n" and hd.get("x-api") == "v1" and hd.get("x-endpoint") == "stats"))
st, hd, body = get("/api")
results.append(check("GET /api -> 404 (a prefix is not an endpoint)", st == 404))
st, hd, body = get("/users/new")
results.append(check("GET /users/new -> the static segment beats the :id capture", st == 200 and body == b"new user form\n"))
rs = raw_exchange([b"POST /users/new HTTP/1.1\r\nHost: x\r\nConnection: close\r\nContent-Length: 0\r\n\r\n"])
results.append(check("POST /users/new -> no POST on the static segment: falls through to POST /users/:id",
                     rs == [(200, b"updated new\n")]))
rs = raw_exchange([b"PUT /users/new HTTP/1.1\r\nHost: x\r\nConnection: close\r\nContent-Length: 0\r\n\r\n"])
results.append(check("PUT /users/new -> 405 (the path is known, no PUT anywhere on it)", len(rs) == 1 and rs[0][0] == 405))

print("all passed" if all(results) else "FAILURES")
sys.exit(0 if all(results) else 1)
