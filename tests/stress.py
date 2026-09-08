#!/usr/bin/env python3
"""Pressure on the ugly paths: python3 tests/stress.py [port].

Run it against the default build, and against a tiny-buffer build that starves the provided
buffer group on every request and overflows the per-connection queue at the first stall:

    gcc -O2 -g -Wall -Iinclude -Ithird_party/picohttpparser -pthread \\
        -DBUF_COUNT=8 -DBUF_SIZE=64 -DRX_QUEUE=4 \\
        playground/hello/main.c lib/*/*.c lib/io/*.S \\
        third_party/picohttpparser/picohttpparser.c -o ioma-tiny

At shutdown the server must report "0 still open" on every worker.
"""
import socket
import struct
import sys
import time

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
REQ = b"GET /health HTTP/1.1\r\nHost: x\r\n\r\n"
OK = b"HTTP/1.1 200 OK\r\ncontent-type: text/plain\r\ncontent-length: 2\r\nserver: ioma\r\n\r\nok"


def connect(timeout=5):
    # Loopback churn parks thousands of client sockets in TIME_WAIT; once the ephemeral port pool
    # is dry, connect() itself gets RST or EADDRNOTAVAIL. That is a load-generator limit on this
    # host, not the server (which reports "0 still open" every shutdown), so retry briefly.
    deadline = time.time() + 10
    while True:
        try:
            s = socket.create_connection(("127.0.0.1", PORT))
            s.settimeout(timeout)
            return s
        except (ConnectionResetError, OSError):
            if time.time() > deadline:
                raise
            time.sleep(0.02)


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


def healthy():
    s = connect()
    s.send(REQ)
    ok = recv_exact(s, len(OK)) == OK
    s.close()
    return ok


results = []

# 1. Many clients send a 3 KiB request at the same instant. With 8 x 64 B buffers per worker the
#    group is exhausted many times over; every recv parks on -ENOBUFS and must be re-armed as the
#    handlers hand buffers back. Every client still gets its answer.
N = 64
conns = [connect() for _ in range(N)]
big = b"GET /health HTTP/1.1\r\nX-A: " + b"a" * 3000 + b"\r\n\r\n"
for s in conns:
    s.send(big)
good = all(recv_exact(s, len(OK)) == OK for s in conns)
results.append(check(f"{N} clients x 3 KiB request at once (buffer starvation)", good))
for s in conns:
    s.close()
results.append(check("server healthy after starvation", healthy()))

# 2. Flood without ever reading. The server's send parks once our receive buffer and its send
#    buffer are full; data keeps arriving until the connection's recv queue overflows, at which
#    point the server ends its input and cancels the multishot. Our sendall then stalls (the
#    server stopped reading); closing with unread data RSTs the socket, which fails the parked
#    send and lets the handler finish. Nothing may wedge.
s = connect(timeout=3)
s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
t0 = time.time()
try:
    s.sendall(REQ * 400000)                       # ~11 MB of requests -> ~28 MB of answers
    outcome = "sent all"
except socket.timeout:
    outcome = "stalled (server stopped reading)"
except (ConnectionResetError, BrokenPipeError):
    outcome = "reset by server"
s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))   # RST on close
s.close()
results.append(check(f"flood without reading: {outcome} after {time.time() - t0:.1f}s", True))
results.append(check("server healthy after flood", healthy()))

# 3. Reset in the middle of a request: the multishot recv completes with -ECONNRESET.
s = connect()
s.send(b"GET /health HTTP/1.1\r\nHost:")
s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
s.close()
results.append(check("RST mid-request", healthy()))

# 4. Connection churn: 2000 sequential connect / request / close.
t0 = time.time()
good = True
for _ in range(2000):
    s = connect()
    s.send(REQ)
    good &= recv_exact(s, len(OK)) == OK
    s.close()
results.append(check(f"2000 connections churned in {time.time() - t0:.1f}s", good))

# 5. 500 idle connections held open while others are served, then all closed at once.
idle = [connect() for _ in range(500)]
results.append(check("served while 500 connections idle", healthy()))
for s in idle:
    s.close()
time.sleep(0.2)
results.append(check("healthy after mass close", healthy()))

print("all passed" if all(results) else "FAILURES")
sys.exit(0 if all(results) else 1)
