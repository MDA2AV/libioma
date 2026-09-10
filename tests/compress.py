#!/usr/bin/env python3
"""The compression middleware: python3 tests/compress.py [port].

Talks to tests/server.c, where ioxd_compress is listed on /json/big, /zip/stream, /zip/declared,
/zip/health and /zip/raw and nowhere else. Every coded body is decoded and compared with the
plain one; brotli bodies are decoded when the brotli module is there and checked for their
framing otherwise.
"""
import gzip
import socket
import sys
import time

try:
    import brotli
except ImportError:                                    # pip install brotli; the framing is still checked
    brotli = None

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8080


def connect(timeout=5):
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


def read_response(s, head_only=False):
    """One response: (status, headers, body, chunked). A chunked body comes back de-chunked; a
    reply to HEAD has none, whatever its framing says."""
    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = s.recv(65536)
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
    if head_only:
        return status, headers, b"", headers.get("transfer-encoding") == "chunked"
    if headers.get("transfer-encoding") == "chunked":
        data = rest
        while b"\r\n0\r\n\r\n" not in data and not data.startswith(b"0\r\n\r\n"):
            chunk = s.recv(65536)
            if not chunk:
                raise EOFError("closed inside a chunked body")
            data += chunk
        body = b""
        while True:
            size_line, _, data = data.partition(b"\r\n")
            size = int(size_line.split(b";")[0], 16)
            if size == 0:
                break
            body += data[:size]
            data = data[size + 2:]
        return status, headers, body, True
    n = int(headers.get("content-length", "0"))
    body = rest
    while len(body) < n:
        chunk = s.recv(min(65536, n - len(body)))
        if not chunk:
            break
        body += chunk
    return status, headers, body, False


def get(path, extra=b"", method="GET"):
    s = connect()
    s.sendall(f"{method} {path} HTTP/1.1\r\nHost: x\r\nConnection: close\r\n".encode() + extra + b"\r\n")
    r = read_response(s, head_only=method == "HEAD")
    s.close()
    return r


def decode(headers, body):
    coding = headers.get("content-encoding")
    if coding == "gzip":
        return gzip.decompress(body)
    if coding == "br":
        if brotli is None:
            return None
        return brotli.decompress(body)
    return body


def check(name, cond):
    print(f"{'ok ' if cond else 'FAIL'} {name}")
    return cond


results = []
BOTH = b"Accept-Encoding: gzip, br\r\n"

# the plain forms, to compare with: a JSON that fits the slab, one that streams, a feed, a declared length
st, hd, plain_json, ch = get("/json/big?n=200")
results.append(check("GET /json/big?n=200 without Accept-Encoding -> plain, no content-encoding, no vary",
                     st == 200 and "content-encoding" not in hd and "vary" not in hd and plain_json.startswith(b"[") and not ch))
st, hd, plain_big, ch = get("/json/big")
results.append(check("GET /json/big plain -> streams (chunked)", st == 200 and ch and len(plain_big) > 40000))
st, hd, plain_stream, ch = get("/zip/stream?n=3000")
results.append(check("GET /zip/stream plain -> chunked text", st == 200 and ch and plain_stream.endswith(b"line 3000 of 3000\n")))
st, hd, plain_declared, ch = get("/zip/declared?n=100000")
results.append(check("GET /zip/declared plain -> content-length 100000", st == 200 and not ch and len(plain_declared) == 100000))

# br and gzip on the buffered JSON: one message with the exact coded length
st, hd, body, ch = get("/json/big?n=200", BOTH)
results.append(check("gzip, br -> content-encoding br (preferred on a tie), vary accept-encoding",
                     st == 200 and hd.get("content-encoding") == "br" and hd.get("vary") == "accept-encoding"))
results.append(check("  content-length is the coded length, not chunked", not ch and hd.get("content-length") == str(len(body))))
results.append(check("  a third of the plain body or less", len(body) * 3 <= len(plain_json)))
if brotli:
    results.append(check("  decodes to the plain body", decode(hd, body) == plain_json))
else:
    print("skip brotli decoding: no brotli module (pip install brotli); the framing was checked")
st, hd, body, ch = get("/json/big?n=200", b"Accept-Encoding: gzip\r\n")
results.append(check("gzip alone -> gzip, decodes to the plain body",
                     hd.get("content-encoding") == "gzip" and not ch and decode(hd, body) == plain_json))
results.append(check("  a gzip member: the magic bytes", body[:2] == b"\x1f\x8b"))
st, hd, body, ch = get("/json/big?n=200", b"Accept-Encoding: br;q=0.5, gzip;q=0.9\r\n")
results.append(check("br;q=0.5, gzip;q=0.9 -> gzip, the higher q", hd.get("content-encoding") == "gzip" and decode(hd, body) == plain_json))
st, hd, body, ch = get("/json/big?n=200", b"Accept-Encoding: br;q=0, gzip;q=0\r\n")
results.append(check("both refused -> plain, vary still set", "content-encoding" not in hd and hd.get("vary") == "accept-encoding" and body == plain_json))
st, hd, body, ch = get("/json/big?n=200", b"Accept-Encoding: deflate\r\n")
results.append(check("deflate only -> plain", "content-encoding" not in hd and body == plain_json))
st, hd, body, ch = get("/json/big?n=200", b"Accept-Encoding: *\r\n")
results.append(check("* -> br", hd.get("content-encoding") == "br"))
st, hd, body, ch = get("/json/big?n=200", b"Accept-Encoding: identity, *;q=0\r\n")
results.append(check("identity, *;q=0 -> plain", "content-encoding" not in hd and body == plain_json))
st, hd, body, ch = get("/json/big?n=200", b"Accept-Encoding: GZIP\r\n")
results.append(check("GZIP (any case) -> gzip", hd.get("content-encoding") == "gzip"))

# the gates
st, hd, body, ch = get("/zip/health", BOTH)
results.append(check("a two-byte body -> below min_bytes: as it is", st == 200 and "content-encoding" not in hd and body == b"ok"))
st, hd, body, ch = get("/json/big?n=200", BOTH, method="HEAD")
results.append(check("HEAD -> no coding, no body", st == 200 and "content-encoding" not in hd and body == b""))
st, hd, body, ch = get("/health", BOTH)
results.append(check("an endpoint without the middleware -> never coded, no vary", "content-encoding" not in hd and "vary" not in hd))
st, hd, body, ch = get("/zip/raw?n=4096", BOTH)
results.append(check("a body written into the slab directly (ioxd_reserve) -> coded like any other",
                     st == 200 and hd.get("content-encoding") == "br" and (brotli is None or len(decode(hd, body)) == 4096)))
st, hd, body, ch = get("/zip/raw?n=4096", b"Accept-Encoding: gzip\r\n")
results.append(check("  with gzip, decoded", hd.get("content-encoding") == "gzip" and len(decode(hd, body)) == 4096))

# streams: coded flush by flush, chunked, whatever the handler declared
st, hd, body, ch = get("/json/big", BOTH)
results.append(check("a JSON that streams -> br, chunked, decodes whole",
                     st == 200 and hd.get("content-encoding") == "br" and ch and (brotli is None or decode(hd, body) == plain_big)))
st, hd, body, ch = get("/json/big", b"Accept-Encoding: gzip\r\n")
results.append(check("  with gzip", hd.get("content-encoding") == "gzip" and ch and decode(hd, body) == plain_big))
st, hd, body, ch = get("/zip/stream?n=3000", b"Accept-Encoding: gzip\r\n")
results.append(check("a streamed chunked body -> gzip, chunked, decodes whole",
                     st == 200 and hd.get("content-encoding") == "gzip" and ch and decode(hd, body) == plain_stream))
st, hd, body, ch = get("/zip/stream?n=3000", BOTH)
results.append(check("the same with br", hd.get("content-encoding") == "br" and ch and (brotli is None or decode(hd, body) == plain_stream)))
st, hd, body, ch = get("/zip/declared?n=100000", b"Accept-Encoding: gzip\r\n")
results.append(check("a declared length that streams -> the declaration dropped, chunked, decodes to 100000 bytes",
                     st == 200 and hd.get("content-encoding") == "gzip" and ch and "content-length" not in hd
                     and decode(hd, body) == plain_declared))
st, hd, body, ch = get("/zip/declared?n=100000", BOTH)
results.append(check("  with br", hd.get("content-encoding") == "br" and ch and (brotli is None or decode(hd, body) == plain_declared)))
st, hd, body, ch = get("/zip/raw?n=4096", BOTH)
results.append(check("a body that fit the slab, written in place -> one message, the coded length",
                     hd.get("content-encoding") == "br" and not ch and hd.get("content-length") == str(len(body))))
st, hd, body, ch = get("/zip/stream?n=200000", b"Accept-Encoding: gzip\r\n")
results.append(check("a 3 MB stream -> gzip, decodes whole", ch and len(decode(hd, body)) == len(b"".join(f"line {i} of 200000\n".encode() for i in range(1, 200001)))))

# a handler's own flush moves the feed: /stream flushes nothing on purpose, so /zip/declared's four flushes stand in
s = connect()
s.sendall(b"GET /zip/declared?n=100000 HTTP/1.1\r\nHost: x\r\nConnection: close\r\nAccept-Encoding: gzip\r\n\r\n")
first = s.recv(65536)
s.settimeout(5)
rest = b""
while True:
    try:
        chunk = s.recv(65536)
    except socket.timeout:
        break
    if not chunk:
        break
    rest += chunk
s.close()
results.append(check("the first recv of a coded stream carries the head and a first chunk, not the whole body",
                     first.startswith(b"HTTP/1.1 200") and len(first) < len(first + rest)))

# keep-alive: coded and plain replies pipelined on one connection, in order
def parse_all(data, n):
    """n responses laid end to end: [(headers, body)], chunked bodies de-chunked."""
    out = []
    for _ in range(n):
        head, _, data = data.partition(b"\r\n\r\n")
        headers = {}
        for line in head.split(b"\r\n")[1:]:
            k, _, v = line.partition(b": ")
            headers[k.decode().lower()] = v.decode()
        if headers.get("transfer-encoding") == "chunked":
            body = b""
            while True:
                size_line, _, data = data.partition(b"\r\n")
                size = int(size_line.split(b";")[0], 16)
                if size == 0:
                    data = data[2:]
                    break
                body += data[:size]
                data = data[size + 2:]
        else:
            length = int(headers.get("content-length", "0"))
            body, data = data[:length], data[length:]
        out.append((headers, body))
    return out


s = connect()
s.sendall(b"GET /json/big?n=200 HTTP/1.1\r\nHost: x\r\nAccept-Encoding: gzip\r\n\r\n"
          b"GET /json/big?n=200 HTTP/1.1\r\nHost: x\r\n\r\n"
          b"GET /zip/stream?n=100 HTTP/1.1\r\nHost: x\r\nAccept-Encoding: gzip\r\n\r\n"
          b"GET /health HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
data = b""
while True:
    chunk = s.recv(65536)
    if not chunk:
        break
    data += chunk
s.close()
(h1, b1), (h2, b2), (h3, b3), (h4, b4) = parse_all(data, 4)
r1, r2, r3, r4 = (0, h1, b1), (0, h2, b2), (0, h3, b3), (0, h4, b4)
results.append(check("four pipelined requests: gzip, plain, gzip (a feed that fit the slab), plain, each whole and in order",
                     r1[1].get("content-encoding") == "gzip" and decode(r1[1], r1[2]) == plain_json
                     and "content-encoding" not in r2[1] and r2[2] == plain_json
                     and r3[1].get("content-encoding") == "gzip" and decode(r3[1], r3[2]).endswith(b"line 100 of 100\n")
                     and r4[2] == b"ok"))

print("all passed" if all(results) else "FAILURES")
sys.exit(0 if all(results) else 1)
