#!/usr/bin/env python3
"""The TLS handoff at the wire level, with tlslite-ng (pip install tlslite-ng): the request sent
in the same TCP write as the client's Finished, once whole and once with its record cut in two
with a pause, so the server has to decrypt what the kernel already delivered and fetch the rest
of a split record from the socket before kernel RX takes over; then a close_notify, and a
corrupted record. Skips itself when tlslite-ng is not installed."""
import socket, sys, time

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8101   # the fixture's TLS port

try:
    from tlslite import TLSConnection, HandshakeSettings
except ImportError:
    print("skip tls_early: tlslite-ng not installed (pip install tlslite-ng)")
    sys.exit(0)


class Held:
    """A socket whose writes can be held back and released in one or more pieces, so several
    TLS records - or half of one - go out in a single TCP write, or with a pause inside."""
    def __init__(self, sock):
        self.sock, self.buf, self.hold = sock, bytearray(), False

    def send(self, data):
        if self.hold:
            self.buf += data
            return len(data)
        return self.sock.send(data)

    def release(self, n=None):
        n = len(self.buf) if n is None else n
        self.sock.sendall(bytes(self.buf[:n]))
        del self.buf[:n]

    def recv(self, n):
        self.release()                           # anything held goes out before we wait
        return self.sock.recv(n)

    def __getattr__(self, name):
        return getattr(self.sock, name)


def settings():
    s = HandshakeSettings()
    s.minVersion = (3, 4)
    s.maxVersion = (3, 4)
    s.cipherNames = ["aes128gcm"]
    return s


def connect():
    raw = socket.create_connection(("127.0.0.1", PORT), timeout=5)
    held = Held(raw)
    held.hold = True                             # the ClientHello is released by the first recv
    conn = TLSConnection(held)
    conn.handshakeClientCert(settings=settings(), serverName="localhost")
    return conn, held                            # the client's Finished is still held


def read_reply(conn):
    data = b""
    while b"\r\n\r\n" not in data:
        data += conn.read()
    head, _, body = data.partition(b"\r\n\r\n")
    n = int([l for l in head.split(b"\r\n") if l.lower().startswith(b"content-length:")][0].split(b":")[1])
    while len(body) < n:
        body += conn.read()
    return head.split(b"\r\n")[0], body


results = []


def check(name, cond):
    print(f"{'ok ' if cond else 'FAIL'} {name}")
    results.append(cond)


REQ = b"GET /health HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"

# 1. the request rides in the same TCP write as the Finished
conn, held = connect()
conn.write(REQ)
held.release()
status, body = read_reply(conn)
check("request in the same write as Finished -> 200 ok", status == b"HTTP/1.1 200 OK" and body == b"ok")
conn.close()

# 2. the same, with the request's record cut in half and a pause: the tail is still in the socket
#    when the server pauses its recv, and it has to fetch it before handing RX to the kernel
conn, held = connect()
conn.write(REQ)
total = len(held.buf)
held.release(total - 10)
time.sleep(0.3)
held.release()
status, body = read_reply(conn)
check("record split with a pause after the Finished -> 200 ok", status == b"HTTP/1.1 200 OK" and body == b"ok")
conn.close()

# 3. two requests: the first whole with the Finished, the second's record split, keep-alive on the first
conn, held = connect()
conn.write(b"GET /health HTTP/1.1\r\nHost: x\r\n\r\n")
conn.write(REQ)
total = len(held.buf)
held.release(total - 7)
time.sleep(0.3)
held.release()
s1, b1 = read_reply(conn)
s2, b2 = read_reply(conn)
check("two early requests, the second's record split -> both answered", b1 == b"ok" and b2 == b"ok")
conn.close()

# 4. a plain exchange, then the client's close_notify: the server sees a control record and closes
conn, held = connect()
held.release()
conn.write(b"GET /health HTTP/1.1\r\nHost: x\r\n\r\n")
status, body = read_reply(conn)
conn.closeSocket = False                         # close() sends close_notify but leaves the socket to us
conn.close()
eof = held.sock.recv(1) == b""
check("close_notify from the client -> the server closes the connection", body == b"ok" and eof)

# 5. garbage after the handshake: the kernel cannot decrypt it and the server closes
conn, held = connect()
held.release()
held.sock.sendall(b"\x17\x03\x03\x00\x20" + bytes(range(32)))
held.sock.settimeout(3)
try:
    eof = held.sock.recv(1) == b""
except ConnectionResetError:
    eof = True                                   # closed with unread data: a reset, still a close
except socket.timeout:
    eof = False
check("a corrupted record after the handshake -> the server closes", eof)

failed = results.count(False)
print("all passed" if not failed else f"{failed} FAILED")
sys.exit(1 if failed else 0)
