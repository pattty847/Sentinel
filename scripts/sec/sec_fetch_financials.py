#!/usr/bin/env python3
"""Fetch SEC financial summary for a ticker"""
import sys
import json
import asyncio

from copetech_sec import SECDataFetcher

async def main():
    if len(sys.argv) < 2:
        print(json.dumps({"error": "Missing ticker argument"}))
        sys.exit(1)
    
    ticker = sys.argv[1]
    
    try:
        fetcher = SECDataFetcher()
        financials = await fetcher.get_financial_summary(ticker)
        print("FINANCIALS_DATA:" + json.dumps(financials))
    except Exception as e:
        print(json.dumps({"error": str(e)}))
        sys.exit(1)

if __name__ == "__main__":
    asyncio.run(main())