#!/usr/bin/env python3
"""Request one recording heatmap page from a running local Sentinel server."""

import argparse
import base64
import json
import os
import socket
import ssl
import struct
import time


def read_exact(sock, count):
    data = bytearray()
    while len(data) < count:
        chunk = sock.recv(count - len(data))
        if not chunk:
            raise EOFError("server closed the connection")
        data.extend(chunk)
    return bytes(data)


def send_frame(sock, payload):
    payload = payload.encode()
    mask = os.urandom(4)
    if len(payload) < 126:
        head = bytes([0x81, 0x80 | len(payload)])
    elif len(payload) < 65536:
        head = b"\x81\xfe" + struct.pack("!H", len(payload))
    else:
        head = b"\x81\xff" + struct.pack("!Q", len(payload))
    sock.sendall(head + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(payload)))


def read_frame(sock):
    first, second = read_exact(sock, 2)
    length = second & 0x7F
    if length == 126:
        length = struct.unpack("!H", read_exact(sock, 2))[0]
    elif length == 127:
        length = struct.unpack("!Q", read_exact(sock, 8))[0]
    if length > 20_000_000:
        raise ValueError(f"frame too large: {length}")
    mask = read_exact(sock, 4) if second & 0x80 else None
    payload = read_exact(sock, length)
    if mask:
        payload = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
    return first & 0x0F, payload, bool(first & 0x80)


def read_message(sock):
    """Reassemble fragmented messages: the server (Boost.Beast) splits large replies."""
    opcode, payload, fin = read_frame(sock)
    while not fin:
        _, more, fin = read_frame(sock)
        payload += more
    return opcode, payload


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--symbol", default="BTC-USD")
    parser.add_argument("--timeframe-ms", type=int, default=60000)
    parser.add_argument("--price-min", type=float, required=True)
    parser.add_argument("--price-max", type=float, required=True)
    parser.add_argument("--rows", type=int, default=256)
    parser.add_argument("--count", type=int, default=8)
    parser.add_argument("--display-tick", type=float)
    args = parser.parse_args()
    context = ssl._create_unverified_context()  # local development certificate
    with socket.create_connection((args.host, args.port), timeout=10) as raw:
        with context.wrap_socket(raw, server_hostname=args.host) as sock:
            sock.settimeout(20)
            key = base64.b64encode(os.urandom(16)).decode()
            sock.sendall((f"GET / HTTP/1.1\r\nHost: {args.host}:{args.port}\r\n"
                          "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                          f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
            header = bytearray()
            while not header.endswith(b"\r\n\r\n"):
                header.extend(read_exact(sock, 1))
                if len(header) > 8192:
                    raise ValueError("oversize handshake")
            if b" 101 " not in header.split(b"\r\n", 1)[0]:
                raise RuntimeError(header.decode(errors="replace"))
            request = {"type": "heatmap_history_request", "source": "recording",
                       "symbol": args.symbol, "timeframe_ms": args.timeframe_ms,
                       "end_time": 0, "count": args.count, "rows": args.rows,
                       "price_min": args.price_min, "price_max": args.price_max,
                       "request_id": f"probe-{os.getpid()}", "band_generation": 1}
            if args.display_tick is not None:
                request["display_tick"] = args.display_tick
            send_frame(sock, json.dumps(request))
            deadline = time.monotonic() + 30
            while time.monotonic() < deadline:
                opcode, payload = read_message(sock)
                if opcode == 9:
                    continue
                if opcode != 1:
                    continue
                reply = json.loads(payload)
                if reply.get("request_id") != request["request_id"]:
                    continue
                if reply.get("type") == "error":
                    raise RuntimeError(reply.get("message", "unknown server error"))
                if reply.get("type") == "heatmap_history_chunk":
                    print(json.dumps({key: reply.get(key) for key in
                                      ("status", "layer", "band_lo", "band_tick", "band_rows",
                                       "scanned_start", "scanned_end", "next_end", "exhausted",
                                       "oldest_available_ms", "latest_available_ms")}, indent=2))
                    print(f"columns={len(reply.get('columns', []))}")
                    return
            raise TimeoutError("no matching recording history reply")


if __name__ == "__main__":
    main()
