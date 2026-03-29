# Sentinel Python Scripts

One-shot data backends for the Sentinel terminal (and anything else that needs market data).
All scripts are designed to be called as subprocesses — they print a tagged line to stdout and exit.
No servers, no daemons, no persistent state required.

## Setup

Requires [uv](https://docs.astral.sh/uv/). From this directory (`scripts/`):

```bash
uv sync
```

That's it. `uv run python <script>` will auto-activate the venv from `pyproject.toml` here.

---

## 1. Stock Candles — `stocks/fetch_daily_ohlcv.py`

Fetches daily OHLCV candle history for any stock ticker via yfinance.

**Usage:**
```bash
uv run python stocks/fetch_daily_ohlcv.py <TICKER> [PERIOD]
```

| Arg      | Values                         | Default |
|----------|-------------------------------|---------|
| `TICKER` | `AAPL`, `NVDA`, `MSFT`, etc.  | —       |
| `PERIOD` | `1y` `2y` `5y` `10y` `max`   | `5y`    |

**Output (stdout):**
```
OHLCV_DATA:{"ticker":"AAPL","period":"5y","count":1256,"candles":[{"date":"2020-02-18","ts_ms":1582...,"open":...,"high":...,"low":...,"close":...,"volume":...}, ...]}
```
On error:
```
ERROR_DATA:{"error":"<message>"}
```

**Parse pattern (Python):**
```python
import subprocess, json

result = subprocess.run(
    ["uv", "run", "python", "stocks/fetch_daily_ohlcv.py", "AAPL", "1y"],
    capture_output=True, text=True, cwd="path/to/scripts"
)
for line in result.stdout.splitlines():
    if line.startswith("OHLCV_DATA:"):
        data = json.loads(line[len("OHLCV_DATA:"):])
        candles = data["candles"]  # list of dicts: date, ts_ms, open, high, low, close, volume
```

**Good for:** cron jobs that pull daily candles for a watchlist, TA indicator pipelines, backtests.

---

## 2. Screener — `screener/screener_fetch.py`

Pulls top movers / screener results from TradingView via `tvscreener`. Works for both stocks and crypto.

**Usage:**
```bash
uv run python screener/screener_fetch.py [--asset stock|crypto] [--limit N] [--min-volume V] [--fields FIELD1,FIELD2,...]
```

| Flag          | Default  | Description                                      |
|---------------|----------|--------------------------------------------------|
| `--asset`     | `crypto` | `stock` or `crypto`                              |
| `--limit`     | `50`     | Number of results                                |
| `--min-volume`| `0`      | Filter by minimum volume                         |
| `--fields`    | —        | Extra fields beyond the base set (see below)     |

**Base fields always included:**

*Stocks:* `symbol, name, price, change%, volume, rel_volume, market_cap, P/E, div_yield, sector, exchange`

*Crypto:* `symbol, name, price, change%, volume, rel_volume, market_cap, category, sector, exchange`

**Extra field shorthands (stocks):** `RSI`, `EMA20`, `EMA50`, `EMA200`, `MACD`, `ADX`, `ATR`, `PE`, `DIV_YIELD`

**Extra field shorthands (crypto):** `RSI`, `RSI_1H`, `RSI_4H`, `EMA20`, `EMA50`, `EMA200`, `MACD_4H`, `VWAP`

**Output (stdout):**
```
SCREENER_DATA:{"asset":"stock","rows":[{"symbol":"NVDA","name":"NVIDIA","price":950.2,...}, ...]}
```

**Parse pattern (Python):**
```python
import subprocess, json

result = subprocess.run(
    ["uv", "run", "python", "screener/screener_fetch.py",
     "--asset", "stock", "--limit", "20", "--fields", "RSI,EMA50,ADX"],
    capture_output=True, text=True, cwd="path/to/scripts"
)
for line in result.stdout.splitlines():
    if line.startswith("SCREENER_DATA:"):
        data = json.loads(line[len("SCREENER_DATA:"):])
        rows = data["rows"]  # list of dicts keyed by field name
```

**Good for:** cron jobs that scan for high-RSI stocks, watchlist alerting, building ranked lists by any TA field.

---

## 3. SEC Filings — `sec/`

Fetches SEC EDGAR data: insider transactions (Form 4), financial summaries, and filing lists.
See [`sec/README.md`](sec/README.md) for the full module breakdown.

The three CLI entry points are:

| Script                      | What it fetches                                  |
|-----------------------------|--------------------------------------------------|
| `sec/sec_fetch_filings.py`  | Recent filing list for a ticker (10-K, 10-Q, 8-K, etc.) |
| `sec/sec_fetch_financials.py` | Key financial metrics from XBRL Company Facts  |
| `sec/sec_fetch_transactions.py` | Recent insider transactions (Form 4)         |

All three follow the same tagged-stdout pattern:
```bash
uv run python sec/sec_fetch_filings.py AAPL
uv run python sec/sec_fetch_financials.py AAPL
uv run python sec/sec_fetch_transactions.py AAPL
```

Output lines are prefixed with a data-type tag (e.g. `FILINGS_DATA:`, `FINANCIALS_DATA:`, `TRANSACTIONS_DATA:`) followed by JSON — same parse pattern as above.

Responses are cached locally under `data/edgar/` to respect SEC rate limits (10 req/s).

---

## The Subprocess Pattern

All scripts follow the same convention so any caller (C++, Python, shell, cron) can use them identically:

1. Call `uv run python <script> <args>` with `cwd` set to this `scripts/` directory.
2. Scan stdout line by line for a line starting with `<TAG>:`.
3. Parse everything after the colon as JSON.
4. Any line not starting with a known tag is a log/debug line — ignore or print it.
5. Non-zero exit code = hard failure; `ERROR_DATA:{"error":"..."}` = soft/data failure.

```python
import subprocess, json

def run_script(script: str, args: list[str], scripts_dir: str) -> dict | None:
    result = subprocess.run(
        ["uv", "run", "python", script, *args],
        capture_output=True, text=True, cwd=scripts_dir
    )
    for line in result.stdout.splitlines():
        for tag in ("OHLCV_DATA:", "SCREENER_DATA:", "FILINGS_DATA:", "FINANCIALS_DATA:", "TRANSACTIONS_DATA:", "ERROR_DATA:"):
            if line.startswith(tag):
                return json.loads(line[len(tag):])
    return None
```
