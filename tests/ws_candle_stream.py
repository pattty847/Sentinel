import argparse
import asyncio
import json
import sys

try:
    import websockets
except Exception:
    websockets = None


def parse_args():
    p = argparse.ArgumentParser(description="Subscribe to live candle updates from Sentinel WS server")
    p.add_argument("--url", default="ws://127.0.0.1:8080", help="WebSocket URL")
    p.add_argument("--symbol", default="BTC-USD", help="Product symbol")
    p.add_argument("--timeframe-sec", type=int, default=1, help="Candle timeframe in seconds (filter)"
                 )
    p.add_argument("--max", type=int, default=50, help="Max messages before exit (0=run forever)")
    return p.parse_args()


async def main():
    if websockets is None:
        print("Missing dependency: websockets. Run: python -m pip install websockets", file=sys.stderr)
        sys.exit(1)

    args = parse_args()
    sub = {"type": "subscribe", "symbol": args.symbol}

    count = 0
    async with websockets.connect(args.url) as ws:
        await ws.send(json.dumps(sub))
        while True:
            msg = await ws.recv()
            try:
                data = json.loads(msg)
            except Exception:
                print(msg)
                continue

            mtype = data.get("type", "")
            if mtype not in ("candle_bar_update", "candle_bar_closed"):
                continue
            if args.timeframe_sec and data.get("timeframe_sec") != args.timeframe_sec:
                continue

            print(msg)
            count += 1
            if args.max > 0 and count >= args.max:
                break


if __name__ == "__main__":
    asyncio.run(main())