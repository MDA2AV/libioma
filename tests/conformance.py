#!/usr/bin/env python3
"""HTTP/1.1 conformance at the wire level (RFC 9110/9112), against tests/server.c: the request
framing the engine must refuse, the replies that must carry no body, Expect: 100-continue, the
reply head a handler cannot corrupt, and a declared Content-Length held to. Every case sends raw
bytes and reads raw bytes, so what the client library would hide is visible.

    python3 tests/conformance.py [port]
"""
import socket, sys, time

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8080

results = []


def check(name, cond):
    print(f"{'ok ' if cond else 'FAIL'} {name}")
    results.append(cond)
    return cond


def connect(timeout=3):
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


def read_all(s):
    """Everything until the peer closes, or a timeout: (bytes, closed)."""
    buf = b""
    try:
        while True:
            c = s.recv(65536)
            if not c:
                return buf, True
            buf += c
    except socket.timeout:
        return buf, False


def exchange(raw, timeout=3):
    """Send raw bytes, read until close or timeout."""
    s = connect(timeout)
    s.sendall(raw)
    data, closed = read_all(s)
    s.close()
    return data, closed


def split_responses(data, head_first=False):
    """The responses in a byte stream, as (status, headers, body) triples; the head is parsed
    strictly so a malformed head shows up as a failure, not as a guess. head_first: the first
    response answers a HEAD, so it ends at its blank line whatever its headers say."""
    out = []
    while data:
        if b"\r\n\r\n" not in data:
            out.append(("MALFORMED", {}, data))
            break
        head, _, rest = data.partition(b"\r\n\r\n")
        lines = head.split(b"\r\n")
        parts = lines[0].split(b" ", 2)
        status = int(parts[1]) if parts[0] == b"HTTP/1.1" and len(parts) >= 2 and parts[1].isdigit() else "MALFORMED"
        headers = {}
        for line in lines[1:]:
            k, sep, v = line.partition(b": ")
            if not sep:
                status = "MALFORMED"
            headers[k.decode(errors="replace").lower()] = v.decode(errors="replace")
        if head_first and not out:
            body = b""
        elif "transfer-encoding" in headers:
            body, rest = dechunk(rest)
        else:
            n = int(headers.get("content-length", "0"))
            body, rest = rest[:n], rest[n:]
        out.append((status, headers, body))
        data = rest
    return out


def dechunk(raw):
    out = b""
    while True:
        line, _, raw = raw.partition(b"\r\n")
        n = int(line.split(b";")[0], 16)
        if n == 0:
            _, _, raw = raw.partition(b"\r\n")   # the empty trailer section
            return out, raw
        out += raw[:n]
        raw = raw[n + 2:]


def one(raw, timeout=3):
    """One request: its first response and whether the connection closed after the stream."""
    data, closed = exchange(raw, timeout)
    rs = split_responses(data)
    return (rs[0] if rs else ("NONE", {}, b"")), closed, len(rs)


SMUGGLE = b"GET /health HTTP/1.1\r\nHost: x\r\n\r\n"

# ── request framing the engine must refuse (400 and close), each one a smuggling vector ──────
for name, head in [
    ("Content-Length with a sign",          b"Content-Length: +5\r\n"),
    ("Content-Length in hex",               b"Content-Length: 0x5\r\n"),
    ("Content-Length with trailing junk",   b"Content-Length: 5abc\r\n"),
    ("Content-Length as a list",            b"Content-Length: 5, 5\r\n"),
    ("Content-Length past 64 bits",         b"Content-Length: 18446744073709551621\r\n"),
    ("Content-Length empty",                b"Content-Length: \r\n"),
    ("two Content-Length that disagree",    b"Content-Length: 5\r\nContent-Length: 0\r\n"),
    ("Transfer-Encoding and Content-Length", b"Transfer-Encoding: chunked\r\nContent-Length: 5\r\n"),
    ("an obs-folded Content-Length",        b"Content-Length:\r\n 5\r\n"),
    ("an obs-folded Transfer-Encoding",     b"Transfer-Encoding:\r\n chunked\r\n"),
]:
    (st, hd, body), closed, n = one(b"POST /echo HTTP/1.1\r\nHost: x\r\n" + head + b"\r\nAAAAA" + SMUGGLE)
    check(f"{name} -> 400 and close, nothing smuggled", st == 400 and closed and n == 1)

(st, hd, body), closed, n = one(b"POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: gzip\r\n\r\nAAAAA" + SMUGGLE)
check("an unknown transfer coding -> 501 and close", st == 501 and closed and n == 1)
(st, hd, body), closed, n = one(b"POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked, gzip\r\n\r\nAAAAA" + SMUGGLE)
check("chunked not the last coding -> 501 and close, nothing smuggled", st == 501 and closed and n == 1)
(st, hd, body), closed, n = one(b"POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: identity\r\n\r\nAAAAA" + SMUGGLE)
check("TE.TE: chunked then identity -> 501 and close, nothing smuggled", st == 501 and closed and n == 1)
(st, hd, body), closed, n = one(b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\nhello")
check("two Content-Length that agree -> accepted", st == 200 and body == b"hello")

# ── Host ─────────────────────────────────────────────────────────────────────────────────────
(st, hd, body), closed, n = one(b"GET /health HTTP/1.1\r\n\r\n")
check("HTTP/1.1 without Host -> 400", st == 400 and closed)
(st, hd, body), closed, n = one(b"GET /health HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n")
check("two Host lines -> 400", st == 400 and closed)
(st, hd, body), closed, n = one(b"GET /health HTTP/1.0\r\n\r\n")
check("HTTP/1.0 without Host -> served", st == 200 and body == b"ok")
(st, hd, body), closed, n = one(b"GET http://127.0.0.1:%d/health HTTP/1.1\r\nHost: x\r\n\r\n" % PORT)
check("absolute-form target -> routed by its path", st == 200 and body == b"ok")
(st, hd, body), closed, n = one(b"GET http://x HTTP/1.1\r\nHost: x\r\n\r\n")
check("absolute-form target with no path -> '/'", st == 200 and b"hello" in body)

# ── Connection: every line counts, close wins ────────────────────────────────────────────────
data, closed = exchange(b"GET /health HTTP/1.1\r\nHost: x\r\nConnection: close\r\nConnection: keep-alive\r\n\r\n" + SMUGGLE)
rs = split_responses(data)
check("Connection: close on an earlier line wins -> one reply, closed",
      len(rs) == 1 and rs[0][1].get("connection") == "close" and closed)

# ── chunked bodies ───────────────────────────────────────────────────────────────────────────
def chunked_post(body_bytes):
    return one(b"POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n" + body_bytes)

(st, hd, body), closed, n = chunked_post(b"5\r\nHELLO\r\n0\r\n\r\n")
check("a plain chunked body -> echoed", st == 200 and body == b"HELLO")
(st, hd, body), closed, n = chunked_post(b"5;ext=1\r\nHELLO\r\n0\r\n\r\n")
check("a chunk extension -> accepted", st == 200 and body == b"HELLO")
(st, hd, body), closed, n = chunked_post(b"5 \r\nHELLO\r\n0\r\n\r\n")
check("bare whitespace after the chunk size -> 400", st == 400 and closed)
(st, hd, body), closed, n = chunked_post(b"5;\r\nHELLO\r\n0\r\n\r\n")
check("an empty chunk extension -> 400", st == 400 and closed)
(st, hd, body), closed, n = chunked_post(b"5\r\nHELLO\r\n0\r\nx: 1\r\ny: 2\r\n\r\n")
check("trailers -> taken, body echoed", st == 200 and body == b"HELLO")
(st, hd, body), closed, n = chunked_post(b"0\r\n" + b"x: " + b"y" * 3000 + b"\r\n" + b"z: " + b"w" * 3000 + b"\r\n\r\n")
check("trailers past their cap -> 400 and close", st == 400 and closed)
(st, hd, body), closed, n = one(b"POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n"
                                b"5;" + b"e" * 20000 + b"\r\nHELLO\r\n0\r\n\r\n")
check("a chunk size line the reader cannot hold -> 400", st == 400 and closed)

# ── replies that carry no body ───────────────────────────────────────────────────────────────
data, closed = exchange(b"HEAD / HTTP/1.1\r\nHost: x\r\n\r\nGET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
rs = split_responses(data, head_first=True)
check("HEAD / -> the GET's head, no body, then the pipelined GET answered",
      len(rs) == 2 and rs[0][0] == 200 and rs[0][1].get("content-length") == rs[1][1].get("content-length")
      and rs[0][2] == b"" and rs[1][2] == b"hello from ioxd\n")
data, closed = exchange(b"HEAD /stream HTTP/1.1\r\nHost: x\r\n\r\nGET /health HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
rs = split_responses(data, head_first=True)
check("HEAD of a streamed reply -> chunked head, no chunks, next request answered",
      len(rs) == 2 and rs[0][0] == 200 and rs[0][1].get("transfer-encoding") == "chunked" and rs[0][2] == b""
      and rs[1][2] == b"ok")
data, closed = exchange(b"HEAD /nope HTTP/1.1\r\nHost: x\r\n\r\nGET /health HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
rs = split_responses(data, head_first=True)
check("HEAD of an unrouted path -> 404 head, no body", len(rs) == 2 and rs[0][0] == 404 and rs[0][2] == b"" and rs[1][2] == b"ok")

for path, code, framing in [("/status/204", 204, None), ("/status/304", 304, None), ("/status/204?body=1", 204, None)]:
    data, closed = exchange(b"GET %s HTTP/1.1\r\nHost: x\r\n\r\nGET /health HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n" % path.encode())
    rs = split_responses(data)
    check(f"{path} -> {code} with no body, no framing header, the next request answered",
          len(rs) == 2 and rs[0][0] == code and rs[0][2] == b"" and "content-length" not in rs[0][1]
          and "transfer-encoding" not in rs[0][1] and rs[1][2] == b"ok")

# ── the reply head a handler cannot corrupt ──────────────────────────────────────────────────
(st, hd, body), closed, n = one(b"GET /reflect?name=x-echo&value=safe HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
check("a reflected header value -> sent", st == 200 and hd.get("x-echo") == "safe" and body == b"added")
(st, hd, body), closed, n = one(b"GET /reflect?name=x-echo&value=a%0d%0ax-evil:%201 HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
check("a value with CRLF -> refused by ioxd_header, no injected line",
      st == 200 and "x-evil" not in hd and "x-echo" not in hd and body == b"refused")
(st, hd, body), closed, n = one(b"GET /reflect?name=x-echo&value=a%00b HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
check("a value with %00 -> kept literal, sent as text", st == 200 and hd.get("x-echo") == "a%00b")
(st, hd, body), closed, n = one(b"GET /reflect?name=bad%20name&value=v HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
check("a name that is not a token -> refused", st == 200 and body == b"refused")
(st, hd, body), closed, n = one(b"GET /reflect?name=Content-Length&value=0 HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
check("an engine-owned header -> refused, the real content-length stands", st == 200 and hd.get("content-length") == "7" and body == b"refused")
(st, hd, body), closed, n = one(b"GET /reflect?name=Content-Type&value=text/x-custom HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
check("content-type through ioxd_header -> the content type", st == 200 and hd.get("content-type") == "text/x-custom")
(st, hd, body), closed, n = one(b"GET /reflect?name=x-local&value=stack HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
check("a header from a handler-local buffer -> copied, intact on the wire", st == 200 and hd.get("x-local") == "stack")

# ── the status line ──────────────────────────────────────────────────────────────────────────
(st, hd, body), closed, n = one(b"GET /status/99 HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
check("a status outside 100-999 -> 500", st == 500)
(st, hd, body), closed, n = one(b"GET /status/418 HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
check("an unlisted status -> its number, a reason phrase", st == 418)

# ── a declared length, held to ───────────────────────────────────────────────────────────────
(st, hd, body), closed, n = one(b"GET /promise?say=100&write=9 HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
check("a declared length larger than a buffered body -> the real length", st == 200 and hd.get("content-length") == "9" and body == b"123456789")
data, closed = exchange(b"GET /promise?say=3&write=19&stream=1 HTTP/1.1\r\nHost: x\r\n\r\nGET /health HTTP/1.1\r\nHost: x\r\n\r\n")
rs = split_responses(data)
check("a streamed body past its declared length -> cut at the length, connection closed",
      len(rs) == 1 and rs[0][1].get("content-length") == "3" and rs[0][2] == b"123" and closed)
data, closed = exchange(b"GET /promise?say=100&write=9&stream=1 HTTP/1.1\r\nHost: x\r\n\r\nGET /health HTTP/1.1\r\nHost: x\r\n\r\n")
check("a streamed body short of its declared length -> the connection closes (the client sees the cut)",
      closed and data.startswith(b"HTTP/1.1 200") and len(split_responses(data + b"")) <= 1)

# ── Expect: 100-continue ─────────────────────────────────────────────────────────────────────
s = connect()
s.sendall(b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nExpect: 100-continue\r\nConnection: close\r\n\r\n")
first = s.recv(4096)
got_continue = first.startswith(b"HTTP/1.1 100 Continue\r\n\r\n")
s.sendall(b"hello")
rest, closed = read_all(s)
s.close()
rs = split_responses(first[len(b"HTTP/1.1 100 Continue\r\n\r\n"):] + rest) if got_continue else []
check("Expect: 100-continue -> 100 Continue before the body is read, then the reply",
      got_continue and len(rs) == 1 and rs[0][0] == 200 and rs[0][2] == b"hello")
s = connect()
s.sendall(b"POST /health HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nExpect: 100-continue\r\n\r\n")
data, closed = read_all(s)
s.close()
rs = split_responses(data)
check("Expect with a handler that never reads the body -> final status, connection closed, no wait",
      not data.startswith(b"HTTP/1.1 100") and len(rs) == 1 and rs[0][1].get("connection") == "close" and closed)
(st, hd, body), closed, n = one(b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nExpect: 200-please\r\n\r\nhello")
check("an expectation we do not know -> 417", st == 417)

# ── query parameters: the whole query or nothing ─────────────────────────────────────────────
many = b"&".join(b"p%d=1" % i for i in range(40))
(st, hd, body), closed, n = one(b"GET /params?" + many + b" HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
check("more query parameters than fit -> 400, never a partial view", st == 400)
huge = b"v=" + b"%41" * 1500
(st, hd, body), closed, n = one(b"GET /params?" + huge + b" HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
check("more decoded query than the arena holds -> 414", st == 414)

failed = results.count(False)
print("all passed" if not failed else f"{failed} FAILED")
sys.exit(1 if failed else 0)
