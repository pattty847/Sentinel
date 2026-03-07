# Sentinel AI — Implementation Roadmap
## Come Home and Bang It Out

This is the step-by-step build guide. Each session is self-contained and leaves
you with something working and testable. No loose ends between sessions.

**Order**: FRED → COT → Alpha Vantage → AI Pipeline Core → Claude LLM → C++ wiring

---

## One-Time Setup

```bash
# From repo root
cd scripts

# Add to requirements.txt and install
uv pip install fredapi cot-reports aiohttp aiosqlite anthropic hmmlearn \
               scikit-learn lightgbm ruptures pykalman pycoingecko requests

# Add these to .env (copy .env.example if it exists, otherwise create it)
echo "FRED_API_KEY=your_key_here"         >> .env   # free at fred.stlouisfed.org
echo "ALPHA_VANTAGE_KEY=your_key_here"    >> .env   # free at alphavantage.co
echo "ANTHROPIC_API_KEY=sk-ant-..."       >> .env   # anthropic console
```

**FRED key**: https://fred.stlouisfed.org/docs/api/api_key.html — fill in email, instant.
**Alpha Vantage key**: https://www.alphavantage.co/support/#api-key — instant, free tier is 25 req/day.

---

## Session 1 — FRED Macro Layer (~2 hours)

**What you'll have after**: A SQLite-backed macro data layer that classifies the
current macro regime (rate env, yield curve, risk appetite) and serves it as a
compact object to the AI pipeline.

### 1.1 Create `scripts/macro/__init__.py`

```bash
mkdir -p scripts/macro
touch scripts/macro/__init__.py
```

### 1.2 Create `scripts/macro/macro_db.py`

Reuses the existing `aiosqlite` pattern from `scripts/sec/sql_cache_manager.py`.

```python
# scripts/macro/macro_db.py
import aiosqlite
import json
import os
from datetime import datetime, date
from typing import Optional, Any

DB_PATH = "data/sentinel.db"


async def init_macro_tables(db_path: str = DB_PATH):
    os.makedirs(os.path.dirname(db_path), exist_ok=True)
    async with aiosqlite.connect(db_path) as db:
        await db.execute("""
            CREATE TABLE IF NOT EXISTS macro_series (
                series_id   TEXT NOT NULL,
                date        TEXT NOT NULL,
                value       REAL,
                fetched_at  TEXT NOT NULL,
                PRIMARY KEY (series_id, date)
            )
        """)
        await db.execute("""
            CREATE TABLE IF NOT EXISTS macro_regime_cache (
                cache_key   TEXT PRIMARY KEY,
                regime_json TEXT NOT NULL,
                computed_at TEXT NOT NULL
            )
        """)
        await db.commit()


async def upsert_series(series_id: str, observations: list[dict], db_path: str = DB_PATH):
    """observations = [{'date': 'YYYY-MM-DD', 'value': float}, ...]"""
    now = datetime.utcnow().isoformat()
    async with aiosqlite.connect(db_path) as db:
        for obs in observations:
            val = obs.get("value")
            try:
                val = float(val) if val not in (None, ".") else None
            except (TypeError, ValueError):
                val = None
            await db.execute(
                "INSERT OR REPLACE INTO macro_series (series_id, date, value, fetched_at) "
                "VALUES (?, ?, ?, ?)",
                (series_id, obs["date"], val, now),
            )
        await db.commit()


async def get_latest(series_id: str, n: int = 90, db_path: str = DB_PATH) -> list[dict]:
    async with aiosqlite.connect(db_path) as db:
        db.row_factory = aiosqlite.Row
        async with db.execute(
            "SELECT date, value FROM macro_series WHERE series_id=? AND value IS NOT NULL "
            "ORDER BY date DESC LIMIT ?",
            (series_id, n),
        ) as cur:
            rows = await cur.fetchall()
    return [{"date": r["date"], "value": r["value"]} for r in reversed(rows)]


async def cache_regime(regime: dict, cache_key: str = "global", db_path: str = DB_PATH):
    async with aiosqlite.connect(db_path) as db:
        await db.execute(
            "INSERT OR REPLACE INTO macro_regime_cache (cache_key, regime_json, computed_at) "
            "VALUES (?, ?, ?)",
            (cache_key, json.dumps(regime), datetime.utcnow().isoformat()),
        )
        await db.commit()


async def load_cached_regime(
    max_age_hours: float = 6.0, cache_key: str = "global", db_path: str = DB_PATH
) -> Optional[dict]:
    async with aiosqlite.connect(db_path) as db:
        db.row_factory = aiosqlite.Row
        async with db.execute(
            "SELECT regime_json, computed_at FROM macro_regime_cache WHERE cache_key=?",
            (cache_key,),
        ) as cur:
            row = await cur.fetchone()
    if row is None:
        return None
    computed = datetime.fromisoformat(row["computed_at"])
    age_hours = (datetime.utcnow() - computed).total_seconds() / 3600
    if age_hours > max_age_hours:
        return None
    return json.loads(row["regime_json"])
```

### 1.3 Create `scripts/macro/fred_collector.py`

```python
# scripts/macro/fred_collector.py
"""
Fetches key macro series from FRED and classifies the current macro regime.

Series fetched:
  DGS10         - 10-year Treasury yield
  T10Y2Y        - Yield curve (10Y minus 2Y)
  VIXCLS        - VIX
  DTWEXBGS      - Trade-weighted dollar index
  BAMLH0A0HYM2  - High-yield credit spread (OAS)
  M2SL          - M2 money supply
  CPIAUCSL      - CPI (index level, we compute YoY)
  UNRATE        - Unemployment rate
"""
import asyncio
import json
import logging
import os
from datetime import datetime, timedelta
from typing import Optional

from dotenv import load_dotenv
from fredapi import Fred

from .macro_db import init_macro_tables, upsert_series, get_latest, cache_regime, load_cached_regime

load_dotenv()
log = logging.getLogger(__name__)

SERIES = {
    "DGS10":        "10y_yield",
    "T10Y2Y":       "yield_curve",
    "VIXCLS":       "vix",
    "DTWEXBGS":     "dxy",
    "BAMLH0A0HYM2": "hy_spread",
    "M2SL":         "m2",
    "CPIAUCSL":     "cpi",
    "UNRATE":       "unemployment",
}


def _fetch_series_sync(fred: Fred, series_id: str, days: int = 400) -> list[dict]:
    """Synchronous FRED fetch — will be wrapped in executor."""
    start = (datetime.today() - timedelta(days=days)).strftime("%Y-%m-%d")
    try:
        s = fred.get_series(series_id, observation_start=start)
        return [{"date": str(d.date()), "value": v} for d, v in s.items()]
    except Exception as e:
        log.error(f"FRED fetch failed for {series_id}: {e}")
        return []


async def fetch_all_series(force: bool = False) -> dict[str, list[dict]]:
    """Fetch all FRED series, using cache when fresh enough (< 6 hours old)."""
    await init_macro_tables()

    api_key = os.environ.get("FRED_API_KEY")
    if not api_key:
        raise RuntimeError("FRED_API_KEY not set in environment")

    fred = Fred(api_key=api_key)
    loop = asyncio.get_event_loop()

    results: dict[str, list[dict]] = {}
    for series_id, label in SERIES.items():
        obs = await loop.run_in_executor(None, _fetch_series_sync, fred, series_id)
        if obs:
            await upsert_series(series_id, obs)
        results[label] = await get_latest(series_id, n=365)

    return results


def _pct_change(series: list[dict], lag: int = 1) -> Optional[float]:
    """Return % change from lag periods ago to most recent."""
    vals = [r["value"] for r in series if r["value"] is not None]
    if len(vals) <= lag:
        return None
    return (vals[-1] - vals[-(lag + 1)]) / abs(vals[-(lag + 1)]) * 100


def _latest(series: list[dict]) -> Optional[float]:
    for r in reversed(series):
        if r["value"] is not None:
            return r["value"]
    return None


def classify_macro_regime(data: dict[str, list[dict]]) -> dict:
    """
    Convert raw series data into a compact macro regime object.
    This is what gets fed into every AI prompt.
    """
    yield_curve  = _latest(data.get("yield_curve", []))
    ten_y        = _latest(data.get("10y_yield", []))
    ten_y_3m_ago = next((r["value"] for r in reversed(data.get("10y_yield", [])[:-60]) if r["value"]), None)
    vix          = _latest(data.get("vix", []))
    hy_spread    = _latest(data.get("hy_spread", []))
    hy_3m_ago    = next((r["value"] for r in reversed(data.get("hy_spread", [])[:-60]) if r["value"]), None)
    m2_chg       = _pct_change(data.get("m2", []), lag=52)   # M2 is weekly; 52 lags = 1 year
    cpi_series   = data.get("cpi", [])
    cpi_yoy      = _pct_change(cpi_series, lag=12) if len(cpi_series) >= 13 else None
    unemployment = _latest(data.get("unemployment", []))
    dxy          = _latest(data.get("dxy", []))
    dxy_3m       = next((r["value"] for r in reversed(data.get("dxy", [])[:-60]) if r["value"]), None)

    # --- Yield curve regime ---
    if yield_curve is None:
        curve_regime = "UNKNOWN"
    elif yield_curve < -0.1:
        curve_regime = "INVERTED"   # recession risk
    elif yield_curve < 0.5:
        curve_regime = "FLAT"       # late cycle
    else:
        curve_regime = "STEEP"      # early/mid cycle, risk-on

    # --- Rate environment ---
    if ten_y is None or ten_y_3m_ago is None:
        rate_env = "UNKNOWN"
    elif ten_y > ten_y_3m_ago + 0.2:
        rate_env = "RISING"
    elif ten_y < ten_y_3m_ago - 0.2:
        rate_env = "FALLING"
    else:
        rate_env = "STABLE"

    # --- Credit stress ---
    if hy_spread is None:
        credit_stress = "UNKNOWN"
    elif hy_spread > 500:
        credit_stress = "STRESS"    # >500bps = distress
    elif hy_spread > 350:
        credit_stress = "ELEVATED"
    else:
        credit_stress = "NORMAL"

    # HY spread widening faster than 50bps in 3 months = leading indicator
    credit_widening = False
    if hy_spread and hy_3m_ago and (hy_spread - hy_3m_ago) > 50:
        credit_widening = True

    # --- VIX regime ---
    if vix is None:
        vix_regime = "UNKNOWN"
    elif vix > 30:
        vix_regime = "FEAR"
    elif vix > 20:
        vix_regime = "CAUTION"
    elif vix < 15:
        vix_regime = "COMPLACENCY"
    else:
        vix_regime = "NORMAL"

    # --- Dollar regime ---
    if dxy is None or dxy_3m is None:
        dxy_trend = "UNKNOWN"
    elif dxy > dxy_3m * 1.02:
        dxy_trend = "STRENGTHENING"  # bad for crypto, bad for EM
    elif dxy < dxy_3m * 0.98:
        dxy_trend = "WEAKENING"       # good for crypto, good for commodities
    else:
        dxy_trend = "STABLE"

    # --- M2 liquidity ---
    if m2_chg is None:
        liquidity = "UNKNOWN"
    elif m2_chg > 3:
        liquidity = "EXPANDING"   # risk-on, bull for crypto
    elif m2_chg < -1:
        liquidity = "CONTRACTING"  # bear for crypto
    else:
        liquidity = "FLAT"

    # --- Overall risk appetite ---
    bullish_factors = sum([
        curve_regime == "STEEP",
        rate_env == "FALLING",
        credit_stress == "NORMAL" and not credit_widening,
        vix_regime in ("NORMAL", "COMPLACENCY"),
        dxy_trend == "WEAKENING",
        liquidity == "EXPANDING",
    ])
    bearish_factors = sum([
        curve_regime == "INVERTED",
        rate_env == "RISING",
        credit_stress in ("ELEVATED", "STRESS") or credit_widening,
        vix_regime == "FEAR",
        dxy_trend == "STRENGTHENING",
        liquidity == "CONTRACTING",
    ])

    if bullish_factors >= 4:
        risk_appetite = "RISK_ON"
    elif bearish_factors >= 4:
        risk_appetite = "RISK_OFF"
    elif bearish_factors >= 2:
        risk_appetite = "CAUTIOUS"
    else:
        risk_appetite = "NEUTRAL"

    return {
        "rate_env":        rate_env,
        "curve":           curve_regime,
        "credit_stress":   credit_stress,
        "credit_widening": credit_widening,
        "vix":             round(vix, 1) if vix else None,
        "vix_regime":      vix_regime,
        "dxy_trend":       dxy_trend,
        "liquidity":       liquidity,
        "cpi_yoy":         round(cpi_yoy, 2) if cpi_yoy else None,
        "unemployment":    unemployment,
        "risk_appetite":   risk_appetite,
        "10y_yield":       round(ten_y, 3) if ten_y else None,
        "hy_spread_bps":   round(hy_spread, 0) if hy_spread else None,
        "m2_yoy_pct":      round(m2_chg, 1) if m2_chg else None,
        "computed_at":     datetime.utcnow().isoformat(),
    }


async def get_macro_regime(force_refresh: bool = False) -> dict:
    """Main entry point. Returns cached regime if fresh, else fetches and classifies."""
    if not force_refresh:
        cached = await load_cached_regime(max_age_hours=6.0)
        if cached:
            return cached

    data = await fetch_all_series()
    regime = classify_macro_regime(data)
    await cache_regime(regime)
    return regime


# --- CLI entry point (tagged stdout for C++ subprocess) ---
if __name__ == "__main__":
    import sys
    logging.basicConfig(level=logging.WARNING)
    regime = asyncio.run(get_macro_regime())
    print(f"MACRO_DATA:{json.dumps(regime)}", flush=True)
```

### 1.4 Test it

```bash
cd /path/to/repo/scripts
python -m macro.fred_collector
# → MACRO_DATA:{"rate_env": "STABLE", "curve": "FLAT", "risk_appetite": "CAUTIOUS", ...}
```

---

## Session 2 — COT Reports (~1.5 hours)

**What you'll have after**: Weekly CFTC futures positioning data for S&P, BTC, treasuries,
and dollar classified into extremes (record long/short → contrarian signals).

### 2.1 Create `scripts/macro/cot_collector.py`

```python
# scripts/macro/cot_collector.py
"""
Fetches CFTC Commitment of Traders reports and extracts net positioning
for key markets. COT is released every Friday ~3:30pm ET for prior Tuesday.

Markets tracked:
  E-MINI S&P 500   (CFTC code: 13874+)  → institutional equity sentiment
  BITCOIN CME      (CFTC code: 133741)   → institutional crypto stance
  10-YEAR T-NOTE   (CFTC code: 043602)   → rate expectations
  U.S. DOLLAR IDX  (CFTC code: 098662)   → macro dollar positioning
  GOLD             (CFTC code: 088691)   → safe haven demand
"""
import asyncio
import json
import logging
import os
import sqlite3
from datetime import datetime, timedelta
from typing import Optional

import aiosqlite

log = logging.getLogger(__name__)

DB_PATH = "data/sentinel.db"

# CFTC market codes (from legacy COT reports)
MARKETS = {
    "ES":    {"code": "13874+",  "label": "S&P 500 E-mini"},
    "BTC":   {"code": "133741",  "label": "Bitcoin CME"},
    "TY":    {"code": "043602",  "label": "10-Year T-Note"},
    "DX":    {"code": "098662",  "label": "Dollar Index"},
    "GC":    {"code": "088691",  "label": "Gold"},
}


async def _init_cot_table():
    os.makedirs(os.path.dirname(DB_PATH), exist_ok=True)
    async with aiosqlite.connect(DB_PATH) as db:
        await db.execute("""
            CREATE TABLE IF NOT EXISTS cot_positioning (
                market       TEXT NOT NULL,
                report_date  TEXT NOT NULL,
                large_spec_long  REAL,
                large_spec_short REAL,
                commercial_long  REAL,
                commercial_short REAL,
                small_spec_long  REAL,
                small_spec_short REAL,
                large_spec_net   REAL,
                commercial_net   REAL,
                fetched_at   TEXT NOT NULL,
                PRIMARY KEY (market, report_date)
            )
        """)
        await db.commit()


def _fetch_cot_sync(market_code: str, year: int = None) -> list[dict]:
    """
    Downloads COT legacy futures CSV from CFTC and filters for our market.
    Falls back to cot-reports library if available, else direct HTTP.
    """
    try:
        import cot_reports as cot
        df = cot.cot_year(year=year or datetime.now().year, cot_report_type="legacy_fut")
        filtered = df[df["Market_and_Exchange_Names"].str.contains(market_code, na=False)]
        if filtered.empty and year is None:
            # Try previous year as fallback
            prev = cot.cot_year(year=datetime.now().year - 1, cot_report_type="legacy_fut")
            filtered = prev[prev["Market_and_Exchange_Names"].str.contains(market_code, na=False)]

        rows = []
        for _, r in filtered.iterrows():
            try:
                rows.append({
                    "date":            str(r.get("As_of_Date_In_Form_YYMMDD", r.get("Report_Date_as_YYYY-MM-DD", ""))),
                    "large_spec_long": float(r.get("NonComm_Positions_Long_All", 0)),
                    "large_spec_short":float(r.get("NonComm_Positions_Short_All", 0)),
                    "commercial_long": float(r.get("Comm_Positions_Long_All", 0)),
                    "commercial_short":float(r.get("Comm_Positions_Short_All", 0)),
                    "small_spec_long": float(r.get("NonRept_Positions_Long_All", 0)),
                    "small_spec_short":float(r.get("NonRept_Positions_Short_All", 0)),
                })
            except Exception:
                continue
        return rows
    except ImportError:
        log.warning("cot-reports not installed. Run: uv pip install cot-reports")
        return []
    except Exception as e:
        log.error(f"COT fetch failed for {market_code}: {e}")
        return []


async def fetch_cot_data(force: bool = False) -> dict[str, list[dict]]:
    await _init_cot_table()
    loop = asyncio.get_event_loop()
    results = {}

    for symbol, meta in MARKETS.items():
        rows = await loop.run_in_executor(None, _fetch_cot_sync, meta["code"])
        if not rows:
            continue

        now = datetime.utcnow().isoformat()
        async with aiosqlite.connect(DB_PATH) as db:
            for r in rows:
                ls_net = r["large_spec_long"] - r["large_spec_short"]
                comm_net = r["commercial_long"] - r["commercial_short"]
                await db.execute("""
                    INSERT OR REPLACE INTO cot_positioning
                    (market, report_date, large_spec_long, large_spec_short,
                     commercial_long, commercial_short, small_spec_long, small_spec_short,
                     large_spec_net, commercial_net, fetched_at)
                    VALUES (?,?,?,?,?,?,?,?,?,?,?)
                """, (symbol, r["date"], r["large_spec_long"], r["large_spec_short"],
                      r["commercial_long"], r["commercial_short"],
                      r["small_spec_long"], r["small_spec_short"],
                      ls_net, comm_net, now))
            await db.commit()

        results[symbol] = rows
    return results


async def get_cot_signals() -> dict[str, dict]:
    """
    Returns compact COT signal for each market:
      net_position, net_change_wk, percentile_3y, signal
    """
    await _init_cot_table()
    signals = {}

    for symbol in MARKETS:
        async with aiosqlite.connect(DB_PATH) as db:
            db.row_factory = aiosqlite.Row
            async with db.execute(
                "SELECT report_date, large_spec_net FROM cot_positioning "
                "WHERE market=? ORDER BY report_date DESC LIMIT 156",  # 3 years of weekly
                (symbol,)
            ) as cur:
                rows = await cur.fetchall()

        if not rows:
            signals[symbol] = {"signal": "NO_DATA"}
            continue

        latest_net = rows[0]["large_spec_net"]
        prev_net   = rows[1]["large_spec_net"] if len(rows) > 1 else latest_net
        all_nets   = [r["large_spec_net"] for r in rows if r["large_spec_net"] is not None]

        # Percentile of current positioning vs 3-year history
        if len(all_nets) >= 10:
            rank = sum(1 for v in all_nets if v < latest_net)
            percentile = round(rank / len(all_nets) * 100, 0)
        else:
            percentile = None

        # Classify signal
        if percentile is not None:
            if percentile >= 90:
                sig = "EXTREME_LONG"    # contrarian bearish
            elif percentile >= 75:
                sig = "NET_LONG"
            elif percentile <= 10:
                sig = "EXTREME_SHORT"   # contrarian bullish
            elif percentile <= 25:
                sig = "NET_SHORT"
            else:
                sig = "NEUTRAL"
        else:
            sig = "INSUFFICIENT_DATA"

        signals[symbol] = {
            "net_position":   round(latest_net, 0),
            "net_change_wk":  round(latest_net - prev_net, 0),
            "percentile_3y":  percentile,
            "signal":         sig,
            "label":          MARKETS[symbol]["label"],
            "report_date":    rows[0]["report_date"],
        }

    return signals


async def get_cot_context() -> dict:
    """Compact summary for AI prompt."""
    signals = await get_cot_signals()
    return {
        "es_positioning":  signals.get("ES", {}),
        "btc_positioning": signals.get("BTC", {}),
        "treasury_10y":    signals.get("TY", {}),
        "dollar":          signals.get("DX", {}),
        "gold":            signals.get("GC", {}),
    }


if __name__ == "__main__":
    import sys
    logging.basicConfig(level=logging.WARNING)

    async def main():
        print("Fetching COT data (downloads ~year of CFTC CSVs, takes ~30s first run)...")
        await fetch_cot_data()
        ctx = await get_cot_context()
        print(f"COT_DATA:{json.dumps(ctx)}", flush=True)

    asyncio.run(main())
```

### 2.2 Test it

```bash
python -m macro.cot_collector
# First run downloads the full year CSV from CFTC (~30s)
# → COT_DATA:{"es_positioning": {"net_position": 45231, "percentile_3y": 72, "signal": "NET_LONG"}, ...}
```

---

## Session 3 — Alpha Vantage Intraday Stock Candles (~1 hour)

**What you'll have after**: 1-hour candles for up to 5-10 watchlist stocks,
aggressively cached to stay within the 25 req/day free limit.

### 3.1 Create `scripts/stocks/alpha_vantage.py`

```python
# scripts/stocks/alpha_vantage.py
"""
Alpha Vantage free tier: 25 requests/day.
Strategy: fetch 1h candles, cache per (symbol, month), only re-fetch when stale.
At 25 req/day you can cover ~12 symbols/day with monthly 1h data.
"""
import aiohttp
import asyncio
import json
import logging
import os
from datetime import datetime, timedelta
from typing import Optional

import aiosqlite
from dotenv import load_dotenv

load_dotenv()
log = logging.getLogger(__name__)

DB_PATH    = "data/sentinel.db"
BASE_URL   = "https://www.alphavantage.co/query"
MAX_DAILY_REQUESTS = 24  # leave 1 in reserve


async def _init_av_tables():
    os.makedirs(os.path.dirname(DB_PATH), exist_ok=True)
    async with aiosqlite.connect(DB_PATH) as db:
        await db.execute("""
            CREATE TABLE IF NOT EXISTS av_candles (
                symbol    TEXT NOT NULL,
                interval  TEXT NOT NULL,
                ts        TEXT NOT NULL,
                open      REAL, high REAL, low REAL, close REAL, volume REAL,
                PRIMARY KEY (symbol, interval, ts)
            )
        """)
        await db.execute("""
            CREATE TABLE IF NOT EXISTS av_request_log (
                date      TEXT NOT NULL,
                count     INTEGER NOT NULL DEFAULT 0,
                PRIMARY KEY (date)
            )
        """)
        await db.commit()


async def _requests_today() -> int:
    today = datetime.utcnow().strftime("%Y-%m-%d")
    async with aiosqlite.connect(DB_PATH) as db:
        db.row_factory = aiosqlite.Row
        async with db.execute(
            "SELECT count FROM av_request_log WHERE date=?", (today,)
        ) as cur:
            row = await cur.fetchone()
    return row["count"] if row else 0


async def _log_request():
    today = datetime.utcnow().strftime("%Y-%m-%d")
    async with aiosqlite.connect(DB_PATH) as db:
        await db.execute("""
            INSERT INTO av_request_log (date, count) VALUES (?, 1)
            ON CONFLICT(date) DO UPDATE SET count = count + 1
        """, (today,))
        await db.commit()


async def _fetch_intraday(symbol: str, interval: str = "60min",
                          month: str = None) -> list[dict]:
    """
    One API call. month format: "YYYY-MM" (if None, fetches most recent 30 days).
    Returns list of {ts, open, high, low, close, volume}.
    """
    api_key = os.environ.get("ALPHA_VANTAGE_KEY")
    if not api_key:
        raise RuntimeError("ALPHA_VANTAGE_KEY not set")

    reqs = await _requests_today()
    if reqs >= MAX_DAILY_REQUESTS:
        log.warning(f"Alpha Vantage daily limit reached ({reqs} requests). Using cache only.")
        return []

    params = {
        "function":   "TIME_SERIES_INTRADAY",
        "symbol":     symbol,
        "interval":   interval,
        "outputsize": "full",
        "datatype":   "json",
        "apikey":     api_key,
    }
    if month:
        params["month"] = month

    async with aiohttp.ClientSession() as session:
        async with session.get(BASE_URL, params=params, timeout=aiohttp.ClientTimeout(total=30)) as resp:
            data = await resp.json()

    await _log_request()

    ts_key = f"Time Series ({interval})"
    if ts_key not in data:
        note = data.get("Note", data.get("Information", "unknown error"))
        log.error(f"Alpha Vantage error for {symbol}: {note}")
        return []

    candles = []
    for ts_str, bar in data[ts_key].items():
        candles.append({
            "ts":     ts_str,
            "open":   float(bar["1. open"]),
            "high":   float(bar["2. high"]),
            "low":    float(bar["3. low"]),
            "close":  float(bar["4. close"]),
            "volume": float(bar["5. volume"]),
        })
    candles.sort(key=lambda x: x["ts"])
    return candles


async def _store_candles(symbol: str, interval: str, candles: list[dict]):
    async with aiosqlite.connect(DB_PATH) as db:
        for c in candles:
            await db.execute("""
                INSERT OR REPLACE INTO av_candles (symbol, interval, ts, open, high, low, close, volume)
                VALUES (?,?,?,?,?,?,?,?)
            """, (symbol.upper(), interval, c["ts"], c["open"], c["high"], c["low"], c["close"], c["volume"]))
        await db.commit()


async def get_candles(symbol: str, interval: str = "60min",
                      bars: int = 200) -> list[dict]:
    """
    Returns up to `bars` candles from cache. Fetches from API if cache is stale
    (most recent cached candle is > 2 hours old for 1h data).
    """
    await _init_av_tables()
    symbol = symbol.upper()

    # Check cache freshness
    async with aiosqlite.connect(DB_PATH) as db:
        db.row_factory = aiosqlite.Row
        async with db.execute(
            "SELECT ts FROM av_candles WHERE symbol=? AND interval=? ORDER BY ts DESC LIMIT 1",
            (symbol, interval),
        ) as cur:
            latest = await cur.fetchone()

    needs_fetch = True
    if latest:
        try:
            last_ts = datetime.strptime(latest["ts"], "%Y-%m-%d %H:%M:%S")
            age_hours = (datetime.utcnow() - last_ts).total_seconds() / 3600
            # For 1h bars, stale after 2h; for daily no need to refetch same day
            stale_threshold = 2.0 if interval == "60min" else 24.0
            needs_fetch = age_hours > stale_threshold
        except ValueError:
            needs_fetch = True

    if needs_fetch:
        candles = await _fetch_intraday(symbol, interval)
        if candles:
            await _store_candles(symbol, interval, candles)

    # Read from cache
    async with aiosqlite.connect(DB_PATH) as db:
        db.row_factory = aiosqlite.Row
        async with db.execute(
            "SELECT ts, open, high, low, close, volume FROM av_candles "
            "WHERE symbol=? AND interval=? ORDER BY ts DESC LIMIT ?",
            (symbol, interval, bars),
        ) as cur:
            rows = await cur.fetchall()

    return [dict(r) for r in reversed(rows)]


async def get_stock_summary(symbol: str) -> dict:
    """Compact summary of recent price action for AI prompt."""
    candles = await get_candles(symbol, interval="60min", bars=100)
    if not candles:
        candles = await get_candles(symbol, interval="60min", bars=100)  # retry

    if not candles:
        return {"symbol": symbol, "status": "NO_DATA"}

    closes = [c["close"] for c in candles]
    volumes = [c["volume"] for c in candles]
    latest = candles[-1]
    prev   = candles[-2] if len(candles) > 1 else candles[-1]

    # Simple trend: slope of last 20 closes
    if len(closes) >= 20:
        subset = closes[-20:]
        slope = (subset[-1] - subset[0]) / subset[0] * 100
    else:
        slope = None

    avg_vol = sum(volumes) / len(volumes) if volumes else 0
    rel_vol = latest["volume"] / avg_vol if avg_vol > 0 else 1.0

    return {
        "symbol":       symbol,
        "last_close":   latest["close"],
        "change_pct":   round((latest["close"] - prev["close"]) / prev["close"] * 100, 2),
        "slope_20h":    round(slope, 2) if slope else None,
        "rel_volume":   round(rel_vol, 2),
        "high_100h":    max(c["high"] for c in candles),
        "low_100h":     min(c["low"] for c in candles),
        "bars_cached":  len(candles),
        "source":       "alpha_vantage_1h",
    }


if __name__ == "__main__":
    import sys
    logging.basicConfig(level=logging.WARNING)
    symbol = sys.argv[1] if len(sys.argv) > 1 else "AAPL"

    async def main():
        summary = await get_stock_summary(symbol)
        print(f"AV_DATA:{json.dumps(summary)}", flush=True)

    asyncio.run(main())
```

### 3.2 Test it

```bash
python -m stocks.alpha_vantage NVDA
# → AV_DATA:{"symbol": "NVDA", "last_close": 875.4, "slope_20h": 3.2, "rel_volume": 1.8, ...}
```

---

## Session 4 — AI Pipeline Core (~2.5 hours)

**What you'll have after**: A single Python function `gather_context(symbol, asset_type,
timeframe)` that pulls all available data and returns a compact structured dict ready
to send to Claude.

### 4.1 Directory structure

```bash
mkdir -p scripts/ai/collectors
touch scripts/ai/__init__.py
touch scripts/ai/collectors/__init__.py
```

### 4.2 Create `scripts/ai/collectors/sec_collector.py`

Thin wrapper over the existing SEC scripts — no reimplementation needed.

```python
# scripts/ai/collectors/sec_collector.py
"""
Wraps existing scripts/sec/ modules. Produces compact objects for AI prompt.
"""
import sys
import os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

import asyncio
import logging
from typing import Optional
from sec.sec_api import SECDataFetcher

log = logging.getLogger(__name__)
_fetcher: Optional[SECDataFetcher] = None


def _get_fetcher() -> SECDataFetcher:
    global _fetcher
    if _fetcher is None:
        _fetcher = SECDataFetcher()
    return _fetcher


async def get_sec_context(ticker: str) -> dict:
    """
    Returns compact SEC context for one stock ticker.
    Covers insider transactions, recent 8-Ks, financial regime.
    """
    fetcher = _get_fetcher()
    ticker = ticker.upper()

    # Run all three fetches concurrently
    insider_task   = fetcher.analyze_insider_transactions(ticker, days_back=90)
    filings_task   = fetcher.fetch_current_reports(ticker, days_back=30)
    financials_task = fetcher.get_financial_summary(ticker)

    insider, filings, financials = await asyncio.gather(
        insider_task, filings_task, financials_task,
        return_exceptions=True
    )

    # Insider summary
    insider_summary = {"status": "ERROR"}
    if isinstance(insider, dict) and "error" not in insider:
        total_buy_val  = insider.get("total_buy_value", 0) or 0
        total_sell_val = insider.get("total_sell_value", 0) or 0
        net = total_buy_val - total_sell_val
        insiders_buying  = insider.get("unique_buyers", 0) or 0
        insiders_selling = insider.get("unique_sellers", 0) or 0

        if net > 500_000 and insiders_buying >= 2:
            pressure = "CLUSTER_BUY"
        elif net > 0:
            pressure = "NET_BUYING"
        elif net < -1_000_000 and insiders_selling >= 2:
            pressure = "CLUSTER_SELL"
        elif net < 0:
            pressure = "NET_SELLING"
        else:
            pressure = "NEUTRAL"

        insider_summary = {
            "pressure":        pressure,
            "net_value_usd":   round(net, 0),
            "buyers":          insiders_buying,
            "sellers":         insiders_selling,
            "total_buy_usd":   round(total_buy_val, 0),
            "total_sell_usd":  round(total_sell_val, 0),
            "days_back":       90,
        }

    # Recent 8-K events (material items)
    events = []
    if isinstance(filings, list):
        for f in filings[:5]:
            events.append({
                "date":  f.get("filing_date"),
                "form":  f.get("form"),
                "desc":  f.get("primary_document_description", ""),
            })

    # Financial regime
    fin_summary = {}
    if isinstance(financials, dict) and financials:
        fin_summary = {
            "revenue_trend":   financials.get("revenue_trend"),
            "net_income_trend":financials.get("net_income_trend"),
            "gross_margin":    financials.get("gross_margin_latest"),
            "cash_ratio":      financials.get("cash_to_debt"),
            "regime":          financials.get("financial_regime", "UNKNOWN"),
        }

    return {
        "ticker":           ticker,
        "insider":          insider_summary,
        "recent_filings":   events,
        "financials":       fin_summary,
    }
```

### 4.3 Create `scripts/ai/collectors/screener_collector.py`

```python
# scripts/ai/collectors/screener_collector.py
import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

import asyncio
import logging
from screener.screener_core import build_screener, screener_to_rows

log = logging.getLogger(__name__)


def _fetch_screener_sync(symbol: str, asset_type: str) -> dict:
    """Fetch screener data for a specific symbol."""
    try:
        if asset_type == "crypto":
            extra = ["RSI", "RSI_1H", "RSI_4H", "EMA20", "EMA50", "EMA200", "MACD_4H", "VWAP"]
        else:
            extra = ["RSI", "EMA20", "EMA50", "EMA200", "MACD", "ADX", "ATR"]

        screener = build_screener(asset_type, extra, min_volume=0, limit=200)
        df = screener.get()
        rows = screener_to_rows(df)

        # Find our symbol in results
        sym_upper = symbol.upper()
        for row in rows:
            if row.get("symbol", "").upper().startswith(sym_upper):
                return row

        log.warning(f"Symbol {symbol} not found in screener results")
        return {}
    except Exception as e:
        log.error(f"Screener fetch failed for {symbol}: {e}")
        return {}


async def get_screener_context(symbol: str, asset_type: str) -> dict:
    loop = asyncio.get_event_loop()
    raw = await loop.run_in_executor(None, _fetch_screener_sync, symbol, asset_type)
    if not raw:
        return {"status": "NO_DATA"}

    # Classify EMA stack
    price = raw.get("close") or raw.get("price") or 0
    ema20 = raw.get("EMA20_240") or raw.get("EXPONENTIAL_MOVING_AVERAGE_20") or 0
    ema50 = raw.get("EMA50_240") or raw.get("EXPONENTIAL_MOVING_AVERAGE_50") or 0
    ema200 = raw.get("EMA200_240") or raw.get("EXPONENTIAL_MOVING_AVERAGE_200") or 0

    if all([price, ema20, ema50, ema200]):
        if price > ema20 > ema50 > ema200:
            ema_stack = "FULL_BULL"
        elif price < ema20 < ema50 < ema200:
            ema_stack = "FULL_BEAR"
        elif price > ema200:
            ema_stack = "ABOVE_200_MIXED"
        else:
            ema_stack = "BELOW_200_MIXED"
    else:
        ema_stack = "UNKNOWN"

    rsi = raw.get("RSI_240") or raw.get("RELATIVE_STRENGTH_INDEX_14")
    adx = raw.get("AVERAGE_DIRECTIONAL_INDEX_14")
    rel_vol = raw.get("RELATIVE_VOLUME") or raw.get("relative_volume_10d_calc")

    return {
        "price":      price,
        "change_pct": raw.get("CHANGE_PERCENT") or raw.get("change_percent"),
        "rsi":        round(rsi, 1) if rsi else None,
        "rsi_regime": "OVERBOUGHT" if rsi and rsi > 70 else ("OVERSOLD" if rsi and rsi < 30 else "NEUTRAL"),
        "ema_stack":  ema_stack,
        "adx":        round(adx, 1) if adx else None,
        "trending":   adx > 25 if adx else None,
        "rel_volume": round(rel_vol, 2) if rel_vol else None,
        "market_cap": raw.get("MARKET_CAP") or raw.get("market_cap_calc"),
    }
```

### 4.4 Create `scripts/ai/collectors/macro_collector.py`

```python
# scripts/ai/collectors/macro_collector.py
import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

import asyncio
from macro.fred_collector import get_macro_regime
from macro.cot_collector import get_cot_context


async def get_macro_context() -> dict:
    regime, cot = await asyncio.gather(
        get_macro_regime(),
        get_cot_context(),
        return_exceptions=True
    )
    return {
        "macro_regime": regime if isinstance(regime, dict) else {"status": "ERROR"},
        "cot":          cot   if isinstance(cot, dict)    else {"status": "ERROR"},
    }
```

### 4.5 Create `scripts/ai/data_aggregator.py`

```python
# scripts/ai/data_aggregator.py
"""
Orchestrates all collectors into a single context dict for the AI pipeline.
Runs all I/O concurrently. Handles partial failures gracefully.
"""
import asyncio
import logging
import sys, os
sys.path.insert(0, os.path.dirname(os.path.dirname(__file__)))

from collectors.sec_collector      import get_sec_context
from collectors.screener_collector import get_screener_context
from collectors.macro_collector    import get_macro_context

log = logging.getLogger(__name__)


async def gather_context(symbol: str, asset_type: str, timeframe: str = "1d",
                         include_macro: bool = True) -> dict:
    """
    Gathers all available context for (symbol, asset_type).

    asset_type: "crypto" | "stock"
    Returns a flat dict ready to pass to prompt_builder.
    """
    tasks = {
        "screener": get_screener_context(symbol, asset_type),
    }

    if asset_type == "stock":
        tasks["sec"] = get_sec_context(symbol)

    if include_macro:
        tasks["macro"] = get_macro_context()

    results = {}
    coros = list(tasks.values())
    keys  = list(tasks.keys())

    settled = await asyncio.gather(*coros, return_exceptions=True)
    for key, result in zip(keys, settled):
        if isinstance(result, Exception):
            log.warning(f"Collector '{key}' failed: {result}")
            results[key] = {"status": "ERROR", "error": str(result)}
        else:
            results[key] = result

    return {
        "symbol":    symbol,
        "asset_type": asset_type,
        "timeframe": timeframe,
        **results,
    }
```

### 4.6 Test the aggregator

```bash
# From scripts/ directory
python -c "
import asyncio, sys, os
sys.path.insert(0, 'ai')
from ai.data_aggregator import gather_context
import json
ctx = asyncio.run(gather_context('AAPL', 'stock'))
print(json.dumps(ctx, indent=2, default=str))
"
```

---

## Session 5 — Regime Detector (~1.5 hours)

**What you'll have after**: An algorithmic layer that takes the aggregated context
and outputs a structured `RegimeSignal` object — the safety net that grounds
the LLM so it can't hallucinate a regime.

### 5.1 Create `scripts/ai/regime_detector.py`

```python
# scripts/ai/regime_detector.py
"""
Pure algorithmic regime detection. No LLM. No network I/O.
Input:  aggregated context dict from data_aggregator.py
Output: RegimeSignal dict with regime_type, confidence, anomaly_flags
"""
from dataclasses import dataclass, field, asdict
from typing import Literal
import json


@dataclass
class AnomalyFlag:
    code:        str
    severity:    Literal["INFO", "CAUTION", "WARNING", "ALERT"]
    description: str
    source:      str


@dataclass
class RegimeSignal:
    regime_type:       str   # TRENDING_UP / TRENDING_DOWN / RANGING / ACCUMULATION / DISTRIBUTION / UNKNOWN
    regime_confidence: float # 0-1
    anomaly_flags:     list[AnomalyFlag] = field(default_factory=list)
    insider_pressure:  str = "N/A"
    macro_bias:        str = "NEUTRAL"
    technical_bias:    str = "NEUTRAL"
    key_notes:         list[str] = field(default_factory=list)

    def to_dict(self) -> dict:
        d = asdict(self)
        d["anomaly_flags"] = [asdict(f) for f in self.anomaly_flags]
        return d


def detect_regime(ctx: dict) -> RegimeSignal:
    """
    Classifies market regime from aggregated context.
    Uses a scoring system: each signal votes for a regime.
    """
    flags: list[AnomalyFlag] = []
    notes: list[str] = []

    screener  = ctx.get("screener", {})
    sec       = ctx.get("sec", {})
    macro     = ctx.get("macro", {})
    asset     = ctx.get("asset_type", "stock")

    # ── Technical signals ──────────────────────────────────────────
    rsi       = screener.get("rsi")
    ema_stack = screener.get("ema_stack", "UNKNOWN")
    adx       = screener.get("adx")
    rel_vol   = screener.get("rel_volume", 1.0) or 1.0
    trending  = screener.get("trending", False)

    bull_tech = bear_tech = 0

    if ema_stack == "FULL_BULL":
        bull_tech += 2
    elif ema_stack == "FULL_BEAR":
        bear_tech += 2
    elif ema_stack == "ABOVE_200_MIXED":
        bull_tech += 1
    elif ema_stack == "BELOW_200_MIXED":
        bear_tech += 1

    if rsi:
        if rsi > 70:
            flags.append(AnomalyFlag("RSI_OVERBOUGHT", "CAUTION",
                f"RSI at {rsi:.0f} — overbought territory", "screener"))
            bear_tech += 1
        elif rsi < 30:
            flags.append(AnomalyFlag("RSI_OVERSOLD", "CAUTION",
                f"RSI at {rsi:.0f} — oversold territory", "screener"))
            bull_tech += 1

    if rel_vol and rel_vol > 3.0:
        flags.append(AnomalyFlag("RELATIVE_VOLUME_SPIKE", "WARNING",
            f"Relative volume {rel_vol:.1f}x average — unusual activity", "screener"))

    # ── Insider signals (stocks only) ───────────────────────────────
    insider_pressure = "N/A"
    if asset == "stock" and isinstance(sec, dict):
        insider = sec.get("insider", {})
        pressure = insider.get("pressure", "NEUTRAL")
        insider_pressure = pressure

        if pressure == "CLUSTER_BUY":
            bull_tech += 2
            flags.append(AnomalyFlag("INSIDER_CLUSTER_BUY", "ALERT",
                f"{insider.get('buyers',0)} insiders buying, net +${insider.get('net_value_usd',0):,.0f} in 90d",
                "sec_form4"))
        elif pressure == "NET_BUYING":
            bull_tech += 1
            notes.append(f"Net insider buying: +${insider.get('net_value_usd',0):,.0f}")
        elif pressure == "CLUSTER_SELL":
            bear_tech += 2
            flags.append(AnomalyFlag("INSIDER_CLUSTER_SELL", "ALERT",
                f"{insider.get('sellers',0)} insiders selling, net -${abs(insider.get('net_value_usd',0)):,.0f} in 90d",
                "sec_form4"))
        elif pressure == "NET_SELLING":
            bear_tech += 1
            notes.append(f"Net insider selling: -${abs(insider.get('net_value_usd',0)):,.0f}")

        # Recent 8-K events
        for filing in sec.get("recent_filings", []):
            desc = filing.get("desc", "").lower()
            if any(w in desc in desc for w in ["material weakness", "going concern", "departure", "resignation"]):
                flags.append(AnomalyFlag("MATERIAL_8K_EVENT", "WARNING",
                    f"8-K filed {filing.get('date')}: {filing.get('desc','')}", "sec_8k"))
                bear_tech += 1
            elif any(w in desc for w in ["agreement", "partnership", "acquisition"]):
                flags.append(AnomalyFlag("MATERIAL_8K_POSITIVE", "INFO",
                    f"8-K filed {filing.get('date')}: {filing.get('desc','')}", "sec_8k"))
                bull_tech += 1

    # ── Macro signals ────────────────────────────────────────────────
    macro_bias = "NEUTRAL"
    macro_regime = macro.get("macro_regime", {})
    risk_appetite = macro_regime.get("risk_appetite", "NEUTRAL")
    credit_stress = macro_regime.get("credit_stress", "NORMAL")
    credit_widening = macro_regime.get("credit_widening", False)

    if risk_appetite == "RISK_ON":
        macro_bias = "BULLISH"
    elif risk_appetite == "RISK_OFF":
        macro_bias = "BEARISH"
        flags.append(AnomalyFlag("MACRO_RISK_OFF", "CAUTION",
            f"Macro: {macro_regime.get('curve')} curve, {macro_regime.get('vix_regime')} VIX, "
            f"{macro_regime.get('dxy_trend')} dollar", "fred"))
    elif risk_appetite == "CAUTIOUS":
        macro_bias = "CAUTIOUS"

    if credit_widening:
        flags.append(AnomalyFlag("CREDIT_SPREAD_WIDENING", "WARNING",
            f"HY credit spread widening (now {macro_regime.get('hy_spread_bps')}bps) — leads equity by 2-4 weeks",
            "fred"))

    # COT extremes
    cot = macro.get("cot", {})
    if asset == "stock":
        es = cot.get("es_positioning", {})
        if es.get("signal") == "EXTREME_LONG":
            flags.append(AnomalyFlag("COT_ES_EXTREME_LONG", "CAUTION",
                f"S&P futures at {es.get('percentile_3y')}th percentile net long — historically precedes corrections",
                "cftc_cot"))
        elif es.get("signal") == "EXTREME_SHORT":
            flags.append(AnomalyFlag("COT_ES_EXTREME_SHORT", "INFO",
                f"S&P futures at {es.get('percentile_3y')}th percentile net short — contrarian bullish",
                "cftc_cot"))
    elif asset == "crypto":
        btc = cot.get("btc_positioning", {})
        if btc.get("signal") == "EXTREME_LONG":
            flags.append(AnomalyFlag("COT_BTC_EXTREME_LONG", "CAUTION",
                f"BTC CME futures at {btc.get('percentile_3y')}th percentile net long",
                "cftc_cot"))

    # ── Classify regime ──────────────────────────────────────────────
    net_score = bull_tech - bear_tech

    if not trending and adx and adx < 20:
        if net_score >= 1:
            regime = "ACCUMULATION"
            confidence = 0.5 + min(net_score * 0.1, 0.35)
        elif net_score <= -1:
            regime = "DISTRIBUTION"
            confidence = 0.5 + min(abs(net_score) * 0.1, 0.35)
        else:
            regime = "RANGING"
            confidence = 0.6
    elif net_score >= 3:
        regime = "TRENDING_UP"
        confidence = 0.5 + min(net_score * 0.08, 0.4)
    elif net_score <= -3:
        regime = "TRENDING_DOWN"
        confidence = 0.5 + min(abs(net_score) * 0.08, 0.4)
    elif net_score >= 1:
        regime = "ACCUMULATION"
        confidence = 0.45 + net_score * 0.08
    elif net_score <= -1:
        regime = "DISTRIBUTION"
        confidence = 0.45 + abs(net_score) * 0.08
    else:
        regime = "RANGING"
        confidence = 0.5

    technical_bias = "BULLISH" if net_score > 0 else ("BEARISH" if net_score < 0 else "NEUTRAL")

    return RegimeSignal(
        regime_type=regime,
        regime_confidence=round(min(confidence, 0.95), 2),
        anomaly_flags=flags,
        insider_pressure=insider_pressure,
        macro_bias=macro_bias,
        technical_bias=technical_bias,
        key_notes=notes,
    )
```

---

## Session 6 — Claude LLM Integration (~2 hours)

**What you'll have after**: A complete end-to-end pipeline. Pass in a symbol,
get back a narrative + chart annotations.

### 6.1 Create `scripts/ai/prompt_builder.py`

```python
# scripts/ai/prompt_builder.py
"""
Assembles structured context into the Claude prompt.
Rule: send numbers and labels, not prose. Let Claude generate the narrative.
"""
import json
from datetime import datetime

SYSTEM_PROMPT = """You are a senior quantitative market analyst embedded in a
professional trading terminal called Sentinel.

Your role: synthesize structured market signals into a clear, actionable narrative
that helps traders distinguish between algorithmic noise and informed price action.

Rules:
- Be specific: cite actual numbers from the data
- Be direct: 2-4 sentence narrative max
- Cite the source of each claim (e.g. "per Form 4", "per FRED", "per COT")
- Never generate financial advice or price targets
- When multiple signals conflict, say so explicitly
- Confidence language: "strongly suggests", "consistent with", "inconclusive"
- If data is missing or insufficient, say "insufficient data" for that signal

You MUST respond in this exact JSON format, no markdown, no preamble:
{
  "narrative": "string",
  "annotations": [
    {
      "type": "ZONE|LINE|LABEL|EVENT_MARKER",
      "price_low": float_or_null,
      "price_high": float_or_null,
      "timestamp": "ISO8601_or_null",
      "label": "string (max 40 chars)",
      "severity": "INFO|CAUTION|WARNING|ALERT",
      "color_hint": "BULLISH|BEARISH|NEUTRAL|INSIDER|MACRO"
    }
  ],
  "alerts": ["string"]
}"""


def build_prompt(ctx: dict, regime) -> str:
    symbol    = ctx.get("symbol", "UNKNOWN")
    asset     = ctx.get("asset_type", "stock")
    timeframe = ctx.get("timeframe", "1d")
    screener  = ctx.get("screener", {})
    sec       = ctx.get("sec", {})
    macro_ctx = ctx.get("macro", {})
    macro_regime = macro_ctx.get("macro_regime", {})
    cot       = macro_ctx.get("cot", {})

    regime_dict = regime.to_dict() if hasattr(regime, "to_dict") else regime

    # Format anomaly flags concisely
    flags_str = ""
    for f in regime_dict.get("anomaly_flags", []):
        flags_str += f"  [{f['severity']}] {f['code']}: {f['description']}\n"
    if not flags_str:
        flags_str = "  None detected\n"

    # Format insider data
    insider = sec.get("insider", {}) if isinstance(sec, dict) else {}
    insider_str = (
        f"  Pressure: {insider.get('pressure','N/A')}\n"
        f"  Net value 90d: ${insider.get('net_value_usd', 0):,.0f}\n"
        f"  Buyers: {insider.get('buyers',0)}, Sellers: {insider.get('sellers',0)}\n"
    ) if insider else "  Not available (crypto or no data)\n"

    # Format recent filings
    filings_str = ""
    for f in (sec.get("recent_filings", []) if isinstance(sec, dict) else [])[:3]:
        filings_str += f"  {f.get('date','')}: {f.get('form','')} — {f.get('desc','')}\n"
    if not filings_str:
        filings_str = "  None in last 30 days\n"

    # Format COT
    es   = cot.get("es_positioning", {})
    btc  = cot.get("btc_positioning", {})
    cot_str = (
        f"  S&P futures: {es.get('signal','N/A')} ({es.get('percentile_3y','?')}th pct, {es.get('report_date','?')})\n"
        f"  BTC futures: {btc.get('signal','N/A')} ({btc.get('percentile_3y','?')}th pct)\n"
    )

    return f"""Analyze {symbol} ({asset.upper()}) on {timeframe} timeframe as of {datetime.utcnow().strftime('%Y-%m-%d %H:%MZ')}.

## Detected Regime
- Type: {regime_dict.get('regime_type','UNKNOWN')} (confidence: {regime_dict.get('regime_confidence',0):.0%})
- Technical bias: {regime_dict.get('technical_bias','NEUTRAL')}
- Insider pressure: {regime_dict.get('insider_pressure','N/A')}
- Macro bias: {regime_dict.get('macro_bias','NEUTRAL')}

## Anomaly Flags
{flags_str}
## Technical Signals (TradingView Screener)
  RSI: {screener.get('rsi','N/A')} ({screener.get('rsi_regime','N/A')})
  EMA stack: {screener.get('ema_stack','N/A')}
  ADX: {screener.get('adx','N/A')} ({'trending' if screener.get('trending') else 'ranging/choppy'})
  Relative volume: {screener.get('rel_volume','N/A')}x

## Insider Activity — SEC Form 4 (90-day window)
{insider_str}
## Recent 8-K Filings (30 days)
{filings_str}
## Macro Regime (FRED)
  Rate environment: {macro_regime.get('rate_env','N/A')}
  Yield curve: {macro_regime.get('curve','N/A')}
  VIX: {macro_regime.get('vix','N/A')} ({macro_regime.get('vix_regime','N/A')})
  Credit stress: {macro_regime.get('credit_stress','N/A')}{' ⚠ WIDENING' if macro_regime.get('credit_widening') else ''}
  DXY trend: {macro_regime.get('dxy_trend','N/A')}
  Liquidity (M2): {macro_regime.get('liquidity','N/A')}
  Risk appetite: {macro_regime.get('risk_appetite','N/A')}

## CFTC COT Positioning
{cot_str}"""
```

### 6.2 Create `scripts/ai/claude_client.py`

```python
# scripts/ai/claude_client.py
import json
import logging
import os
import re

import anthropic
from dotenv import load_dotenv

load_dotenv()
log = logging.getLogger(__name__)

MODEL = "claude-sonnet-4-6"


class ClaudeClient:
    def __init__(self):
        self.client = anthropic.Anthropic()  # reads ANTHROPIC_API_KEY from env

    def analyze(self, prompt: str, stream_callback=None) -> dict:
        """
        Calls Claude. If stream_callback provided, calls it with each text chunk
        for live display in the commentary dock.
        Returns parsed JSON result dict.
        """
        full_text = ""

        with self.client.messages.stream(
            model=MODEL,
            max_tokens=900,
            system=self._system_prompt(),
            messages=[{"role": "user", "content": prompt}],
        ) as stream:
            for chunk in stream.text_stream:
                full_text += chunk
                if stream_callback:
                    stream_callback(chunk)

        # Extract JSON from response
        try:
            # Strip any accidental markdown code fences
            cleaned = re.sub(r"```(?:json)?|```", "", full_text).strip()
            return json.loads(cleaned)
        except json.JSONDecodeError as e:
            log.error(f"Failed to parse Claude response as JSON: {e}\nRaw: {full_text[:500]}")
            return {
                "narrative": full_text[:500],
                "annotations": [],
                "alerts": ["AI response was not valid JSON — raw text shown"],
            }

    def _system_prompt(self) -> str:
        from prompt_builder import SYSTEM_PROMPT
        return SYSTEM_PROMPT
```

### 6.3 Create `scripts/ai/ai_pipeline.py` — The Main Entry Point

This is the persistent server. C++ launches it once and keeps it alive.

```python
# scripts/ai/ai_pipeline.py
"""
Persistent stdin/stdout JSON server.
C++ sentinel-server launches this once and communicates via line-delimited JSON.

Input (one JSON object per line on stdin):
  {"id": "req-1", "symbol": "AAPL", "asset_type": "stock", "timeframe": "1d"}

Output (one JSON object per line on stdout):
  {"id": "req-1", "type": "chunk",    "text": "...narrative chunk..."}
  {"id": "req-1", "type": "complete", "result": { narrative, annotations, alerts, regime }}
  {"id": "req-1", "type": "error",    "error": "...message..."}
"""
import asyncio
import json
import logging
import sys
import os

# Make sure imports work from scripts/ root
sys.path.insert(0, os.path.dirname(os.path.dirname(__file__)))

from ai.data_aggregator import gather_context
from ai.regime_detector import detect_regime
from ai.prompt_builder  import build_prompt
from ai.claude_client   import ClaudeClient

log = logging.getLogger(__name__)
logging.basicConfig(level=logging.WARNING, stream=sys.stderr)

_claude = ClaudeClient()


def _emit(obj: dict):
    """Write one JSON line to stdout, flush immediately."""
    sys.stdout.write(json.dumps(obj) + "\n")
    sys.stdout.flush()


async def handle_request(req: dict):
    req_id     = req.get("id", "unknown")
    symbol     = req.get("symbol", "BTC-USD")
    asset_type = req.get("asset_type", "crypto")
    timeframe  = req.get("timeframe", "1d")
    query      = req.get("query", "")

    try:
        # 1. Gather all data
        ctx = await gather_context(symbol, asset_type, timeframe)

        # 2. Detect regime algorithmically
        regime = detect_regime(ctx)

        # 3. Build prompt
        prompt = build_prompt(ctx, regime)
        if query:
            prompt += f"\n\nUser question: {query}"

        # 4. Stream Claude response
        def on_chunk(text: str):
            _emit({"id": req_id, "type": "chunk", "text": text})

        result = _claude.analyze(prompt, stream_callback=on_chunk)

        # 5. Emit complete result with regime metadata
        _emit({
            "id":     req_id,
            "type":   "complete",
            "result": {
                **result,
                "regime":      regime.regime_type,
                "confidence":  regime.regime_confidence,
                "anomaly_flags": [f.code for f in regime.anomaly_flags],
                "macro_bias":  regime.macro_bias,
            }
        })

    except Exception as e:
        log.exception(f"Pipeline error for {symbol}")
        _emit({"id": req_id, "type": "error", "error": str(e)})


async def main():
    """Read JSON lines from stdin forever."""
    loop = asyncio.get_event_loop()
    while True:
        line = await loop.run_in_executor(None, sys.stdin.readline)
        if not line:
            break
        line = line.strip()
        if not line:
            continue
        try:
            req = json.loads(line)
        except json.JSONDecodeError:
            _emit({"type": "error", "error": f"Invalid JSON input: {line[:100]}"})
            continue
        asyncio.create_task(handle_request(req))


if __name__ == "__main__":
    asyncio.run(main())
```

### 6.4 Test the full pipeline end-to-end

```bash
cd scripts

# One-shot test (send one request, read output, exit)
echo '{"id":"t1","symbol":"AAPL","asset_type":"stock","timeframe":"1d"}' \
  | python -m ai.ai_pipeline

# Should stream:
# {"id":"t1","type":"chunk","text":"Apple is currently..."}
# {"id":"t1","type":"chunk","text":" showing..."}
# ...
# {"id":"t1","type":"complete","result":{"narrative":"...","annotations":[...],"alerts":[],"regime":"RANGING",...}}
```

---

## Session 7 — C++ Wiring (1-2 hours, after pipeline is solid)

This is the final step. Don't start here until Session 6 passes the end-to-end test.

### 7.1 New files to create

```
libs/core/protocol/AIAnnotation.hpp      — annotation structs + result struct
libs/core/servermodel/AIContextManager.hpp/cpp  — Python subprocess manager
```

### 7.2 `AIContextManager` sketch

```cpp
// libs/core/servermodel/AIContextManager.hpp
#pragma once
#include <QObject>
#include <QProcess>
#include <QJsonObject>
#include <QVector>
#include "../protocol/AIAnnotation.hpp"

class AIContextManager : public QObject {
    Q_OBJECT
public:
    explicit AIContextManager(QObject* parent = nullptr);
    ~AIContextManager();

    void start();   // launches scripts/ai/ai_pipeline.py subprocess
    void stop();

    // Request analysis for a symbol/timeframe. Fires signals when done.
    void requestAnalysis(const QString& symbol, const QString& assetType,
                         const QString& timeframe, const QString& query = {});

signals:
    void narrativeChunk(const QString& reqId, const QString& text);
    void analysisComplete(const QString& reqId, const AIAnalysisResult& result);
    void analysisError(const QString& reqId, const QString& error);

private slots:
    void onReadyReadStdOut();
    void onProcessError();

private:
    QProcess*   m_proc   = nullptr;
    int         m_reqSeq = 0;
    QByteArray  m_lineBuf;

    void handleLine(const QByteArray& line);
    QString nextReqId() { return QString("req-%1").arg(++m_reqSeq); }
};
```

### 7.3 Wire to `AICommentaryFeedDock`

In `AICommentaryFeedDock`, connect `AIContextManager::narrativeChunk` to append
streaming text, and `analysisComplete` to receive final annotations.

The `SentinelStreamServer` broadcasts `ai_annotations_update` messages to all
connected GUI clients when annotations change.

---

## Quick Reference — File Map

```
scripts/
├── macro/
│   ├── __init__.py
│   ├── macro_db.py          ← Session 1 (SQLite tables)
│   ├── fred_collector.py    ← Session 1 (FRED series + regime classifier)
│   └── cot_collector.py     ← Session 2 (CFTC COT positioning)
├── stocks/
│   └── alpha_vantage.py     ← Session 3 (1h intraday candles, 25 req/day limit)
└── ai/
    ├── __init__.py
    ├── ai_pipeline.py        ← Session 6 (persistent stdin/stdout server)
    ├── data_aggregator.py    ← Session 4 (orchestrates all collectors)
    ├── regime_detector.py    ← Session 5 (algorithmic regime + anomaly flags)
    ├── prompt_builder.py     ← Session 6 (structured → prompt)
    ├── claude_client.py      ← Session 6 (Anthropic SDK, streaming)
    └── collectors/
        ├── __init__.py
        ├── sec_collector.py      ← Session 4 (wraps existing sec/ scripts)
        ├── screener_collector.py ← Session 4 (wraps existing screener/ scripts)
        └── macro_collector.py    ← Session 4 (wraps fred + cot)

libs/core/
├── protocol/
│   └── AIAnnotation.hpp     ← Session 7
└── servermodel/
    ├── AIContextManager.hpp ← Session 7
    └── AIContextManager.cpp ← Session 7
```

## Updated `scripts/requirements.txt` additions

```
# Macro data
fredapi>=0.5.0
cot-reports>=0.3.0

# Alpha Vantage (no library needed — uses aiohttp directly)
# aiohttp already in requirements

# AI pipeline
anthropic>=0.40.0

# ML (add when Phase 2 ML models are implemented)
# hmmlearn>=0.3.2
# scikit-learn>=1.4.0
# lightgbm>=4.3.0
# ruptures>=1.1.9
# pykalman>=0.9.7
```

---

*Sessions 1–3 are pure Python with no C++ changes — start there.
Sessions 4–6 are also pure Python. Only Session 7 touches C++.
Each session produces testable output before moving to the next.*
