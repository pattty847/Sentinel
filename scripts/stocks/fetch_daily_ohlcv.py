"""Fetch daily OHLCV candles for a stock ticker via yfinance.

Usage: uv run python fetch_daily_ohlcv.py <TICKER> <PERIOD>
  TICKER : e.g. AAPL, NVDA, MSFT
  PERIOD : 1y | 2y | 5y | 10y | max   (default: 5y)

Output (stdout):
  OHLCV_DATA:<json>   — on success
  ERROR_DATA:<json>   — on failure
"""
import sys
import json
import math


def _safe(v):
    """Replace NaN/inf with None for JSON safety."""
    if isinstance(v, float) and (math.isnan(v) or math.isinf(v)):
        return None
    return v


def main():
    if len(sys.argv) < 2:
        print("ERROR_DATA:" + json.dumps({"error": "Usage: fetch_daily_ohlcv.py <TICKER> [PERIOD]"}))
        sys.exit(1)

    ticker = sys.argv[1].upper().strip()
    period = sys.argv[2].strip() if len(sys.argv) >= 3 else "5y"

    valid_periods = {"1y", "2y", "5y", "10y", "max"}
    if period not in valid_periods:
        print("ERROR_DATA:" + json.dumps({"error": f"Invalid period '{period}'. Use: {', '.join(sorted(valid_periods))}"}))
        sys.exit(1)

    try:
        import yfinance as yf
    except ImportError:
        print("ERROR_DATA:" + json.dumps({"error": "yfinance not installed. Run: uv add yfinance"}))
        sys.exit(1)

    try:
        t = yf.Ticker(ticker)
        df = t.history(period=period, interval="1d", auto_adjust=True)

        if df is None or df.empty:
            print("ERROR_DATA:" + json.dumps({"error": f"No data returned for '{ticker}'. Check the ticker symbol."}))
            sys.exit(1)

        candles = []
        for ts, row in df.iterrows():
            candles.append({
                "date":   ts.strftime("%Y-%m-%d"),
                "ts_ms":  int(ts.timestamp() * 1000),
                "open":   _safe(float(row["Open"])),
                "high":   _safe(float(row["High"])),
                "low":    _safe(float(row["Low"])),
                "close":  _safe(float(row["Close"])),
                "volume": _safe(float(row["Volume"])),
            })

        payload = {
            "ticker": ticker,
            "period": period,
            "count":  len(candles),
            "candles": candles,
        }
        print("OHLCV_DATA:" + json.dumps(payload))

    except Exception as e:
        print("ERROR_DATA:" + json.dumps({"error": str(e)}))
        sys.exit(1)


if __name__ == "__main__":
    main()
