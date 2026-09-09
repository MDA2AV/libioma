#!/usr/bin/env python3
"""The pipe API through tests/pipe-server.c, a line echo: whole lines, a line split across
sends, two lines in one packet, bytes copied into a buffer of the handler's own, bytes kept where
the kernel left them, a quit, and a line too long for the pipe's buffer."""
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
    """Everything until the server closes: (data, closed). closed is False when the read timed out
    instead - the connection is still open, which is a hang, not a close, and an assertion about a
    close must not pass on one."""
    data = b""
    while True:
        try:
            chunk = s.recv(65536)
        except socket.timeout:
            return data, False
        except ConnectionResetError:
            return data, True
        if not chunk:
            return data, True
        data += chunk


results = []


def check(name, cond):
    print(f"{'ok ' if cond else 'FAIL'} {name}")
    results.append(cond)


s = connect()
s.sendall(b"hello\n")
check("a line comes back echoed", read_until(s, 12) == b"echo: hello\n")
s.sendall(b"ab")
time.sleep(0.05)
s.sendall(b"c\n")
check("a line split across sends is echoed whole", read_until(s, 10) == b"echo: abc\n")
s.sendall(b"x\ny\n")
check("two lines in one packet -> two echoes", read_until(s, 16) == b"echo: x\necho: y\n")

# copy: the bytes after the command line, read into the handler's own buffer and sent back
s.sendall(b"copy 5\nhello")
check("copy N -> the next N bytes, copied out and sent back", read_until(s, 12) == b"copy: hello\n")

# keep: the bytes are waited for where the kernel left them, however many receives that takes,
# then kept - which consumes them, so the line after is read fresh rather than seen twice
s.sendall(b"hold 9\n")
for part in (b"abc", b"def", b"ghi"):
    s.sendall(part)
    time.sleep(0.05)
check("hold N -> N bytes gathered over three receives, kept and written back",
      read_until(s, 16) == b"hold: abcdefghi\n")
s.sendall(b"after\n")
check("the held bytes were consumed: the next line is echoed on its own", read_until(s, 12) == b"echo: after\n")

s.sendall(b"quit\n")
data, closed = read_to_close(s)
check("quit closes the connection", closed and data == b"")
s.close()

s = connect()
s.sendall(b"z" * 20000)
data, closed = read_to_close(s)
check("a line longer than the pipe's buffer closes the connection", closed and data == b"")
s.close()

s = connect()
big = b"w" * 6000 + b"\n"
s.sendall(big)
check("a 6 KB line, spanning kernel buffers, is echoed whole", read_until(s, 6 + len(big)) == b"echo: " + big)
s.close()

failed = results.count(False)
print("all passed" if not failed else f"{failed} FAILED")
sys.exit(1 if failed else 0)
