#!/usr/bin/env python3
"""Fetch SEC insider signal payload for a ticker."""
import sys
import json
import asyncio
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent))

from sec.sec_api import SECDataFetcher


async def main():
    if len(sys.argv) < 2:
        print("ERROR_DATA:" + json.dumps({"error": "Missing ticker argument"}))
        sys.exit(1)

    ticker = sys.argv[1]
    days_back = int(sys.argv[2]) if len(sys.argv) > 2 else 180

    try:
        fetcher = SECDataFetcher()
        payload = await fetcher.get_insider_signal_payload(ticker, days_back=days_back)
        print("INSIDER_SIGNALS_DATA:" + json.dumps(payload))
    except Exception as exc:
        print("ERROR_DATA:" + json.dumps({"error": str(exc)}))
        sys.exit(1)


if __name__ == "__main__":
    asyncio.run(main())
