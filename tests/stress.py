#!/usr/bin/env python3
"""Pressure on the ugly paths: python3 tests/stress.py [port].

`make check` runs it against the default build; `make check-tiny` runs it against a build with
-DBUF_COUNT=8 -DBUF_SIZE=64 -DRX_QUEUE=4, which starves the provided buffer group on every
request and pauses the per-connection recv at the first stall.

At shutdown the server must report "0 still open" on every worker.
"""
import os
import socket
import struct
import sys
import threading
import time

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
REQ = b"GET /health HTTP/1.1\r\nHost: x\r\n\r\n"
OK = b"HTTP/1.1 200 OK\r\ncontent-type: text/plain\r\ncontent-length: 2\r\nserver: ioxd\r\n\r\nok"


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
big = b"GET /health HTTP/1.1\r\nHost: x\r\nX-A: " + b"a" * 3000 + b"\r\n\r\n"
for s in conns:
    s.send(big)
good = all(recv_exact(s, len(OK)) == OK for s in conns)
results.append(check(f"{N} clients x 3 KiB request at once (buffer starvation)", good))
for s in conns:
    s.close()
results.append(check("server healthy after starvation", healthy()))

# 2. Flood without ever reading. The server's send parks once our receive buffer and its send
#    buffer are full; data keeps arriving until the connection's recv queue reaches its mark, at
#    which point the server pauses the multishot and the socket's window holds us. Our sendall
#    then stalls (the server stopped reading); closing with unread data RSTs the socket, which
#    fails the parked send and lets the handler finish. Nothing may wedge.
FLOOD_LIMIT = 20                                  # seconds: the send timeout above, with room
s = connect(timeout=3)
s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
t0 = time.time()
outcome = "sent all"                              # what must not happen: the whole flood taken in
try:
    s.sendall(REQ * 400000)                       # ~11 MB of requests -> ~28 MB of answers
except socket.timeout:
    outcome = "stalled (server stopped reading)"
except (ConnectionResetError, BrokenPipeError):
    outcome = "reset by server"
took = time.time() - t0
s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))   # RST on close
s.close()
# Taking it all would mean ~28 MB of answers buffered for a client that reads none; the server
# must end our input instead, and decide it while we are still sending, not eventually.
results.append(check(f"flood without reading: {outcome} after {took:.1f}s",
                     outcome != "sent all" and took < FLOOD_LIMIT))
results.append(check("server healthy after flood", healthy()))

# 2b. A streamed echo: the handler sends each piece while the rest of the body is still arriving,
#     so its recv queue fills while it is parked on a send. The recv is paused at the mark and
#     re-armed as the handler drains - through a queue of 4 in the tiny build, under starvation -
#     and every byte comes back, in order. Sent from a thread: the echo cannot be read after the
#     upload, since a server that had to buffer it all would be the flood above.
def dechunk(data):
    out = b""
    while True:
        line, _, data = data.partition(b"\r\n")
        n = int(line.split(b";")[0], 16)
        if n == 0:
            return out
        out += data[:n]
        data = data[n + 2:]


def streamed_echo(body, chunked):
    s = connect(timeout=30)
    if chunked:
        head = b"POST /echo-stream HTTP/1.1\r\nHost: x\r\nConnection: close\r\nTransfer-Encoding: chunked\r\n\r\n"
        wire = b"".join(b"%x\r\n%s\r\n" % (len(body[i:i + 65536]), body[i:i + 65536])
                        for i in range(0, len(body), 65536)) + b"0\r\n\r\n"
    else:
        head = b"POST /echo-stream HTTP/1.1\r\nHost: x\r\nConnection: close\r\nContent-Length: %d\r\n\r\n" % len(body)
        wire = body
    sender = threading.Thread(target=lambda: s.sendall(head + wire))
    sender.start()
    data = b""
    while True:
        try:
            piece = s.recv(1 << 20)
        except socket.timeout:
            break
        if not piece:
            break
        data += piece
    sender.join()
    s.close()
    reply_head, _, rest = data.partition(b"\r\n\r\n")
    if not reply_head.startswith(b"HTTP/1.1 200"):
        return reply_head[:40]
    return dechunk(rest) if b"transfer-encoding: chunked" in reply_head.lower() else rest


body = os.urandom(1 << 20)
t0 = time.time()
back = streamed_echo(body, chunked=False)
results.append(check(f"1 MiB streamed echo, Content-Length, in {time.time() - t0:.1f}s (recv paused at the mark, re-armed as it drains)",
                     back == body))
t0 = time.time()
back = streamed_echo(body[:256 << 10], chunked=True)
results.append(check(f"256 KiB streamed echo, chunked, in {time.time() - t0:.1f}s", back == body[:256 << 10]))
results.append(check("server healthy after streamed echoes", healthy()))

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
