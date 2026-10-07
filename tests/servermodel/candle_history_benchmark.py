#!/usr/bin/env python3
"""Opt-in live-provider benchmark; run through build-queue.sh, never CTest.

Starts only the supplied branch binary, with fresh scratch configuration, no
recording/roller, an offline loopback market-data endpoint, and non-production ports. Uses public REST
through the actual server's candle_history_request handler. Keeps server run logs
in the printed scratch directory; never reads credentials or production settings.
"""
import argparse
import base64
import datetime
import json
import os
from pathlib import Path
import socket
import ssl
import statistics
import struct
import subprocess
import tempfile
import time


class Client:
    def __init__(self, port, certificate):
        context = ssl.create_default_context(cafile=str(certificate))
        self.socket = context.wrap_socket(socket.create_connection(('127.0.0.1', port), timeout=10),
                                          server_hostname='localhost')
        nonce = base64.b64encode(os.urandom(16)).decode()
        self.socket.sendall((f'GET / HTTP/1.1\r\nHost: localhost:{port}\r\nUpgrade: websocket\r\n'
                             f'Connection: Upgrade\r\nSec-WebSocket-Key: {nonce}\r\n'
                             'Sec-WebSocket-Version: 13\r\n\r\n').encode())
        header = b''
        while not header.endswith(b'\r\n\r\n'):
            header += self.read(1)
        assert b' 101 ' in header, header

    def read(self, count):
        data = b''
        while len(data) < count:
            part = self.socket.recv(count - len(data))
            if not part:
                raise RuntimeError('server closed socket')
            data += part
        return data

    def send(self, payload, opcode=1):
        mask = os.urandom(4)
        length = len(payload)
        prefix = bytes([0x80 | opcode])
        prefix += bytes([0x80 | length]) if length < 126 else bytes([0x80 | 126]) + struct.pack('!H', length)
        self.socket.sendall(prefix + mask + bytes(v ^ mask[i % 4] for i, v in enumerate(payload)))

    def receive(self):
        message = b''
        while True:
            first, second = self.read(2)
            length = second & 127
            if length == 126:
                length = struct.unpack('!H', self.read(2))[0]
            elif length == 127:
                length = struct.unpack('!Q', self.read(8))[0]
            assert not second & 128
            payload = self.read(length)
            opcode = first & 15
            if opcode == 9:
                self.send(payload, 10)
                continue
            if opcode == 8:
                raise RuntimeError('server closed websocket')
            if opcode in (0, 1):
                message += payload
                if first & 128:
                    return json.loads(message)


def measure(client, end, timeframe, bars, log):
    before = log.read_text().count('[candles.rest.request]')
    started = time.monotonic()
    first_ms = None
    requests = 0
    timestamps = []
    cursor = end
    remaining = bars
    page_ms = []
    next_send = started
    while remaining:
        # Match CandleBackfillState's leading/trailing 100 ms send throttle.
        time.sleep(max(0, next_send - time.monotonic()))
        next_send = time.monotonic() + .1
        count = min(350, remaining)
        request = {'type': 'candle_history_request', 'symbol': 'BTC-USD',
                   'timeframe_sec': timeframe, 'end_time_sec': cursor, 'limit': count}
        client.send(json.dumps(request).encode())
        requests += 1
        while True:
            response = client.receive()
            if response['type'] == 'error':
                raise RuntimeError(response)
            if response['type'] == 'candle_history_chunk':
                break
        assert response['end_time_sec'] == cursor, response
        page = response['candles']
        assert len(page) == count, (cursor, count, len(page))
        assert all(bar['is_closed'] for bar in page)
        expected = list(range((cursor - count * timeframe) * 1000, cursor * 1000, timeframe * 1000))
        assert [bar['time_start_ms'] for bar in page] == expected
        timestamps += expected
        elapsed = (time.monotonic() - started) * 1000
        page_ms.append(round(elapsed, 2))
        if first_ms is None:
            first_ms = elapsed
        cursor -= count * timeframe
        remaining -= count
    assert len(set(timestamps)) == bars
    # Debug/probe log records flush every 250 ms. This wait is outside the
    # measured interval and makes REST counts include the final page.
    time.sleep(.3)
    starts = [datetime.datetime.fromisoformat(line[:23]).timestamp()
              for line in log.read_text().splitlines() if '[candles.rest.request]' in line][before:]
    gaps = [(b - a) * 1000 for a, b in zip(starts, starts[1:])]
    if gaps:
        assert min(gaps) >= 399, gaps  # log timestamps have millisecond precision
    return {'first_ms': round(first_ms, 2), 'full_ms': round(elapsed, 2),
            'server_requests': requests, 'rest_calls': len(starts),
            'min_rest_start_gap_ms': round(min(gaps), 1) if gaps else None,
            'bars': bars, 'page_ms': page_ms}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--worktree', type=Path, required=True)
    parser.add_argument('--port', type=int, default=18088)
    parser.add_argument('--health-port', type=int, default=18098)
    parser.add_argument('--offline-mdc-port', type=int, default=18089)
    parser.add_argument('--end', type=int, default=1791342900, help='Exclusive, closed UTC page boundary')
    parser.add_argument('--timeframe', type=int, default=900)
    parser.add_argument('--bars', type=int, default=1344)
    args = parser.parse_args()
    assert len({args.port, args.health_port, args.offline_mdc_port}) == 3
    assert all(p not in (8080, 8090, 8091, 17100, 17190) and 1024 < p < 65536
               for p in (args.port, args.health_port, args.offline_mdc_port))
    assert args.end % args.timeframe == 0 and args.end <= time.time()
    root = args.worktree.resolve()
    binary = root / 'build/mac-clang/apps/sentinel-server/sentinel-server'
    scratch = Path(tempfile.mkdtemp(prefix='sentinel-candle-history-', dir='/tmp'))
    (scratch / 'config').mkdir()
    certificate, key = scratch / 'test.crt', scratch / 'test.key'
    subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1',
                    '-subj', '/CN=localhost', '-keyout', str(key), '-out', str(certificate)],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    (scratch / 'config/server_config.yaml').write_text(f'''schema_version: 1
stream_port: {args.port}
default_symbols: [BTC-USD]
heatmap:
  timeframes_ms: [60000, 300000, 900000, 3600000, 14400000, 86400000]
  timeframe: 60000
  persistence_enabled: false
  persistence_dir: {scratch}/heatmap
recording:
  enabled: false
  dir: {scratch}/recording
  fallback_dir: {scratch}/fallback
roller_shadow:
  enabled: false
  journal_dir: {scratch}/journal
  dir: {scratch}/roller
  socket: {scratch}/capture.sock
tls:
  cert_file: {certificate}
  key_file: {key}
mdc:
  host: 127.0.0.1
  port: "{args.offline_mdc_port}"
  connect_timeout_ms: 100
  use_jwt: false
  ssl_ca_bundle: {root}/resources/certs/ca-bundle.crt
''')
    # Empty default_symbols is ignored by ConfigLoader. Keep the default product
    # but bind a non-serving loopback endpoint so no live provider feed is opened.
    offline = socket.socket()
    offline.bind(('127.0.0.1', args.offline_mdc_port))
    print(json.dumps({'scratch': str(scratch), 'binary': str(binary)}), flush=True)
    results = []
    for run in range(1, 4):
        logdir = scratch / f'run-{run}'
        logdir.mkdir()
        env = {**os.environ, 'SENTINEL_HEALTH_PORT': str(args.health_port),
               'SENTINEL_LOG_DIR': str(logdir), 'SENTINEL_PROBES': 'candles,history.rest'}
        process = subprocess.Popen([str(binary)], cwd=scratch, env=env,
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        client = None
        try:
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    raise RuntimeError(f'server exited {process.returncode}; logs: {logdir}')
                try:
                    client = Client(args.port, certificate)
                    break
                except (OSError, ssl.SSLError):
                    time.sleep(.05)
            if client is None:
                raise RuntimeError(f'server never ready; logs: {logdir}')
            log = logdir / 'sentinel-server-latest.log'
            time.sleep(.3)  # startup debug records use the same 250 ms log flusher
            header = log.read_text()
            assert f'exe={binary}' in header and 'built=' in header
            assert 'credentials=false' in header or 'credentials=0' in header
            for cache in ('cold', 'warm'):
                result = {'run': run, 'cache': cache, **measure(client, args.end, args.timeframe, args.bars, log)}
                assert result['rest_calls'] == (0 if cache == 'warm' else (args.bars + 349) // 350)
                results.append(result)
                print(json.dumps(result), flush=True)
        finally:
            if client:
                client.socket.close()
            process.terminate()
            try:
                process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
    offline.close()
    for cache in ('cold', 'warm'):
        runs = [r for r in results if r['cache'] == cache]
        print(json.dumps({'cache': cache, **{
            field: {'p50': round(statistics.median(r[field] for r in runs), 2),
                    'max': max(r[field] for r in runs)} for field in ('first_ms', 'full_ms', 'rest_calls')}}))


if __name__ == '__main__':
    main()
