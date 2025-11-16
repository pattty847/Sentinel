#!/usr/bin/env python3
"""Fetch SEC insider transactions for a ticker"""
import sys
import json
import asyncio
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent))

from sec.sec_api import SECDataFetcher

async def main():
    if len(sys.argv) < 2:
        print(json.dumps({"error": "Missing ticker argument"}))
        sys.exit(1)
    
    ticker = sys.argv[1]
    
    try:
        fetcher = SECDataFetcher()
        transactions = await fetcher.fetch_insider_filings(ticker)
        print("TRANSACTIONS_DATA:" + json.dumps(transactions))
    except Exception as e:
        print(json.dumps({"error": str(e)}))
        sys.exit(1)

if __name__ == "__main__":
    asyncio.run(main())