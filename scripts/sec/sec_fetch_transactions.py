#!/usr/bin/env python3
"""Fetch SEC insider transactions (Form 4) for a ticker."""
import sys
import json
import asyncio
import argparse
import logging
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent))

from sec.sec_api import SECDataFetcher


def _load_payload(args: argparse.Namespace) -> dict:
    if args.input_json:
        try:
            return json.load(sys.stdin)
        except json.JSONDecodeError as e:
            raise ValueError(f"Invalid JSON input: {e}") from e
    return {
        "ticker": args.ticker,
        "days_back": args.days_back,
        "use_cache": not args.no_cache,
        "filing_limit": args.filing_limit,
        "user_agent": args.user_agent,
        "cache_dir": args.cache_dir,
        "db_path": args.db_path,
        "rate_limit_sleep": args.rate_limit_sleep
    }


async def main() -> int:
    parser = argparse.ArgumentParser(description="Fetch SEC insider transactions")
    parser.add_argument("--ticker", "-t", help="Ticker symbol (e.g., AAPL)")
    parser.add_argument("--days-back", type=int, default=90, help="Days back to include")
    parser.add_argument("--filing-limit", type=int, default=10, help="Max filings to process")
    parser.add_argument("--no-cache", action="store_true", help="Disable cache usage")
    parser.add_argument("--user-agent", default=None, help="SEC User-Agent override")
    parser.add_argument("--cache-dir", default=None, help="Override SEC cache directory")
    parser.add_argument("--db-path", default=None, help="Override SQL cache database path")
    parser.add_argument("--rate-limit-sleep", type=float, default=0.1, help="Rate limit sleep interval (seconds)")
    parser.add_argument("--input-json", action="store_true", help="Read JSON input from stdin")
    args = parser.parse_args()

    logging.basicConfig(level=logging.INFO, stream=sys.stderr)

    try:
        payload = _load_payload(args)
        ticker = payload.get("ticker")
        if not ticker:
            raise ValueError("Missing ticker")

        days_back = int(payload.get("days_back", 90))
        filing_limit = int(payload.get("filing_limit", 10))
        use_cache = bool(payload.get("use_cache", True))

        fetcher = SECDataFetcher(
            user_agent=payload.get("user_agent"),
            cache_dir=payload.get("cache_dir"),
            db_path=payload.get("db_path"),
            rate_limit_sleep=float(payload.get("rate_limit_sleep", 0.1))
        )
        try:
            transactions = await fetcher.get_recent_insider_transactions(
                ticker=ticker,
                days_back=days_back,
                use_cache=use_cache,
                filing_limit=filing_limit
            )
        finally:
            await fetcher.close()

        output = {
            "ok": True,
            "operation": "insider_transactions",
            "ticker": ticker.upper(),
            "days_back": days_back,
            "filing_limit": filing_limit,
            "count": len(transactions),
            "data": transactions
        }
        print(json.dumps(output))
        return 0
    except Exception as e:
        logging.error(f"SEC insider transactions fetch failed: {e}")
        error_payload = {"ok": False, "operation": "insider_transactions", "error": {"message": str(e)}}
        print(json.dumps(error_payload))
        return 1


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
