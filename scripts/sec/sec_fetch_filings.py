#!/usr/bin/env python3
"""Fetch SEC filings for a ticker"""
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
    form_type = sys.argv[2] if len(sys.argv) > 2 and sys.argv[2] != "" else None
    
    try:
        fetcher = SECDataFetcher()
        filings = await fetcher.get_filings_by_form(ticker, form_type)
        print("FILINGS_DATA:" + json.dumps(filings))
    except Exception as e:
        print(json.dumps({"error": str(e)}))
        sys.exit(1)

if __name__ == "__main__":
    asyncio.run(main())