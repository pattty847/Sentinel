# Sentinel AI — Architecture Review
## Hard Questions, Critical Issues, Concrete Fixes

This document reviews the planned architecture as a senior engineer would before
a production build. Each issue is rated by severity and given a concrete resolution.

**Severity**: 🔴 Blocker (breaks the feature) | 🟡 High (degrades badly under real use) | 🟢 Important (correct now, pain later)

---

## 1. 🔴 The Heatmap Data Gap — Python Has No Access to C++ Memory

**The issue**: The entire heatmap intelligence — bid/ask liquidity clusters, TWAP density,
the thing that makes Sentinel unique — lives inside `HeatmapTwapStreamer::SymbolState`
in C++ memory. The Python AI pipeline has zero visibility into it. The plan says
"export heatmap summary to Python" but doesn't say how, and it's not optional — for
crypto, this *is* the primary signal.

**Why it's a blocker**: Without heatmap data, the AI analysis for BTC-USD is just
screener RSI + macro. That's a TradingView alert, not a Sentinel feature.

**The fix — C++ assembles the heatmap context and injects it into the request**:

The `AIContextManager` (C++) builds the request JSON before sending it to Python.
It calls `ServerDataModel::getHeatmapHistory()` (already exists) and summarizes it
into a compact object. Python receives this as optional context in the request body.
No Python changes needed for new heatmap signals — you extend the C++ summary function.

```cpp
// AIContextManager.cpp — build heatmap_context before sending request
QJsonObject buildHeatmapContext(const std::string& symbol, ServerDataModel& model) {
    // Pull last 30 columns of heatmap history at active timeframe
    std::vector<HeatmapTwapStreamer::HistoryColumn> history;
    int w, h;
    bool ok = model.getHeatmapHistory(symbol, m_activeTimeframeMs,
                                       QDateTime::currentMSecsSinceEpoch(),
                                       30, w, h, history);
    if (!ok || history.empty()) return {};

    // Find top 5 bid and ask liquidity clusters (highest intensity rows)
    // Find large trade events (from ServerDataModel::collectFootprintTrades)
    // Compute bid/ask ratio over last N columns
    // Return as compact JSON — Python just reads it, never computes it

    QJsonArray bidClusters, askClusters;
    // ... iterate intensity bytes, find peaks ...
    return QJsonObject{
        {"bid_clusters", bidClusters},
        {"ask_clusters", askClusters},
        {"bid_ask_ratio_30col", bidAskRatio},
        {"large_prints", largePrints},
    };
}
```

This is the **correct boundary**: C++ owns the heatmap data, C++ summarizes it,
Python receives a human-readable JSON summary. Never try to stream raw bytes to Python.

---

## 2. 🔴 Request Stacking — The asyncio Task Pile-Up

**The issue**: `ai_pipeline.py` does `asyncio.create_task(handle_request(req))` for
every incoming line. There's no cancellation, no deduplication, no limit. A user
rapidly switching symbols (BTC → ETH → BTC → AAPL in 3 seconds) queues 4 concurrent
Claude API calls, costing $0.08 and producing 3 stale results that overwrite each other.

**Under load**: SEC fetches are slow (1-3s each). If 5 requests come in before the
first finishes, you have 5 concurrent SEC API calls, 5 concurrent Claude calls.
Your SEC agent gets rate-limited and blacklisted in under a minute.

**The fix — request cancellation + debounce + single-flight per symbol**:

```python
# In ai_pipeline.py — replace bare create_task with a managed queue

class PipelineManager:
    def __init__(self):
        self._in_flight: dict[str, asyncio.Task] = {}  # symbol → current task

    async def submit(self, req: dict):
        symbol = req.get("symbol", "")
        timeframe = req.get("timeframe", "1d")
        key = f"{symbol}:{timeframe}"

        # Cancel any in-flight request for this key
        existing = self._in_flight.get(key)
        if existing and not existing.done():
            existing.cancel()
            _emit({"id": existing.req_id, "type": "cancelled"})

        task = asyncio.create_task(handle_request(req))
        task.req_id = req.get("id", "unknown")
        self._in_flight[key] = task

        try:
            await task
        except asyncio.CancelledError:
            pass
        finally:
            if self._in_flight.get(key) is task:
                del self._in_flight[key]
```

On the **C++ side**, add a minimum debounce interval. Don't fire AI requests on
every viewport drag — only on intentional context changes:

```cpp
// AIContextManager.cpp
void AIContextManager::requestAnalysis(...) {
    // Debounce: ignore requests < 2s after the last one for same symbol+timeframe
    auto now = QDateTime::currentMSecsSinceEpoch();
    auto key = symbol + ":" + timeframe;
    if ((now - m_lastRequestTime.value(key, 0)) < 2000) return;
    m_lastRequestTime[key] = now;
    // ... send request
}
```

---

## 3. 🔴 SQLite Multi-Process Write Collisions

**The issue**: `data/sentinel.db` is written by:
1. `ai_pipeline.py` (persistent, long-lived, writes macro + COT + AV cache)
2. Any `sec_fetch_*.py` one-shot scripts (also write to same DB)
3. Potentially multiple concurrent Python subprocesses if SEC fetches are parallelized

SQLite's default journal mode (`DELETE`) locks the entire database on every write.
Two Python processes writing simultaneously = `OperationalError: database is locked`.

**The fix — two changes**:

First, enable WAL mode in every new aiosqlite connection open:
```python
# In macro_db.py, alpha_vantage.py — add to every connect() usage
async with aiosqlite.connect(db_path) as db:
    await db.execute("PRAGMA journal_mode=WAL")
    await db.execute("PRAGMA synchronous=NORMAL")   # safe + faster than FULL
    await db.execute("PRAGMA busy_timeout=5000")     # wait 5s before failing
    # ... rest of operations
```

Second, use **separate database files by domain** to eliminate cross-domain contention:
```
data/sentinel_sec.db    ← all SEC data (existing sql_cache_manager.py)
data/sentinel_macro.db  ← FRED + COT (new macro_db.py)
data/sentinel_av.db     ← Alpha Vantage candles
data/sentinel_ai.db     ← AI result cache, regime logs
```

This also makes backups and debugging much cleaner. The `DB_PATH` constant in each
module should point to its own file.

---

## 4. 🟡 The State Coherence Problem — What Is The AI Analyzing?

**The issue**: The user is looking at a live chart that updates every 50ms. The AI
analysis takes 3-8 seconds. By the time the response arrives and annotations are
painted:
- The heatmap has moved
- A new candle may have closed
- The user may have panned the viewport

Currently there's no mechanism to associate "this annotation was generated from data
as of T=14:32:05" with the chart's current state. An annotation for a liquidity wall
that was absorbed 7 seconds ago gets painted as if it's still there.

**The fix — generation timestamp on every annotation + staleness rendering**:

Every `AIAnnotation` gets a `generated_at_ms` field (already in the plan's result struct
but not used in rendering). The `AIAnnotationLayer` fades out annotations older than
a configurable threshold (default: 15 min for price levels, 1 min for order flow).

```cpp
struct AIAnnotation {
    // ... existing fields ...
    qint64 generatedAtMs = 0;   // when the analysis snapshot was taken
    qint64 ttlMs         = 900'000; // default 15 min; shorter for heatmap-derived
};
```

In the renderer, annotations with `age > ttl * 0.75` start fading their alpha.
At `age > ttl`, they're removed from the store automatically. Users can see
how fresh the analysis is at a glance.

**Separate TTLs by annotation source**:
- Heatmap-derived (liquidity cluster, absorption): 5 minutes — stale quickly
- Technical (RSI divergence, EMA cross): 1 hour — moves slowly
- Insider/SEC (Form 4, 8-K): 7 days — doesn't change
- Macro (credit spread, COT): 1 week — weekly data

---

## 5. 🟡 The Python Import Path Problem Will Break in Production

**The issue**: `sec_collector.py` does:
```python
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))
from sec.sec_api import SECDataFetcher
```

This works if you run from `scripts/ai/collectors/`. It breaks if:
- You run from the repo root: `python scripts/ai/ai_pipeline.py`
- C++ launches it with `QProcess` using an absolute path
- A future test runner uses `pytest` from anywhere

`ai_pipeline.py` is the C++ process's entry point and `QProcess` sets the working
directory to wherever `sentinel-server` was launched from (typically the repo root or
the build directory). All those relative `sys.path` inserts will point to the wrong place.

**The fix — a single canonical path setup at the top of `ai_pipeline.py`**:

```python
# ai_pipeline.py — first thing, before any other imports
import sys
from pathlib import Path

# Always resolve relative to THIS file's location regardless of cwd
SCRIPTS_ROOT = Path(__file__).resolve().parent.parent  # → /path/to/repo/scripts/
sys.path.insert(0, str(SCRIPTS_ROOT))
```

And every collector should use this same pattern — resolve relative to `__file__`,
never relative to `cwd`. Add this to a shared `scripts/ai/_path_setup.py` that
every file in `scripts/ai/` imports first.

---

## 6. 🟡 Alpha Vantage 25 req/day — Wrong Choice for the Use Case

**The issue**: 25 requests/day sounds usable until you realize:
- 10 watchlist stocks × 1 fetch/day = 10 requests. Fine.
- But fetching 90-day history for a new symbol = 3 requests (monthly endpoint, 3 months).
- Add a 10th symbol mid-day, and you're at 32 requests — already over limit.
- Free tier is now officially 25 req/day (it used to be 500/day). This changed in 2023.

More importantly: **yfinance already does this for free with no key and no daily limit**.
The existing `scripts/stocks/fetch_daily_ohlcv.py` uses it. yfinance also supports
1h intervals for the last 730 days: `yf.download(ticker, interval='1h', period='2y')`.

**The fix — drop Alpha Vantage from the roadmap, extend yfinance**:

```python
# scripts/stocks/fetch_daily_ohlcv.py — extend existing file to support 1h
# yfinance interval='1h' gives 730 days of 1h bars, completely free, no key

import yfinance as yf

def fetch_intraday(ticker: str, interval: str = "1h") -> list[dict]:
    """Fetches intraday candles. interval: '1h', '30m', '15m', '5m', '1m'"""
    # yfinance: 1h = up to 730 days, 30m = 60 days, 1m = 7 days
    period = "730d" if interval == "1h" else "60d"
    df = yf.download(ticker, interval=interval, period=period, progress=False)
    # ... convert to list of dicts
```

Reserve Alpha Vantage as a future upgrade for the `OVERVIEW` endpoint (company
fundamentals: P/E, EPS, beta, analyst ratings) which isn't available in yfinance
and costs 1 request per symbol per quarter.

---

## 7. 🟡 Protocol Versioning — You Will Break Clients

**The issue**: `SentinelStreamProtocol.hpp` has `kHeatmapSchemaVersion = 1`,
`kCandleSchemaVersion = 1`, etc. But there's no schema version for AI messages.
You're about to add 4+ new `MessageType` entries and corresponding JSON structures.

If a user has the GUI client compiled against an older version and connects to a
newer server (or vice versa), unknown message types arrive as `MessageType::Unknown`
and get silently dropped. That's fine. But if you later *change* an existing AI
message structure (rename `price_low` to `price_bottom`), the old client parses
garbage and potentially panics.

**The fix — add schema version to AI messages now, before you ship**:

```cpp
// SentinelStreamProtocol.hpp additions
namespace SentinelProtocol {
    constexpr int kAISchemaVersion = 1;  // bump when AIAnalysisResult fields change
}

// New MessageType entries — add to the enum:
AIAnalysisRequest,
AIAnalysisChunk,
AIAnalysisComplete,
AIAnnotationsUpdate,
AIAlert,
```

In every AI message JSON, include `"ai_schema_version": 1`. The client checks this
on receipt. If `received_version > client_known_version`, log a warning and skip
unknown fields gracefully (which nlohmann-json does by default if you use `at()`
with `.value()` fallbacks). Don't fail-hard on unknown fields.

---

## 8. 🟡 The Subprocess Crash Recovery Chain

**The issue**: `ai_pipeline.py` runs as a `QProcess`. What happens when:
- Python process crashes (unhandled exception, OOM, SIGKILL)
- Python process hangs (deadlock in SEC HTTP fetch, asyncio loop blocked)
- Python import fails on startup (missing dependency: `anthropic` not installed)

Currently: `AIContextManager` has no crash detection or recovery. The C++ server
keeps sending requests into a dead stdin pipe. They silently disappear. The user
sees the commentary dock frozen with the last result.

**The fix — the supervisor pattern in AIContextManager**:

```cpp
class AIContextManager : public QObject {
    // ...
private:
    // Watchdog
    QTimer   m_heartbeatTimer;   // sends {"type":"ping"} every 30s
    QTimer   m_watchdogTimer;    // fires if no pong received in 60s → restart
    int      m_crashCount = 0;
    int      m_maxRestarts = 5;
    QDateTime m_lastCrashTime;

    void onProcessFinished(int exitCode, QProcess::ExitStatus status) {
        qWarning() << "AI pipeline exited:" << exitCode;
        if (m_crashCount >= m_maxRestarts) {
            emit pipelineDisabled("AI pipeline failed to start after 5 attempts");
            return;
        }
        // Exponential backoff: 2s, 4s, 8s, 16s, 32s
        int delay = 2000 * (1 << m_crashCount++);
        QTimer::singleShot(delay, this, &AIContextManager::start);
    }

    void onWatchdogFired() {
        qWarning() << "AI pipeline heartbeat timeout — restarting";
        m_proc->kill();  // onProcessFinished will handle restart
    }
};
```

Add a `"type": "ping"` / `"type": "pong"` round-trip to `ai_pipeline.py`.
Missing pong in 60s = hung process = kill and restart.

Also: always check `QProcess::state()` before writing to stdin. If the process isn't
running, queue the request and flush when it restarts.

---

## 9. 🟡 The SEC Rate Limit Reality is Worse Than the Code Assumes

**The issue**: `sec_api.py` sets `rate_limit_sleep: 0.1` (10 req/s). SEC EDGAR's
published limit is 10 req/s, but in practice the CDN throttles at ~5 req/s and
will soft-ban IPs that sustain > 3 req/s average over any 10-minute window.

For a Form 4 analysis, the pipeline calls:
1. `get_cik_for_ticker()` → 1 request
2. `get_company_submissions()` → 1 request
3. For each filing in the last 90 days → 1 request per Form 4 document XML

A company with 10 insiders filing in 90 days = 12 SEC requests per analysis.
A watchlist of 20 stocks = 240 SEC requests on first load. At 3 req/s safe rate,
that's 80 seconds of sequential loading. And if your cache is warm, fine. But
on first run or after clearing cache, this is the startup experience.

**The fixes**:

**1. Limit the filing document fetch depth**: Only fetch the XML documents for the
most recent N filings (currently `filing_limit: 10`). Keep it there. Don't remove this limit.

**2. Warmup the cache in background on server start**: When `ai_pipeline.py` starts,
pre-fetch SEC data for the watchlist in background at a conservative 1 req/s,
not on-demand when the user first clicks a symbol. User never sees the wait.

**3. The RSS poller is the correct long-term answer**: Rather than polling SEC on
every analysis, subscribe to the EDGAR RSS feed for watchlist symbols. Only re-fetch
when a new filing appears. This drops steady-state SEC requests from "every analysis"
to "only when something new happens". This was deferred in the roadmap but should
be promoted to Phase 2 — without it, the SEC cache is always stale by unknown amounts.

---

## 10. 🟡 LLM Output Is Not Reliably JSON — Your C++ Parser Must Be Paranoid

**The issue**: Claude is instructed to return JSON. It usually does. But:
- With a long, complex context, it sometimes wraps JSON in ` ```json ``` ` fences
- Field types drift: `"price_low": "67,400"` (string) instead of `67400.0` (float)
- Field names drift under paraphrasing: `"price_bottom"` instead of `"price_low"`
- `null` vs `None` vs `0` vs missing field — all four are possible for optional fields
- Network interruption mid-stream → partial JSON that won't parse

The `claude_client.py` strips code fences before parsing, which handles the most common
case. But the C++ side that parses `AIAnalysisResult` from the `complete` message JSON
must never assume field types are correct.

**The fix — defensive C++ parsing with `.value()` fallbacks everywhere**:

```cpp
// When parsing annotations from the complete message:
AIAnnotation parseAnnotation(const nlohmann::json& j) {
    AIAnnotation a;
    a.label     = j.value("label", "");
    a.priceLow  = j.value("price_low", 0.0);    // handles null → 0.0
    a.priceHigh = j.value("price_high", 0.0);
    // Normalize type string to uppercase before enum lookup
    auto typeStr = j.value("type", "LABEL");
    std::transform(typeStr.begin(), typeStr.end(), typeStr.begin(), ::toupper);
    // ... enum lookup with LABEL as fallback
    return a;
}
```

Also: clamp the annotations array. If Claude returns 30 annotations, take the first 12.
The `AIAnnotationStore` should enforce a hard limit of 20 annotations per symbol at
all times to prevent chart clutter.

---

## 11. 🟡 Annotation Coordinate System — Timestamps Are Ambiguous

**The issue**: `EVENT_MARKER` annotations need a timestamp to position vertically on
the time axis. Claude generates timestamps from SEC filing dates
(e.g., `"2024-01-15"` from a Form 4). But:

- SEC filing dates are Eastern Time business dates, not UTC timestamps
- The chart's time axis is in UTC epoch milliseconds
- `"2024-01-15"` needs to become `1705276800000` (Jan 15 midnight UTC) or
  `1705330800000` (Jan 15 09:30 ET market open)
- If the filing is before market open, should it appear at midnight UTC? At 9:30 ET?
  At the bar that corresponds to that calendar day?

For daily candles (stocks), "day of filing" is fine — map to midnight UTC of that date.
For 1h candles, you want the nearest open bar. For sub-hour charts, the ambiguity
compounds.

**The fix — normalize timestamps in Python before sending to C++**:

```python
# In annotation_extractor.py — normalize all timestamps to UTC milliseconds
from datetime import datetime, timezone
import pytz

ET = pytz.timezone("America/New_York")

def normalize_annotation_timestamp(ts_str: str | None) -> int | None:
    if not ts_str:
        return None
    try:
        # Parse date-only strings as ET 09:30 (market open) for stock events
        if len(ts_str) == 10:   # "YYYY-MM-DD"
            dt = ET.localize(datetime.strptime(ts_str, "%Y-%m-%d").replace(hour=9, minute=30))
            return int(dt.astimezone(timezone.utc).timestamp() * 1000)
        # Parse ISO datetime strings
        dt = datetime.fromisoformat(ts_str.replace("Z", "+00:00"))
        return int(dt.timestamp() * 1000)
    except ValueError:
        return None
```

All timestamps leave Python as UTC milliseconds. The C++ renderer maps them to
x-coordinates using the existing `TimeAxisMapping`. No ambiguity.

---

## 12. 🟡 The Regime Detector Has No Memory and No Feedback Loop

**The issue**: Every call to `detect_regime()` is stateless. It scores the current
snapshot in isolation. But regime detection is inherently temporal:
- "This has been ranging for 3 weeks" is a stronger ranging signal than "this looks
  rangy right now"
- A regime that just transitioned is less reliable than one that's been stable
- There's no way to know "the last time we called this DISTRIBUTION, was it right?"

Without feedback, the regime detector stays permanently calibrated to initial intuitions
about what thresholds mean. With 1000 regime calls and outcomes, you could train a
LightGBM in 10 minutes and dramatically improve accuracy.

**The fix — log every regime call with outcome measurement baked in**:

```python
# In ai_pipeline.py — after every complete analysis, log the regime call
async def log_regime_call(symbol: str, timeframe: str, regime: RegimeSignal,
                          price_at_call: float):
    async with aiosqlite.connect(AI_DB_PATH) as db:
        await db.execute("""
            INSERT INTO regime_log
            (symbol, timeframe, regime_type, confidence, price_at_call,
             anomaly_codes, called_at)
            VALUES (?,?,?,?,?,?,?)
        """, (symbol, timeframe, regime.regime_type, regime.regime_confidence,
              price_at_call,
              json.dumps([f.code for f in regime.anomaly_flags]),
              datetime.utcnow().isoformat()))
        await db.commit()
```

A future background job can JOIN this table with price history to compute
"what happened N bars after each regime call" and build the training set for
LightGBM. The logging cost is negligible. Missing it now means permanently losing
this training data.

---

## 13. 🟡 The Screener Collector is Synchronous in an Async Pipeline

**The issue**: `screener_collector.py` wraps `screener_core.build_screener()` and
calls it with `run_in_executor`. This is correct. But `tvscreener` makes HTTP
requests internally that can take 1-3 seconds. During this time, the executor
thread is blocked.

If `data_aggregator.py` calls screener + SEC + macro concurrently with
`asyncio.gather()`, all three are dispatched to the thread pool simultaneously.
If the thread pool is saturated (default Python ThreadPoolExecutor = `min(32, cpu+4)`),
subsequent requests wait. For a 4-core machine, after 4 concurrent slow screener
calls, everything queues.

**The fix**: This is acceptable for Phase 1 but note it in the architecture. The
real fix is to replace `tvscreener` with a direct `aiohttp` call to TradingView's
screener endpoint (it's just a POST to `https://scanner.tradingview.com/crypto/scan`).
The `screener_core.py` module already builds the payload — it's a small refactor to
make it async-native instead of using a sync library.

For now, set a timeout on the executor call:
```python
async def get_screener_context(symbol: str, asset_type: str) -> dict:
    try:
        loop = asyncio.get_event_loop()
        raw = await asyncio.wait_for(
            loop.run_in_executor(None, _fetch_screener_sync, symbol, asset_type),
            timeout=8.0   # don't block the pipeline for a slow screener
        )
    except asyncio.TimeoutError:
        return {"status": "TIMEOUT", "source": "tvscreener"}
```

---

## 14. 🟡 API Cost Control — You Need a Hard Budget Cap

**The issue**: There's no throttle on Claude API calls. The planned triggers are:
- Symbol switch → analysis
- Timeframe switch → analysis  
- Scheduled refresh every 5 min (crypto) / 15 min (stocks)
- Heatmap anomaly spike → analysis
- Form 4 filing detected → analysis

A power user monitoring 10 crypto pairs with anomaly spikes firing constantly could
easily trigger 200+ analyses/day at $0.015 each = $3/day = $90/month. That adds up
fast when you have zero revenue.

**The fix — a `BudgetGuard` in `ai_pipeline.py`**:

```python
# scripts/ai/budget_guard.py
class BudgetGuard:
    def __init__(self, daily_limit: int = 100, cost_per_call: float = 0.02):
        self.daily_limit = daily_limit
        self.cost_per_call = cost_per_call
        self._db_path = "data/sentinel_ai.db"

    async def check_and_record(self) -> bool:
        """Returns True if the call is allowed, False if over budget."""
        today = datetime.utcnow().strftime("%Y-%m-%d")
        async with aiosqlite.connect(self._db_path) as db:
            await db.execute("""
                CREATE TABLE IF NOT EXISTS api_usage
                (date TEXT PRIMARY KEY, call_count INTEGER, estimated_cost REAL)
            """)
            async with db.execute(
                "SELECT call_count FROM api_usage WHERE date=?", (today,)
            ) as cur:
                row = await cur.fetchone()
            count = row[0] if row else 0
            if count >= self.daily_limit:
                return False
            await db.execute("""
                INSERT INTO api_usage (date, call_count, estimated_cost) VALUES (?,1,?)
                ON CONFLICT(date) DO UPDATE SET
                    call_count=call_count+1,
                    estimated_cost=estimated_cost+?
            """, (today, self.cost_per_call, self.cost_per_call))
            await db.commit()
        return True
```

Make `daily_limit` configurable in the server YAML config. Default 50. When the
limit is hit, return a `{"type": "budget_exceeded"}` message to the GUI — display
"Daily AI analysis limit reached (50/50). Resets at midnight UTC." in the commentary dock.

---

## 15. 🟢 Background Scanning — Architecture Must Support It From Day One

**The issue**: Right now, analysis is purely on-demand. But the highest value signals
(Form 4 cluster, COT extremes, credit spread widening) happen on *other* symbols — ones
the user isn't currently watching. If you only analyze what's on screen, you miss it.

This needs a **background scanner** mode: a low-priority loop that runs the pipeline
for every watchlist symbol on a schedule, caches the result, and surfaces the highest-
severity anomaly flags as proactive alerts even if the user is looking at something else.

The architecture must support this from day one because it requires:
- A separate trigger path from user-focus triggers (different priority, different TTL)
- A way to surface alerts to the GUI without the user having to look at that symbol
- A way to deduplicate: don't fire "INSIDER_CLUSTER_BUY on AAPL" as an alert every
  15 minutes — once per event

**The fix — add `trigger_type` to every request and a `NotificationCenter` in C++**:

```json
// Background scan request (low priority, no streaming needed)
{"id": "bg-AAPL-001", "symbol": "AAPL", "asset_type": "stock",
 "timeframe": "1d", "trigger_type": "background_scan"}

// The response includes a severity field at the top level
{"id": "bg-AAPL-001", "type": "complete", "result": {
  "narrative": "...",
  "annotations": [...],
  "top_severity": "WARNING",   ← highest severity anomaly in this analysis
  "alerts": ["INSIDER_CLUSTER_SELL: 3 insiders sold $4.2M in 30 days"]
}}
```

`AIContextManager` routes `background_scan` results to a `NotificationCenter` that
deduplicates by `(symbol, anomaly_code, date)` and surfaces new high-severity alerts
as a notification badge/popup regardless of current active symbol.

---

## 16. 🟢 The `UserFocusContext` Needs to Be Server-Authoritative

**The issue**: Multiple GUI clients can connect to one `sentinel-server`. If Client A
is watching BTC and Client B is watching AAPL, there's one `AIContextManager` on the
server. Whose focus wins?

Currently the plan doesn't address this. If both clients send `ai_analysis_request`
simultaneously, you get two concurrent analyses. Results are broadcast to all clients.
Client A gets an AAPL analysis it didn't ask for.

**The fix — per-client AI sessions**:

Each connected GUI client has a unique client ID (already implied by `SessionManager`
in the codebase). `ai_analysis_request` messages include the client ID. Results
are routed back only to the requesting client (unicast, not broadcast).

Background scan results are the exception — those broadcast to all clients as they're
not tied to a specific user interaction.

```cpp
// SentinelStreamServer handling
void onAIRequest(const QJsonObject& msg, const ClientId& clientId) {
    QString reqId = msg.value("id").toString();
    m_aiManager->requestAnalysis(
        msg.value("symbol").toString(),
        msg.value("asset_type").toString(),
        msg.value("timeframe").toString(),
        clientId,   // route response back to this client only
        reqId
    );
}
```

---

## 17. 🟢 Error Visibility — The User Is Currently Blind to Failures

**The issue**: When any data source fails silently (FRED down, screener timeout,
SEC rate-limited), the AI either gets called with partial data or doesn't get called
at all. The user sees nothing change. They don't know if "no news is good news" or
"everything broke."

**The fix — a `DataQualityReport` attached to every result**:

```python
# In data_aggregator.py — track what succeeded and what failed
@dataclass
class DataQualityReport:
    sources_requested: list[str]
    sources_succeeded: list[str]
    sources_failed: dict[str, str]   # source → error message
    data_freshness: dict[str, str]   # source → "live" | "cache_Xh" | "stale"
    analysis_completeness: float     # 0.0-1.0 based on how many sources succeeded
```

This gets included in every `AIAnalysisResult`. The `AICommentaryFeedDock` shows a
small status line under the narrative:
```
[DATA] SEC ✓ live | FRED ✓ cache 2h | COT ✓ cache 3d | Screener ✗ timeout
```

If `analysis_completeness < 0.5` (more than half of expected sources missing),
warn the user and skip the Claude call — stale/incomplete data costs money and
produces worse narratives than just saying "insufficient data."

---

## Summary — Priority Order

| # | Issue | Severity | When to Fix |
|---|-------|----------|-------------|
| 1 | Heatmap data gap — C++ must inject context | 🔴 | Before any crypto analysis works |
| 2 | Request stacking — cancellation + debounce | 🔴 | Before first demo |
| 3 | SQLite WAL mode + separate DB files | 🔴 | Before any concurrent load |
| 4 | Annotation staleness / TTL by source type | 🟡 | Phase 2 |
| 5 | Python import paths relative to `__file__` | 🟡 | Before writing a single file |
| 6 | Drop Alpha Vantage, extend yfinance to 1h | 🟡 | Session 3 replacement |
| 7 | Protocol schema version for AI messages | 🟡 | Before first client ships |
| 8 | Subprocess crash recovery + heartbeat | 🟡 | Before production |
| 9 | SEC rate limit — RSS poller + warmup | 🟡 | Phase 2 |
| 10 | Paranoid C++ JSON parsing with fallbacks | 🟡 | When writing AIContextManager |
| 11 | Timestamp normalization → UTC ms | 🟡 | When building prompt_builder.py |
| 12 | Regime call logging for future ML | 🟢 | Add in Session 5, cheap |
| 13 | Screener timeout in async gather | 🟢 | Session 4 |
| 14 | Daily budget cap on Claude API calls | 🟢 | Before turning on auto-triggers |
| 15 | Background scanner architecture | 🟢 | Phase 2, but design for it now |
| 16 | Per-client AI session routing | 🟢 | Before multi-user support |
| 17 | DataQualityReport + user error visibility | 🟢 | Phase 2 |

The three 🔴 issues (heatmap gap, request stacking, SQLite WAL) must be resolved
before the AI pipeline produces reliable results. Fix them first, even before
testing the LLM integration.
