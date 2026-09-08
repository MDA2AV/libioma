#!/usr/bin/env python3
"""The pipe API through tests/pipe-server.c, a line echo: whole lines, a line split across
sends, two lines in one packet, a quit, and a line too long for the pipe's buffer."""
import socket, sys, time

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8100


def connect():
    for attempt in range(20):
        try:
            s = socket.create_connection(("127.0.0.1", PORT), timeout=5)
            return s
        except OSError:
            if attempt == 19:
                raise
            time.sleep(0.05)


def read_until(s, want):
    data = b""
    while len(data) < want:
        chunk = s.recv(65536)
        if not chunk:
            break
        data += chunk
    return data


def read_to_close(s):
    data = b""
    while True:
        try:
            chunk = s.recv(65536)
        except socket.timeout:
            break
        if not chunk:
            break
        data += chunk
    return data


results = []


def check(name, cond):
    print(f"{'ok ' if cond else 'FAIL'} {name}")
    results.append(cond)


s = connect()
s.sendall(b"hello\n")
results and None
check("a line comes back echoed", read_until(s, 12) == b"echo: hello\n")
s.sendall(b"ab")
time.sleep(0.05)
s.sendall(b"c\n")
check("a line split across sends is echoed whole", read_until(s, 10) == b"echo: abc\n")
s.sendall(b"x\ny\n")
check("two lines in one packet -> two echoes", read_until(s, 16) == b"echo: x\necho: y\n")
s.sendall(b"quit\n")
check("quit closes the connection", read_to_close(s) == b"")
s.close()

s = connect()
s.sendall(b"z" * 20000)
check("a line longer than the pipe's buffer closes the connection", read_to_close(s) == b"")
s.close()

s = connect()
big = b"w" * 6000 + b"\n"
s.sendall(big)
check("a 6 KB line, spanning kernel buffers, is echoed whole", read_until(s, 6 + len(big)) == b"echo: " + big)
s.close()

failed = results.count(False)
print("all passed" if not failed else f"{failed} FAILED")
sys.exit(1 if failed else 0)
