#!/usr/bin/env python3
"""The static directory the fixture serves under /static: python3 tests/static.py [port] [dir].

Fills dir (the fixture opened it empty), then checks what comes back: types, exact bytes, the
.br/.gz twins by Accept-Encoding, conditional requests, byte ranges, the name checks, a file
larger than the slab (one message from where it is) and one past the keep limit (streamed), the
same over TLS, and that a file replaced on disk is served new at once.
"""
import os
import socket
import ssl
import sys
import time

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
WWW = sys.argv[2] if len(sys.argv) > 2 else "obj/check/www"
TLS_PORT = PORT + 2


def connect(port=PORT, tls=False, timeout=5):
    for attempt in range(20):
        try:
            s = socket.create_connection(("127.0.0.1", port))
            break
        except OSError:
            if attempt == 19:
                raise
            time.sleep(0.05)
    s.settimeout(timeout)
    if tls:
        ctx = ssl.create_default_context()
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        s = ctx.wrap_socket(s, server_hostname="localhost")
    return s


def read_response(s):
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
    n = int(headers.get("content-length", "0"))
    body = rest
    while len(body) < n:
        chunk = s.recv(min(65536, n - len(body)))
        if not chunk:
            break
        body += chunk
    return status, headers, body


def request(path, method="GET", extra=b"", tls=False):
    s = connect(TLS_PORT if tls else PORT, tls=tls)
    s.sendall(f"{method} {path} HTTP/1.1\r\nHost: x\r\nConnection: close\r\n".encode() + extra + b"\r\n")
    r = read_response(s)
    s.close()
    return r


def get(path, extra=b"", tls=False):
    return request(path, "GET", extra, tls)


def port_open(port):
    try:
        socket.create_connection(("127.0.0.1", port), timeout=1).close()
        return True
    except OSError:
        return False


def check(name, cond):
    print(f"{'ok ' if cond else 'FAIL'} {name}")
    return cond


def put(name, data):
    path = os.path.join(WWW, name)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


def replace(name, data):
    """A new file of the same name, moved into place: a new inode, as a deploy makes."""
    path = os.path.join(WWW, name)
    with open(path + ".new", "wb") as f:
        f.write(data)
    os.replace(path + ".new", path)


results = []
rnd = os.urandom

# --- the directory ---
css = b"body { margin: 0 }\n"
css_br = b"BR-TWIN-" + rnd(40)
css_gz = b"GZ-TWIN-" + rnd(30)
put("site.css", css)
put("site.css.br", css_br)
put("site.css.gz", css_gz)
put("app.js", b"console.log('hi')\n")
put("index.html", b"<h1>root</h1>\n")
put("sub/page.html", b"<p>page</p>\n")
put("sub/index.html", b"<p>sub index</p>\n")
put("sub2/empty.txt", b"")
put(".secret", b"hidden\n")
put("a b.txt", b"space\n")
put("data.unknownext", b"\x00\x01\x02")
mid = rnd(40 * 1024)                                   # kept, and larger than the slab: one message from where it is
put("mid.bin", mid)
big = rnd(200 * 1024)                                  # past the 64 KB keep limit: streamed
put("big.bin", big)

# --- types and headers ---
st, hd, body = get("/static/site.css")
results.append(check("GET /static/site.css -> 200, the bytes", st == 200 and body == css))
results.append(check("  content-type text/css", hd.get("content-type") == "text/css"))
results.append(check("  content-length", hd.get("content-length") == str(len(css))))
results.append(check("  etag, last-modified, accept-ranges, cache-control",
                     hd.get("etag", "").startswith('"') and hd.get("last-modified", "").endswith(" GMT")
                     and hd.get("accept-ranges") == "bytes" and hd.get("cache-control") == "max-age=60"))
results.append(check("  vary: accept-encoding (the file has twins)", hd.get("vary") == "accept-encoding"))
results.append(check("  no content-encoding without accept-encoding", "content-encoding" not in hd))
etag = hd["etag"]
modified = hd["last-modified"]

st, hd, body = get("/static/app.js")
results.append(check("GET /static/app.js -> text/javascript, no vary (no twins)",
                     st == 200 and hd.get("content-type") == "text/javascript" and "vary" not in hd))
st, hd, body = get("/static/sub/page.html")
results.append(check("GET /static/sub/page.html -> a nested file, text/html",
                     st == 200 and body == b"<p>page</p>\n" and hd.get("content-type") == "text/html; charset=utf-8"))
st, hd, body = get("/static/data.unknownext")
results.append(check("an unknown extension -> application/octet-stream",
                     st == 200 and hd.get("content-type") == "application/octet-stream" and body == b"\x00\x01\x02"))
st, hd, body = get("/static/sub2/empty.txt")
results.append(check("an empty file -> 200, content-length 0", st == 200 and hd.get("content-length") == "0" and body == b""))

# --- twins by Accept-Encoding ---
def encoded(accept):
    st, hd, body = get("/static/site.css", b"Accept-Encoding: " + accept + b"\r\n")
    return st, hd.get("content-encoding"), hd.get("content-type"), body

results.append(check("br;q=1, gzip;q=0.8 -> the .br twin, content-encoding br, the base type",
                     encoded(b"br;q=1, gzip;q=0.8") == (200, "br", "text/css", css_br)))
results.append(check("gzip -> the .gz twin", encoded(b"gzip") == (200, "gzip", "text/css", css_gz)))
results.append(check("gzip, br -> br (the smaller wins a tie)", encoded(b"gzip, br") == (200, "br", "text/css", css_br)))
results.append(check("br;q=0, gzip -> br refused, gzip", encoded(b"br;q=0, gzip") == (200, "gzip", "text/css", css_gz)))
results.append(check("br;q=0.5, gzip;q=0.9 -> gzip, the higher q", encoded(b"br;q=0.5, gzip;q=0.9") == (200, "gzip", "text/css", css_gz)))
results.append(check("* -> any coding: br", encoded(b"*") == (200, "br", "text/css", css_br)))
results.append(check("*;q=0 -> nothing taken: the file itself", encoded(b"*;q=0") == (200, None, "text/css", css)))
results.append(check("identity -> the file itself", encoded(b"identity") == (200, None, "text/css", css)))
results.append(check("BR (any case) -> br", encoded(b"BR") == (200, "br", "text/css", css_br)))
results.append(check("deflate -> not served: the file itself", encoded(b"deflate") == (200, None, "text/css", css)))
st, hd, body = get("/static/app.js", b"Accept-Encoding: br, gzip\r\n")
results.append(check("a file without twins -> as it is, whatever is accepted", st == 200 and "content-encoding" not in hd))

# --- names ---
st, hd, body = get("/static/missing.css")
results.append(check("GET /static/missing.css -> 404, no body", st == 404 and body == b""))
st, hd, body = get("/static/../server.c")
results.append(check("GET /static/../server.c -> 404 (a dot segment is refused)", st == 404))
st, hd, body = get("/static/sub/%2e%2e/%2e%2e/server.c")
results.append(check("GET /static/sub/%2e%2e/.. -> 404 (decoded first, then refused)", st == 404))
st, hd, body = get("/static/.secret")
results.append(check("GET /static/.secret -> 404 (hidden)", st == 404))
st, hd, body = get("/static/a%20b.txt")
results.append(check("GET /static/a%20b.txt -> the name percent-decoded", st == 200 and body == b"space\n"))
st, hd, body = get("/static/site.css%00")
results.append(check("GET /static/site.css%00 -> 404 (a NUL is refused)", st == 404))
st, hd, body = get("/static/")
results.append(check("GET /static/ -> index.html", st == 200 and body == b"<h1>root</h1>\n"))
st, hd, body = get("/static")
results.append(check("GET /static -> index.html (the mount itself)", st == 200 and body == b"<h1>root</h1>\n"))
st, hd, body = get("/static/sub/")
results.append(check("GET /static/sub/ -> the directory's index", st == 200 and body == b"<p>sub index</p>\n"))
st, hd, body = get("/static/sub")
results.append(check("GET /static/sub -> the same, without the slash", st == 200 and body == b"<p>sub index</p>\n"))
st, hd, body = get("/static/sub2")
results.append(check("GET /static/sub2 -> 404 (a directory without an index)", st == 404))
st, hd, body = get("/static//sub//page.html")
results.append(check("GET /static//sub//page.html -> empty segments collapse", st == 200 and body == b"<p>page</p>\n"))
st, hd, body = get("/staticx/site.css")
results.append(check("GET /staticx/site.css -> not under the mount: the fixture's own JSON 404",
                     st == 404 and body == b'{"error":"not found"}'))
st, hd, body = get("/shell")
results.append(check("GET /shell -> ioxd_static_file: index.html whatever the path", st == 200 and body == b"<h1>root</h1>\n"))
st, hd, body = request("/static/site.css", "POST", b"Content-Length: 0\r\n")
results.append(check("POST /static/site.css -> 405, allow: GET, HEAD", st == 405 and hd.get("allow") == "GET, HEAD"))

# --- HEAD and conditional requests ---
st, hd, body = request("/static/site.css", "HEAD")
results.append(check("HEAD /static/site.css -> 200, the length, no body",
                     st == 200 and hd.get("content-length") == str(len(css)) and body == b""))
st, hd, body = get("/static/site.css", b"If-None-Match: " + etag.encode() + b"\r\n")
results.append(check("If-None-Match with the etag -> 304, no body, the etag", st == 304 and body == b"" and hd.get("etag") == etag))
st, hd, body = get("/static/site.css", b'If-None-Match: "other", W/' + etag.encode() + b"\r\n")
results.append(check("If-None-Match: a list with the weak form -> 304", st == 304))
st, hd, body = get("/static/site.css", b"If-None-Match: *\r\n")
results.append(check("If-None-Match: * -> 304", st == 304))
st, hd, body = get("/static/site.css", b'If-None-Match: "nope"\r\n')
results.append(check("If-None-Match with another tag -> 200", st == 200 and body == css))
st, hd, body = get("/static/site.css", b"If-Modified-Since: " + modified.encode() + b"\r\n")
results.append(check("If-Modified-Since: the last-modified -> 304", st == 304))
st, hd, body = get("/static/site.css", b"If-Modified-Since: Sun, 06 Nov 1994 08:49:37 GMT\r\n")
results.append(check("If-Modified-Since: long ago -> 200", st == 200 and body == css))
st, hd, body = get("/static/site.css", b'If-None-Match: "nope"\r\nIf-Modified-Since: ' + modified.encode() + b"\r\n")
results.append(check("If-None-Match present -> If-Modified-Since ignored: 200", st == 200))

# --- byte ranges ---
st, hd, body = get("/static/mid.bin", b"Range: bytes=0-9\r\n")
results.append(check("Range: bytes=0-9 -> 206, the first ten, content-range",
                     st == 206 and body == mid[:10] and hd.get("content-range") == f"bytes 0-9/{len(mid)}"))
st, hd, body = get("/static/mid.bin", b"Range: bytes=-5\r\n")
results.append(check("Range: bytes=-5 -> the last five", st == 206 and body == mid[-5:] and hd.get("content-range") == f"bytes {len(mid)-5}-{len(mid)-1}/{len(mid)}"))
st, hd, body = get("/static/mid.bin", b"Range: bytes=40000-\r\n")
results.append(check("Range: bytes=40000- -> from there to the end", st == 206 and body == mid[40000:]))
st, hd, body = get("/static/mid.bin", b"Range: bytes=100-99999999\r\n")
results.append(check("Range: an end past the file -> clamped", st == 206 and body == mid[100:]))
st, hd, body = get("/static/mid.bin", f"Range: bytes={len(mid)}-\r\n".encode())
results.append(check("Range: a start past the end -> 416, content-range bytes */n",
                     st == 416 and hd.get("content-range") == f"bytes */{len(mid)}"))
st, hd, body = get("/static/mid.bin", b"Range: bytes=0-9, 20-29\r\n")
results.append(check("Range: several -> ignored, the whole file", st == 200 and body == mid))
st, hd, body = get("/static/mid.bin", b"Range: items=0-9\r\n")
results.append(check("Range: another unit -> ignored", st == 200 and body == mid))
st, hd, body = get("/static/mid.bin", b'Range: bytes=0-9\r\nIf-Range: "another-version"\r\n')
results.append(check("If-Range naming another version -> the whole file", st == 200 and body == mid))
st, hd, body = get("/static/site.css", b"Range: bytes=0-3\r\nAccept-Encoding: br\r\n")
results.append(check("a range of the br twin -> its first four bytes", st == 206 and body == css_br[:4] and hd.get("content-encoding") == "br"))

# --- large files: kept and sent from where they are, or streamed ---
st, hd, body = get("/static/mid.bin")
results.append(check("GET /static/mid.bin (40 KB, kept) -> exact bytes", st == 200 and body == mid))
st, hd, body = get("/static/big.bin")
results.append(check("GET /static/big.bin (200 KB, streamed) -> exact bytes", st == 200 and body == big))
st, hd, body = request("/static/big.bin", "HEAD")
results.append(check("HEAD /static/big.bin -> the length, no body", st == 200 and hd.get("content-length") == str(len(big)) and body == b""))
st, hd, body = get("/static/big.bin", b"Range: bytes=100000-100999\r\n")
results.append(check("a range of the streamed file", st == 206 and body == big[100000:101000]))
st, hd, body = get("/static/big.bin", b"If-None-Match: " + hd.get("etag", "").encode() + b"\r\n") if hd.get("etag") else (0, {}, b"")
results.append(check("a 304 for the streamed file", st == 304))
if port_open(TLS_PORT):
    st, hd, body = get("/static/mid.bin", tls=True)
    results.append(check("GET /static/mid.bin over TLS -> exact bytes (kTLS, one message)", st == 200 and body == mid))
    st, hd, body = get("/static/big.bin", tls=True)
    results.append(check("GET /static/big.bin over TLS -> exact bytes", st == 200 and body == big))
    st, hd, body = get("/static/site.css", b"Accept-Encoding: br\r\n", tls=True)
    results.append(check("the br twin over TLS", st == 200 and body == css_br))
else:
    print(f"skip TLS: nothing listening on {TLS_PORT} (a TLS=0 build?)")

# --- the disk moves under the cache ---
css2 = b"body { margin: 1 }\n"                           # the same length: only the inode and the time tell
assert len(css2) == len(css)
replace("site.css", css2)
st, hd, body = get("/static/site.css")
results.append(check("a replaced file (same length) -> the new bytes at once", st == 200 and body == css2))
results.append(check("  a new etag", hd.get("etag") not in (None, etag)))
css_br2 = b"BR-TWIN2" + rnd(40)
replace("site.css.br", css_br2)
replace("site.css", css)
st, hd, body = get("/static/site.css", b"Accept-Encoding: br\r\n")
results.append(check("the file and its twin replaced -> the new twin", st == 200 and body == css_br2))
with open(os.path.join(WWW, "app.js"), "ab") as f:                 # rewritten in place: a new size and time
    f.write(b"// more\n")
st, hd, body = get("/static/app.js")
results.append(check("a file appended to in place -> the new bytes", st == 200 and body == b"console.log('hi')\n// more\n"))
os.unlink(os.path.join(WWW, "app.js"))
st, hd, body = get("/static/app.js")
results.append(check("a deleted file -> 404", st == 404))
put("app.js", b"back\n")
st, hd, body = get("/static/app.js")
results.append(check("a file created again -> 200", st == 200 and body == b"back\n"))
mid2 = rnd(40 * 1024)
replace("mid.bin", mid2)
st, hd, body = get("/static/mid.bin")
results.append(check("a replaced 40 KB file -> the new bytes", st == 200 and body == mid2))

# --- keep-alive: many files on one connection, pipelined, in order ---
def bodies(data, n):
    """The bodies of n responses laid end to end in data."""
    out = []
    for _ in range(n):
        head, _, data = data.partition(b"\r\n\r\n")
        length = 0
        for line in head.split(b"\r\n"):
            k, _, v = line.partition(b": ")
            if k.lower() == b"content-length":
                length = int(v)
        out.append(data[:length])
        data = data[length:]
    return out


s = connect()
names = ["site.css", "mid.bin", "sub/page.html", "big.bin", "site.css"]
for i, name in enumerate(names):
    close = "close" if i == len(names) - 1 else "keep-alive"
    s.sendall(f"GET /static/{name} HTTP/1.1\r\nHost: x\r\nConnection: {close}\r\n\r\n".encode())
data = b""
while True:
    chunk = s.recv(65536)
    if not chunk:
        break
    data += chunk
s.close()
want = [css, mid2, b"<p>page</p>\n", big, css]
results.append(check("five files pipelined on one connection, kept and streamed among them, each whole and in order",
                     bodies(data, len(names)) == want))

print("all passed" if all(results) else "FAILURES")
sys.exit(0 if all(results) else 1)
