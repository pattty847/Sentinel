import argparse
import asyncio
import json
import sys

try:
    import websockets
except Exception:
    websockets = None


def parse_args():
    p = argparse.ArgumentParser(description="Request candle history from Sentinel WS server")
    p.add_argument("--url", default="ws://127.0.0.1:8080", help="WebSocket URL")
    p.add_argument("--symbol", default="BTC-USD", help="Product symbol")
    p.add_argument("--timeframe-sec", type=int, default=60, help="Candle timeframe in seconds")
    p.add_argument("--end-time-sec", type=int, default=0, help="End time (sec). 0=server now")
    p.add_argument("--limit", type=int, default=50, help="Number of candles (<=350)")
    return p.parse_args()


async def main():
    if websockets is None:
        print("Missing dependency: websockets. Run: python -m pip install websockets", file=sys.stderr)
        sys.exit(1)

    args = parse_args()
    msg = {
        "type": "candle_history_request",
        "symbol": args.symbol,
        "timeframe_sec": args.timeframe_sec,
        "end_time_sec": args.end_time_sec,
        "limit": args.limit,
    }

    async with websockets.connect(args.url) as ws:
        await ws.send(json.dumps(msg))
        resp = await ws.recv()
        print(resp)


if __name__ == "__main__":
    asyncio.run(main())