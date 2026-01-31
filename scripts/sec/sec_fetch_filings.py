#!/usr/bin/env python3
"""Fetch SEC filings for a ticker."""
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
        "form_type": args.form_type,
        "days_back": args.days_back,
        "use_cache": not args.no_cache,
        "user_agent": args.user_agent,
        "cache_dir": args.cache_dir,
        "db_path": args.db_path,
        "rate_limit_sleep": args.rate_limit_sleep
    }


async def main() -> int:
    parser = argparse.ArgumentParser(description="Fetch SEC filings")
    parser.add_argument("--ticker", "-t", help="Ticker symbol (e.g., AAPL)")
    parser.add_argument("--form-type", "-f", default=None, help="Form type (e.g., 10-K, 10-Q, 8-K). Omit for all.")
    parser.add_argument("--days-back", type=int, default=90, help="Days back to include")
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

        form_type = payload.get("form_type") or None
        if isinstance(form_type, str):
            if form_type.strip().lower() in {"form 4", "form4"}:
                form_type = "4"
        days_back = int(payload.get("days_back", 90))
        use_cache = bool(payload.get("use_cache", True))

        fetcher = SECDataFetcher(
            user_agent=payload.get("user_agent"),
            cache_dir=payload.get("cache_dir"),
            db_path=payload.get("db_path"),
            rate_limit_sleep=float(payload.get("rate_limit_sleep", 0.1))
        )
        try:
            if form_type:
                filings = await fetcher.get_filings_by_form(ticker, form_type, days_back=days_back, use_cache=use_cache)
            else:
                filings = await fetcher.get_recent_filings(ticker, days_back=days_back, use_cache=use_cache)
        finally:
            await fetcher.close()

        output = {
            "ok": True,
            "operation": "filings",
            "ticker": ticker.upper(),
            "form_type": form_type,
            "days_back": days_back,
            "count": len(filings),
            "data": filings
        }
        print(json.dumps(output))
        return 0
    except Exception as e:
        logging.error(f"SEC filings fetch failed: {e}")
        error_payload = {"ok": False, "operation": "filings", "error": {"message": str(e)}}
        print(json.dumps(error_payload))
        return 1


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
