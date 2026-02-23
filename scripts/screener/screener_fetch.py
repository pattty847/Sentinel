#!/usr/bin/env python3
"""One-shot screener query — prints SCREENER_DATA:<json> to stdout."""
import sys
import json
import argparse
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent))

from screener.screener_core import build_screener, screener_to_rows


def parse_args():
    p = argparse.ArgumentParser(description="Fetch TradingView screener data")
    p.add_argument("--asset", choices=["crypto", "stock"], default="crypto")
    p.add_argument("--fields", default="", help="Comma-separated field names, e.g. close,change,volume,RSI|240")
    p.add_argument("--limit", type=int, default=50)
    p.add_argument("--min-volume", type=float, default=0.0, help="Minimum 24h volume filter")
    return p.parse_args()


def main():
    args = parse_args()
    fields = [f.strip() for f in args.fields.split(",") if f.strip()]

    try:
        screener = build_screener(args.asset, fields, args.min_volume, args.limit)
        df = screener.get()
        rows = screener_to_rows(df)
        print("SCREENER_DATA:" + json.dumps({"asset": args.asset, "rows": rows}))
    except Exception as e:
        print(json.dumps({"error": str(e)}))
        sys.exit(1)


if __name__ == "__main__":
    main()
