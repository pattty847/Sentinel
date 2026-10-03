#!/usr/bin/env python3
"""Read-only fanout probe: prints control messages and journal positions (no raw data)."""
import argparse
import json
import os
import socket
import struct

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('product')
parser.add_argument('--socket', default=os.path.expanduser('~/Sentinel-runtime/run/capture.sock'))
parser.add_argument('--resume', type=json.loads, help='last applied position JSON')
parser.add_argument('--resnapshot', action='store_true', help='explicitly request a new upstream snapshot')
args = parser.parse_args()

def exact(stream, size):
    out = bytearray()
    while len(out) < size:
        part = stream.recv(size - len(out))
        if not part:
            raise SystemExit('EOF: resume from last applied journal position; never assume continuity')
        out.extend(part)
    return bytes(out)

with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as stream:
    stream.connect(args.socket)
    stream.sendall((json.dumps(dict(type='hello', version=1, product=args.product, pos=args.resume))+'\n').encode())
    requested = False
    while True:
        size, = struct.unpack('<I', exact(stream, 4))
        if not 4 <= size <= 16 * 1024 * 1024 + 4096:
            raise SystemExit('invalid wire length')
        body = exact(stream, size)
        hsize, = struct.unpack('<I', body[:4])
        if hsize > size - 4:
            raise SystemExit('invalid header length')
        message = json.loads(body[4:4+hsize])
        print(json.dumps(message), flush=True)
        if args.resnapshot and not requested and message['type'] == 'tip':
            stream.sendall((json.dumps(dict(type='resnapshot', product=args.product))+'\n').encode())
            requested = True
