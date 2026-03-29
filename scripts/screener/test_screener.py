#!/usr/bin/env python3
"""
Quick sanity tests for the screener backend.

Tests (run independently, no server needed for 1 & 2):
  1. screener_core — direct tvscreener call (crypto)
  2. screener_core — direct tvscreener call (stocks)
  3. screener_server — spin up server, connect via WS, get a screener_update

Usage:
  python scripts/screener/test_screener.py          # runs all tests
  python scripts/screener/test_screener.py --test 1 # run single test
"""
import asyncio
import json
import sys
import argparse
import subprocess
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent))

from screener.screener_core import build_screener, screener_to_rows

PASS = "[PASS]"
FAIL = "[FAIL]"


# ── Test 1: crypto one-shot ──────────────────────────────────────────────────

def test_crypto_direct():
    print("\n[1] crypto direct fetch (tvscreener)...")
    try:
        screener = build_screener(
            asset="crypto",
            extra_fields=["RSI|240"],
            min_volume=1_000_000,
            limit=10,
        )
        df = screener.get()
        rows = screener_to_rows(df)
        assert len(rows) > 0, "no rows returned"
        first = rows[0]
        assert "symbol" in first, "missing symbol key"
        assert any("price" in k.lower() or "close" in k.lower() for k in first), \
            f"no price-like field in row keys: {list(first.keys())}"
        print(f"  {PASS} got {len(rows)} rows -- first: {first.get('symbol')} "
              f"keys={list(first.keys())[:6]}")
        return True
    except Exception as e:
        print(f"  {FAIL} {e}")
        return False


# ── Test 2: stock one-shot ───────────────────────────────────────────────────

def test_stock_direct():
    print("\n[2] stock direct fetch (tvscreener)...")
    try:
        screener = build_screener(
            asset="stock",
            extra_fields=["RSI_1D", "EMA50"],
            min_volume=500_000,
            limit=10,
        )
        df = screener.get()
        rows = screener_to_rows(df)
        assert len(rows) > 0, "no rows returned"
        first = rows[0]
        print(f"  {PASS} got {len(rows)} rows -- first: {first.get('symbol')} "
              f"keys={list(first.keys())[:6]}")
        return True
    except Exception as e:
        print(f"  {FAIL} {e}")
        return False


# ── Test 3: WebSocket server round-trip ──────────────────────────────────────

async def _ws_test():
    import aiohttp
    port = 17200
    url = f"ws://127.0.0.1:{port}/screener"

    async with aiohttp.ClientSession() as session:
        async with session.ws_connect(url, timeout=aiohttp.ClientWSTimeout(ws_close=5.0)) as ws:
            # Send config requesting immediate fetch
            await ws.send_str(json.dumps({
                "type": "set_config",
                "asset": "crypto",
                "fields": ["RSI|240"],
                "min_volume": 1_000_000,
                "limit": 5,
                "interval_sec": 300,  # long interval so it doesn't auto-repeat during test
            }))

            # Collect messages until we get screener_update or error
            deadline = time.time() + 30  # tvscreener can be slow
            while time.time() < deadline:
                try:
                    msg = await asyncio.wait_for(ws.receive(), timeout=5.0)
                    if msg.type == aiohttp.WSMsgType.TEXT:
                        data = json.loads(msg.data)
                        print(f"  server msg: type={data.get('type')} "
                              f"msg={data.get('message', data.get('row_count', ''))}")
                        if data["type"] == "screener_update":
                            rows = data["rows"]
                            assert len(rows) > 0, "empty rows in update"
                            return True, f"got {len(rows)} rows"
                        elif data["type"] == "error":
                            return False, data.get("message", "unknown error")
                except asyncio.TimeoutError:
                    continue

            return False, "timed out waiting for screener_update"


def test_ws_server():
    print("\n[3] WebSocket server round-trip...")
    server_script = Path(__file__).parent / "screener_server.py"
    proc = subprocess.Popen(
        [sys.executable, str(server_script)],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    time.sleep(1.5)  # give server time to bind

    try:
        ok, msg = asyncio.run(_ws_test())
        if ok:
            print(f"  {PASS} {msg}")
        else:
            print(f"  {FAIL} {msg}")
        return ok
    except Exception as e:
        print(f"  {FAIL} {e}")
        return False
    finally:
        proc.terminate()
        proc.wait(timeout=5)


# ── Runner ────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--test", type=int, choices=[1, 2, 3], help="Run only this test number")
    args = parser.parse_args()

    tests = {
        1: test_crypto_direct,
        2: test_stock_direct,
        3: test_ws_server,
    }

    if args.test:
        selected = {args.test: tests[args.test]}
    else:
        selected = tests

    results = {}
    for num, fn in selected.items():
        results[num] = fn()

    print("\n-- Summary --")
    all_passed = True
    for num, passed in results.items():
        status = PASS if passed else FAIL
        print(f"  Test {num}: {status}")
        if not passed:
            all_passed = False

    sys.exit(0 if all_passed else 1)


if __name__ == "__main__":
    main()
