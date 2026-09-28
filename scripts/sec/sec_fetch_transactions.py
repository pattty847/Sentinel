#!/usr/bin/env python3
"""Fetch SEC insider transactions for a ticker."""
import sys
import json
import asyncio

from copetech_sec import SECDataFetcher

async def main():
    if len(sys.argv) < 2:
        print("ERROR_DATA:" + json.dumps({"error": "Missing ticker argument"}))
        sys.exit(1)
    
    ticker = sys.argv[1]
    
    try:
        fetcher = SECDataFetcher()
        transactions = await fetcher.get_recent_insider_transactions(ticker)
        print("TRANSACTIONS_DATA:" + json.dumps(transactions))
    except Exception as e:
        print("ERROR_DATA:" + json.dumps({"error": str(e)}))
        sys.exit(1)

if __name__ == "__main__":
    asyncio.run(main())
