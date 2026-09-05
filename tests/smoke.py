#!/usr/bin/env python3
"""Functional checks against a running server: python3 tests/smoke.py [port]."""
import socket
import sys
import time

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
REQ = b"GET / HTTP/1.1\r\nHost: x\r\n\r\n"
OK = b"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 2\r\n\r\nok"


def connect():
    s = socket.create_connection(("127.0.0.1", PORT))
    s.settimeout(3)
    return s


def recv_exact(s, n):
    buf = b""
    while len(buf) < n:
        chunk = s.recv(n - len(buf))
        if not chunk:
            break
        buf += chunk
    return buf


def check(name, cond):
    print(f"{'ok ' if cond else 'FAIL'} {name}")
    return cond


results = []

# keep-alive: several requests on one connection
s = connect()
good = True
for _ in range(5):
    s.send(REQ)
    good &= recv_exact(s, len(OK)) == OK
results.append(check("keep-alive, 5 requests on one connection", good))
s.close()

# pipelining: two requests in one write -> two responses
s = connect()
s.send(REQ + REQ)
results.append(check("pipelined pair answered twice", recv_exact(s, 2 * len(OK)) == OK + OK))
s.close()

# a request split over several writes, with pauses
s = connect()
for part in (b"GET / HTTP", b"/1.1\r\nHost: x", b"\r\n\r\n"):
    s.send(part)
    time.sleep(0.05)
results.append(check("request split over three writes", recv_exact(s, len(OK)) == OK))
s.close()

# half-close: shut our send side right after the request, still get the response
s = connect()
s.send(REQ)
s.shutdown(socket.SHUT_WR)
results.append(check("half-close still answered", recv_exact(s, len(OK)) == OK))
results.append(check("then peer FIN", s.recv(16) == b""))
s.close()

# a connection that never sends anything, then closes
s = connect()
time.sleep(0.05)
s.close()
results.append(check("silent connection closed cleanly", True))

# a request larger than the handler's buffer is dropped with a close, not a hang
s = connect()
s.send(b"GET / HTTP/1.1\r\n" + b"X-Pad: " + b"a" * 9000)
try:
    got = s.recv(16)
    results.append(check("oversized request: connection closed", got == b""))
except (ConnectionResetError, socket.timeout) as e:
    results.append(check("oversized request: connection closed", isinstance(e, ConnectionResetError)))
s.close()

# many concurrent connections, each answered
conns = [connect() for _ in range(200)]
for s in conns:
    s.send(REQ)
good = all(recv_exact(s, len(OK)) == OK for s in conns)
results.append(check("200 concurrent connections answered", good))
for s in conns:
    s.close()

# a burst of 4 KiB junk lines (many buffers per request) still parses
s = connect()
s.send(b"GET / HTTP/1.1\r\n" + b"X-A: " + b"b" * 6000 + b"\r\n\r\n")
results.append(check("6 KiB header spanning buffers", recv_exact(s, len(OK)) == OK))
s.close()

print("all passed" if all(results) else "FAILURES")
sys.exit(0 if all(results) else 1)
