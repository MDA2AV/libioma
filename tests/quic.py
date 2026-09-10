#!/usr/bin/env python3
"""QUIC through tests/pipe-server.c: the same line echo, every stream a pipe of its own. A
handshake with the mounted certificate and the "echo" protocol, a line echoed on a stream, many
streams on one connection, streams on many connections, a body larger than every window with the
echo read back while it is still being sent, the server finishing a stream when the handler
returns, a client that resets a stream, a protocol the server does not answer, and an idle close.

    python3 tests/quic.py <port> [certs-dir]   (aioquic in the python; skips itself without it)"""
import asyncio, os, ssl, sys, time

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8101

try:
    from aioquic.asyncio import connect
    from aioquic.asyncio.protocol import QuicConnectionProtocol
    from aioquic.quic.configuration import QuicConfiguration
    from aioquic.quic.events import ConnectionTerminated, StreamDataReceived, StreamReset
except ImportError:
    print("SKIP quic: aioquic is not installed in this python")
    sys.exit(0)

results = []


def check(name, cond):
    print(f"{'ok ' if cond else 'FAIL'} {name}")
    results.append(cond)
    return cond


class Echo(QuicConnectionProtocol):
    """Streams as queues: (data, end) pairs per stream, and the connection's end."""

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.streams = {}
        self.terminated = None
        self.resets = {}

    def queue(self, sid):
        return self.streams.setdefault(sid, asyncio.Queue())

    def quic_event_received(self, event):
        if isinstance(event, StreamDataReceived):
            self.queue(event.stream_id).put_nowait((event.data, event.end_stream))
        elif isinstance(event, StreamReset):
            self.resets[event.stream_id] = event.error_code
            self.queue(event.stream_id).put_nowait((b"", True))
        elif isinstance(event, ConnectionTerminated):
            self.terminated = event
            for q in self.streams.values():
                q.put_nowait((b"", True))

    def send(self, sid, data, end=False):
        self._quic.send_stream_data(sid, data, end)
        self.transmit()

    def open(self):
        return self._quic.get_next_available_stream_id()

    async def read(self, sid, want=None, timeout=10):
        """Bytes until end_stream, or until `want` bytes are in."""
        out = b""
        deadline = time.monotonic() + timeout
        while True:
            left = deadline - time.monotonic()
            if left <= 0:
                return out, False
            try:
                data, end = await asyncio.wait_for(self.queue(sid).get(), left)
            except asyncio.TimeoutError:
                return out, False
            out += data
            if end:
                return out, True
            if want is not None and len(out) >= want:
                return out, False


def config(alpn="echo"):
    c = QuicConfiguration(is_client=True, alpn_protocols=[alpn], idle_timeout=20.0)
    c.verify_mode = ssl.CERT_NONE
    return c


async def echo_line(proto, line):
    sid = proto.open()
    proto.send(sid, line, end=True)
    data, end = await proto.read(sid)
    return sid, data, end


async def main():
    # 1. a handshake, and one line on one stream: the handler echoes it and ends the stream when the
    #    client's FIN ends its input
    async with connect("127.0.0.1", PORT, configuration=config(), create_protocol=Echo) as proto:
        await proto.wait_connected()
        check("handshake with ALPN echo", proto._quic.tls.alpn_negotiated == "echo")
        sid, data, end = await echo_line(proto, b"hello quic\n")
        check("a line on a stream comes back echoed, the server's FIN after it", data == b"echo: hello quic\n" and end)

        # 2. many streams on one connection, all in flight at once, each its own coroutine
        sids = []
        for i in range(50):
            sid = proto.open()
            proto.send(sid, b"line %d\n" % i, end=True)
            sids.append(sid)
        got = [await proto.read(sid) for sid in sids]
        check("50 streams at once, each echoed on its own", all(got[i] == (b"echo: line %d\n" % i, True) for i in range(50)))

        # 3. a stream held open: two lines with a pause between, echoed as they come, then quit
        sid = proto.open()
        proto.send(sid, b"first\n")
        d1, _ = await proto.read(sid, want=len(b"echo: first\n"))
        proto.send(sid, b"second\n")
        d2, _ = await proto.read(sid, want=len(b"echo: second\n"))
        proto.send(sid, b"quit\n")
        d3, end = await proto.read(sid)
        check("a stream held open echoes line by line; quit ends it from the server's side",
              d1 == b"echo: first\n" and d2 == b"echo: second\n" and d3 == b"" and end)

        # 4. more than every window: 2 MB of lines, the echo read as it arrives so nothing has to
        #    be buffered whole on either side (the stream window is 256 KB, the connection's 1 MB)
        line = b"x" * 1000 + b"\n"
        n = 2000
        sid = proto.open()
        total = 0
        async def pump():
            for i in range(n):
                proto.send(sid, line, end=(i == n - 1))
                if i % 50 == 49:
                    await asyncio.sleep(0)
        sender = asyncio.ensure_future(pump())
        data, end = await proto.read(sid, timeout=60)
        await sender
        check(f"2 MB over one stream, echoed while it is still arriving ({len(data)} bytes back)",
              len(data) == n * (6 + len(line)) and end and data.startswith(b"echo: " + line))

        # 5. the client resets a stream mid-way: the handler's read fails, the stream ends
        sid = proto.open()
        proto.send(sid, b"half a line without its end")
        proto._quic.reset_stream(sid, 7)
        proto.transmit()
        await asyncio.sleep(0.2)
        sid2, data, end = await echo_line(proto, b"still alive\n")
        check("a stream reset by the client ends it, the connection serves on", data == b"echo: still alive\n" and end)

        # 6. a unidirectional stream: read-only for the handler, its writes fail, the stream ends
        uni = proto._quic.get_next_available_stream_id(is_unidirectional=True)
        proto.send(uni, b"one way\n", end=True)
        await asyncio.sleep(0.2)
        sid3, data, end = await echo_line(proto, b"after uni\n")
        check("a one-way stream is taken and cannot be answered; the next stream is fine",
              data == b"echo: after uni\n" and end and proto.terminated is None)

    # 7. many connections, each a handshake of its own
    async def one(i):
        async with connect("127.0.0.1", PORT, configuration=config(), create_protocol=Echo) as p:
            await p.wait_connected()
            _, data, end = await echo_line(p, b"conn %d\n" % i)
            return data == b"echo: conn %d\n" % i and end
    outcomes = await asyncio.gather(*(one(i) for i in range(40)))
    check("40 connections at once", all(outcomes))

    # 8. a protocol the server does not answer: the handshake fails
    try:
        async with connect("127.0.0.1", PORT, configuration=config("nope"), create_protocol=Echo) as p:
            await asyncio.wait_for(p.wait_connected(), 5)
            refused = False
    except Exception:
        refused = True
    check("ALPN the server does not serve is refused at the handshake", refused)

    # 9. the server closes the connection when its idle timeout passes (30 s server side is long
    #    for a test; the client's own idle timeout of 2 s closes first and the server must take the
    #    CONNECTION_CLOSE without complaint), and a new connection works afterwards
    c = config()
    c.idle_timeout = 2.0
    async with connect("127.0.0.1", PORT, configuration=c, create_protocol=Echo) as p:
        await p.wait_connected()
        await asyncio.sleep(3.0)
        idle_closed = p.terminated is not None
    async with connect("127.0.0.1", PORT, configuration=config(), create_protocol=Echo) as p:
        await p.wait_connected()
        _, data, end = await echo_line(p, b"after idle\n")
    check("an idle connection is closed, and the port serves the next", idle_closed and data == b"echo: after idle\n" and end)


asyncio.run(main())
print("all passed" if all(results) else "FAILURES")
sys.exit(0 if all(results) else 1)
