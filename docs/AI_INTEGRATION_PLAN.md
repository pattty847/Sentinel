# Sentinel AI Integration Plan
## Multi-Stream Market Intelligence Engine

*The goal: the first trading terminal that synthesizes every available data stream — price action, order flow, SEC filings, insider transactions, screener signals — into a coherent, human-readable narrative that paints directly on the chart and tells you whether what you're seeing is algorithmic noise, accumulation, distribution, or event-driven movement.*

---

## 1. Where We Stand — Data Inventory

### Available Right Now

| Source | What We Get | Crypto | Stocks | Granularity |
|--------|-------------|--------|--------|-------------|
| **Coinbase WebSocket** | L2 order book, market trades, real-time candles | ✅ | ❌ | Tick / 1s |
| **TradingView Screener** | RSI, EMA20/50/200, MACD, VWAP, ADX, ATR, rel vol, market cap | ✅ | ✅ | ~15min refresh |
| **SEC EDGAR** | Form 4 (insider buys/sells), 10-K/10-Q/8-K filings, XBRL financials, supply chain | ❌ | ✅ | Per-filing |
| **Yahoo Finance** | Daily OHLCV, historical | ❌ | ✅ | 1D |
| **GPU Heatmap** | TWAP-aggregated bid/ask density, footprint bid/ask imbalance | ✅ | ❌ | Sub-second |
| **Paper Trading Engine** | Open positions, order history | ✅ | ✅ | Live |

### Gaps for v1 (deferred until funded)

| What | Why Deferred | Free Alternative |
|------|--------------|-----------------|
| Real-time stock Level 2 | Paid feed (Polygon, IEX Cloud, etc.) | TradingView screener TA signals as proxy |
| Intraday stock candles (<1D) | Paid or rate-limited | Yahoo daily + TradingView indicators |
| News / earnings calendar API | Cost (Benzinga, Finnhub, etc.) | SEC 8-K current reports as proxy |
| Options flow | Expensive | SEC 13F institutional holdings as proxy |

**v1 strategy: maximize what we have.** Crypto gets full depth (L2, heatmap, trades). Stocks get SEC intelligence + screener signals + daily OHLCV. The AI synthesizes both layers into a unified narrative.

---

## 2. Core Concept — What the AI Actually Does

The AI is not a chatbot overlay. It is a **market regime analyst** that:

1. **Detects regime** algorithmically (trend/range/distribution/accumulation) before touching LLM
2. **Correlates signals** across timeframes and data layers
3. **Identifies anomalies** that separate algorithmic noise from informed activity
4. **Generates a narrative** that a trader can actually use: *"Large sellers positioned at 67,200–67,400 in the heatmap for 3 sessions. Two Form 4 dispositions by CFO in last 30 days. RSI on 4H diverging from price. This looks like informed distribution, not random noise."*
5. **Paints annotations on the chart** — labeled price zones, timeline markers, severity badges

The critical design principle: **the LLM is the last step**, not the engine. Algorithmic processing does the heavy lifting; the LLM turns structured signals into language.

---

## 3. System Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                     USER CONTEXT MANAGER                         │
│  (active symbol, timeframe, viewport range, what user is doing) │
└──────────────────────┬──────────────────────────────────────────┘
                       │ context request
                       ▼
┌─────────────────────────────────────────────────────────────────┐
│                  DATA AGGREGATION LAYER (Python)                  │
│                                                                   │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐           │
│  │ PriceAction  │  │  SECLayer    │  │ScreenerLayer │           │
│  │ Collector    │  │  Collector   │  │  Collector   │           │
│  │              │  │              │  │              │           │
│  │ - Coinbase   │  │ - Form 4     │  │ - RSI        │           │
│  │   candles    │  │ - 10-K/8-K   │  │ - MACD       │           │
│  │ - Heatmap    │  │ - Financials │  │ - EMA stack  │           │
│  │   summary    │  │ - Insider    │  │ - ADX/ATR    │           │
│  │ - Volume     │  │   analysis   │  │ - Rel vol    │           │
│  │   profile    │  │              │  │              │           │
│  └──────┬───────┘  └──────┬───────┘  └──────┬───────┘           │
│         └─────────────────┴─────────────────┘                    │
│                            │                                      │
│                            ▼                                      │
│              ┌─────────────────────────┐                         │
│              │   REGIME DETECTOR       │                         │
│              │   (algorithmic, no LLM) │                         │
│              │                         │                         │
│              │  Output:                │                         │
│              │  - regime_type          │                         │
│              │  - regime_confidence    │                         │
│              │  - key_levels           │                         │
│              │  - anomaly_flags        │                         │
│              └──────────┬──────────────┘                         │
│                         │                                         │
│                         ▼                                         │
│              ┌─────────────────────────┐                         │
│              │   CONTEXT ASSEMBLER     │                         │
│              │   (structured prompt    │                         │
│              │    builder)             │                         │
│              └──────────┬──────────────┘                         │
│                         │                                         │
│                         ▼                                         │
│              ┌─────────────────────────┐                         │
│              │   CLAUDE API CALL       │                         │
│              │   (claude-sonnet-4-6)   │                         │
│              │                         │                         │
│              │   Returns:              │                         │
│              │   - narrative_text      │                         │
│              │   - chart_annotations[] │                         │
│              │   - alert_flags[]       │                         │
│              └──────────┬──────────────┘                         │
└─────────────────────────┼───────────────────────────────────────┘
                          │ JSON over WebSocket / subprocess stdout
                          ▼
┌─────────────────────────────────────────────────────────────────┐
│                    C++ SENTINEL SERVER                            │
│                                                                   │
│  AIContextManager  →  SentinelStreamServer  →  GUI clients       │
│  (holds state,        (broadcasts AI messages                    │
│   owns Python          on protocol v0)                           │
│   subprocess)                                                     │
└──────────────────────────────┬──────────────────────────────────┘
                               │
                               ▼
┌─────────────────────────────────────────────────────────────────┐
│                         QT6 GUI LAYER                             │
│                                                                   │
│  ┌─────────────────┐   ┌─────────────────┐                      │
│  │ AICommentary    │   │ UnifiedGrid     │                      │
│  │ FeedDock        │   │ Renderer        │                      │
│  │ (narrative      │   │ (chart          │                      │
│  │  stream)        │   │  annotations)   │                      │
│  └─────────────────┘   └─────────────────┘                      │
└─────────────────────────────────────────────────────────────────┘
```

---

## 4. User Context Manager

The AI is only useful if it knows what the user is focused on. The `UserContextManager` tracks:

```cpp
struct UserFocusContext {
    QString  activeSymbol;        // "BTC-USD", "AAPL", etc.
    AssetType assetType;          // Crypto | Stock
    QString  activeTimeframe;     // "1m", "1h", "1d"
    double   viewportLow;         // Price range visible on screen
    double   viewportHigh;
    qint64   viewportStartTs;     // Time range visible
    qint64   viewportEndTs;
    QString  userQuery;           // Optional: what user typed/asked
    QStringList watchlist;        // Current watchlist symbols
    bool     hasOpenPosition;     // Is there a paper trade open?
    double   openPositionEntry;   // If so, at what price?
};
```

**Context triggers** (when to re-run the AI pipeline):
- Symbol switch → immediate re-run (new symbol context)
- Timeframe switch → re-run with new frame
- User types in query box → high-priority run
- Scheduled refresh: crypto every 5 min, stocks every 15 min
- Heatmap anomaly spike (new liquidity pool detected) → async trigger
- Insider filing filed (Form 4 detected) → immediate priority run

Context is serialized to JSON and passed to the Python AI pipeline.

---

## 5. Data Aggregation Layer — Per Data Source

### 5.1 Price Action Collector

**Crypto (full fidelity):**
- Pull recent candles (1m, 5m, 1h, 1d) from Coinbase REST or local tick binary logger
- Compute: trend slope, higher-highs/lower-lows structure, key S/R from volume profile
- Heatmap summary: top 5 bid liquidity clusters, top 5 ask liquidity clusters, bid/ask ratio last 30 min
- Recent large trades (>$100k notional) in last session
- Footprint: bid/ask delta at significant price levels

**Stocks (daily only for v1):**
- Yahoo Finance daily OHLCV last 90 sessions
- Compute same structure analysis on daily data
- Mark earnings dates (from SEC 10-Q filing dates as proxy)
- Flag gap-up/gap-down days

### 5.2 SEC Layer Collector

*This is the real alpha for stocks — public data that most retail tools don't surface intelligently.*

**Form 4 (Insider Transactions):**
- Fetch last 90 days of Form 4 for active symbol
- Classify each transaction: open-market buy/sell, option exercise, grant
- Compute: net insider buy/sell pressure (shares + dollar value)
- Flag: cluster of same-direction transactions (3+ insiders same direction in 30 days = signal)
- Flag: CEO/CFO transactions specifically (C-suite weight higher)
- Flag: large single transaction >1% of outstanding shares

**8-K Current Reports:**
- Last 30 days of 8-K items
- Classify item codes: earnings (2.02), material agreements (1.01), departures (5.02), non-routine (8.01)
- Material agreements and executive departures → high-priority annotation

**10-K / 10-Q Financials:**
- Revenue trend (last 4-8 quarters)
- Net income trend
- Cash vs debt ratio
- Gross margin direction
- Free cash flow
- Classify financial regime: growth/mature/declining/distressed

**Supply Chain (already built):**
- Key suppliers and customers from 10-K text
- Flag if active symbol is supplier/customer of a currently-moving stock

### 5.3 Screener Layer Collector

Pull latest TradingView screener data for the active symbol plus sector peers:

- RSI position (oversold <30, neutral 30-70, overbought >70)
- EMA stack order (bullish: price > EMA20 > EMA50 > EMA200, etc.)
- MACD histogram direction + zero-line position
- ADX strength (trending: ADX > 25, chopping: ADX < 20)
- ATR expansion/contraction (vol expansion = ADX breakout confirmation)
- Relative volume vs average
- Sector + peer comparison: is this symbol leading or lagging its sector?

---

## 6. Regime Detection (Algorithmic — Pre-LLM)

The regime detector runs **before** the LLM call. It produces a structured signal object that constrains the LLM's interpretation and prevents hallucination.

```python
@dataclass
class RegimeSignal:
    # Market structure
    regime_type: Literal["TRENDING_UP", "TRENDING_DOWN", "RANGING", "ACCUMULATION", "DISTRIBUTION", "CLIMAX"]
    regime_confidence: float          # 0.0 - 1.0
    regime_duration_bars: int         # How long current regime has persisted

    # Key price levels
    key_levels: list[KeyLevel]        # Support, resistance, volume nodes, VWAP

    # Anomaly flags (the non-random signals)
    anomaly_flags: list[AnomalyFlag]  # See below

    # Crypto-specific (when available)
    heatmap_bias: Literal["BID_HEAVY", "ASK_HEAVY", "BALANCED", "N/A"]
    large_print_events: list[dict]    # Big trades relative to ATR

    # Stock-specific
    insider_pressure: Literal["NET_BUYING", "NET_SELLING", "NEUTRAL", "N/A"]
    financial_regime: Literal["GROWTH", "MATURE", "DECLINING", "DISTRESSED", "N/A"]
```

**Anomaly Flag types** (what separates noise from signal):

```python
class AnomalyType(Enum):
    # Order flow
    HEATMAP_LIQUIDITY_ABSORPTION  # Large limit orders being eaten through
    HEATMAP_WALL_REMOVAL          # Big bid/ask wall pulled just before move
    LARGE_TRADE_CLUSTER           # Multiple large prints in tight window
    DELTA_DIVERGENCE              # Price making HH but cumulative delta making LL

    # Insider / fundamental
    INSIDER_CLUSTER_BUY           # 3+ insiders buying in 30-day window
    INSIDER_CLUSTER_SELL          # 3+ insiders selling in 30-day window
    CSUITE_LARGE_BUY              # CEO/CFO open-market buy > $500K
    CSUITE_LARGE_SELL             # CEO/CFO open-market sell > $1M
    MATERIAL_8K_FILED             # Material agreement or undisclosed item
    EXECUTIVE_DEPARTURE           # Item 5.02 in recent 8-K

    # Technical
    RSI_EXTREME_WITH_DIVERGENCE   # RSI >80 or <20 + price/RSI divergence
    EMA_STACK_FLIP                # EMA order reversal (trend change signal)
    RELATIVE_VOLUME_SPIKE         # Rel vol > 3x average
    ADX_TREND_BIRTH               # ADX crossing 25 from below = new trend
    VOLATILITY_COMPRESSION        # ATR < 0.5x 20-period average = coiling
```

**Regime detection algorithm:**

```
Trending: ADX > 25 + EMA stack aligned + price above/below VWAP
Ranging: ADX < 20 + price oscillating between two levels + RSI bouncing extremes
Accumulation: Ranging + net insider buying + above-avg volume on down bars, below on up = Wyckoff
Distribution: Ranging near highs + net insider selling + above-avg volume on up bars, below on down
Climax: Volatility expansion + RSI extreme + relative vol > 4x + heatmap showing absorption
```

---

## 7. Context Assembler — Prompt Architecture

The context assembler converts all structured data into a prompt. **Critical design: use structured data, not prose in the prompt.** Give the LLM facts and let it generate the narrative.

```python
def build_prompt(context: UserFocusContext, regime: RegimeSignal, data: AggregatedData) -> str:
    return f"""
You are a senior quantitative market analyst embedded in a trading terminal.
Analyze the following structured data for {context.activeSymbol} and produce:
1. A 2-4 sentence market narrative (plain English, actionable)
2. A list of chart annotations to display
3. Alert flags if any signals require immediate attention

## User Context
- Symbol: {context.activeSymbol} ({context.assetType})
- Viewing: {context.activeTimeframe} timeframe
- Viewport: ${context.viewportLow:,.0f} - ${context.viewportHigh:,.0f}
{f'- User query: {context.userQuery}' if context.userQuery else ''}

## Detected Regime
- Type: {regime.regime_type} (confidence: {regime.regime_confidence:.0%})
- Duration: {regime.regime_duration_bars} bars
- Heatmap bias: {regime.heatmap_bias}
- Insider pressure (90d): {regime.insider_pressure}

## Anomaly Flags Detected
{format_anomalies(regime.anomaly_flags)}

## Key Price Levels
{format_levels(regime.key_levels)}

## Technical Signals (TradingView)
{format_screener(data.screener)}

## Insider Activity (SEC Form 4, 90d)
{format_insiders(data.insider_analysis)}

## Recent 8-K Events (30d)
{format_filings(data.recent_8k)}

## Financial Regime ({data.financials.get('period','N/A')})
{format_financials(data.financials)}

## Heatmap Summary (last 30 min, crypto only)
{format_heatmap(data.heatmap_summary)}

---
RESPOND IN THIS EXACT JSON FORMAT:
{{
  "narrative": "string (2-4 sentences)",
  "annotations": [
    {{
      "type": "ZONE|LINE|LABEL|EVENT_MARKER",
      "price_low": float_or_null,
      "price_high": float_or_null,
      "timestamp": iso8601_or_null,
      "label": "string",
      "severity": "INFO|CAUTION|WARNING|ALERT",
      "color_hint": "BULLISH|BEARISH|NEUTRAL|INSIDER|REGIME"
    }}
  ],
  "alerts": ["string", ...]
}}
"""
```

**Output contract** — the LLM always returns structured JSON, never free-form prose. The C++ layer parses this; the `narrative` field flows to `AICommentaryFeedDock`; the `annotations` array feeds the chart overlay renderer.

---

## 8. Chart Annotation System

This is the visual layer — where the AI "paints" on the chart. We extend `UnifiedGridRenderer` with an annotation overlay.

### 8.1 Annotation Types

| Type | Visual | Example |
|------|--------|---------|
| `ZONE` | Shaded horizontal band | "Insider selling cluster 142–145", "Heatmap ask wall" |
| `LINE` | Horizontal price line | "Key S/R level", "VWAP deviation band" |
| `LABEL` | Text badge at price level | "CFO sold $2.1M here", "ADX breakout triggered" |
| `EVENT_MARKER` | Vertical timeline marker | "Form 4 filed", "8-K material agreement", "Large print" |

### 8.2 Data Structures

```cpp
// libs/core/protocol/AIAnnotation.hpp
struct AIAnnotation {
    enum class Type { ZONE, LINE, LABEL, EVENT_MARKER };
    enum class Severity { INFO, CAUTION, WARNING, ALERT };
    enum class ColorHint { BULLISH, BEARISH, NEUTRAL, INSIDER, REGIME };

    Type        type;
    double      priceLow   = 0.0;   // ZONE/LINE: price level(s)
    double      priceHigh  = 0.0;
    qint64      timestamp  = 0;     // EVENT_MARKER: when
    QString     label;
    Severity    severity   = Severity::INFO;
    ColorHint   colorHint  = ColorHint::NEUTRAL;
};

struct AIAnalysisResult {
    QString              symbol;
    QString              narrative;
    QVector<AIAnnotation> annotations;
    QStringList          alerts;
    qint64               generatedAt;
    QString              regimeType;
    float                regimeConfidence;
};
```

### 8.3 Rendering

`UnifiedGridRenderer` already has `TimeAxisMapping` and price-to-pixel transforms. We add an `AIAnnotationLayer` as a separate `QSGNode` child that renders on top of the heatmap/candlestick layers.

- **ZONE**: two horizontal lines + fill (alpha-blended rectangle in world-space coordinates)
- **LINE**: single horizontal line spanning full viewport width
- **LABEL**: MSDF text badge (reuse existing `MsdfGlyphNode`) pinned to a price level
- **EVENT_MARKER**: vertical line at timestamp + small icon + tooltip text

Annotations are stored in `AIAnnotationStore` (simple `QVector`, max 50 annotations per symbol, FIFO pruning). When the viewport moves/zooms, only annotations within `[viewportLow - 10%, viewportHigh + 10%]` are rendered.

---

## 9. Protocol Extensions (C++ Server ↔ Python)

### 9.1 New Protocol Messages (JSON v0 extensions)

```json
// Client → Server: request AI analysis
{
  "type": "ai_analysis_request",
  "symbol": "BTC-USD",
  "timeframe": "1h",
  "viewport_low": 64000,
  "viewport_high": 68000,
  "query": ""
}

// Server → Client: AI analysis result (streamed)
{
  "type": "ai_analysis_result",
  "symbol": "BTC-USD",
  "status": "streaming|complete|error",
  "chunk": "...narrative text chunk...",   // for streaming
  "result": { ... AIAnalysisResult ... }   // when complete
}

// Server → Client: AI annotation update
{
  "type": "ai_annotations_update",
  "symbol": "BTC-USD",
  "annotations": [ ... ]
}

// Server → Client: AI alert
{
  "type": "ai_alert",
  "symbol": "BTC-USD",
  "severity": "WARNING",
  "message": "Cluster of insider selling detected. 3 Form 4 filings in 7 days."
}
```

### 9.2 Server-Side: AIContextManager

New C++ class in `libs/core/servermodel/`:

```cpp
class AIContextManager : public QObject {
    Q_OBJECT
public:
    void requestAnalysis(const UserFocusContext& ctx);
    void setScheduledRefresh(const QString& symbol, int intervalSeconds);

signals:
    void analysisReady(const AIAnalysisResult& result);
    void annotationsUpdated(const QString& symbol, const QVector<AIAnnotation>& annotations);
    void alertFired(const QString& symbol, const QString& message, AIAnnotation::Severity sev);

private:
    // Manages Python subprocess: scripts/ai/ai_pipeline.py
    QProcess* m_pythonProcess;
    void launchPipeline(const QJsonObject& contextJson);
    void onPipelineOutput(const QByteArray& data);
};
```

The Python subprocess runs `scripts/ai/ai_pipeline.py` as a **persistent server** (not one-shot subprocess), communicating via stdin/stdout JSON lines. This avoids Python startup overhead on each request.

---

## 10. Python AI Pipeline Structure

New directory: `scripts/ai/`

```
scripts/ai/
├── ai_pipeline.py          # Entry point — persistent stdin/stdout JSON server
├── context_builder.py      # UserFocusContext deserialization + validation
├── data_aggregator.py      # Orchestrates all collectors
├── collectors/
│   ├── price_action.py     # Coinbase candles + heatmap summary
│   ├── sec_collector.py    # Wraps existing scripts/sec/ modules
│   └── screener_collector.py  # Wraps existing scripts/screener/ modules
├── regime_detector.py      # Algorithmic regime + anomaly classification
├── prompt_builder.py       # Builds structured Claude prompt
├── claude_client.py        # Anthropic SDK client, streaming support
├── annotation_extractor.py # Parses LLM JSON → AIAnnotation objects
└── cache.py                # Short-TTL result cache (symbol+timeframe → result)
```

### ai_pipeline.py (persistent server pattern)

```python
# Runs forever, reads JSON lines from stdin, writes JSON lines to stdout
import sys, json, asyncio
from data_aggregator import DataAggregator
from regime_detector import RegimeDetector
from claude_client import ClaudeClient

async def process_request(req: dict) -> dict:
    symbol    = req["symbol"]
    timeframe = req["timeframe"]
    context   = req.get("context", {})

    # 1. Gather all data
    data = await DataAggregator().gather(symbol, timeframe, context)

    # 2. Detect regime algorithmically
    regime = RegimeDetector().detect(data)

    # 3. Build prompt + call Claude
    result = await ClaudeClient().analyze(symbol, context, regime, data)

    return result

async def main():
    loop = asyncio.get_event_loop()
    while True:
        line = await loop.run_in_executor(None, sys.stdin.readline)
        if not line:
            break
        req = json.loads(line.strip())
        result = await process_request(req)
        sys.stdout.write(json.dumps(result) + "\n")
        sys.stdout.flush()

asyncio.run(main())
```

---

## 11. Claude API Integration

Uses `anthropic` Python SDK with streaming for real-time narrative delivery to the GUI.

```python
# scripts/ai/claude_client.py
import anthropic
import json
from typing import AsyncGenerator

MODEL = "claude-sonnet-4-6"   # Best cost/performance for real-time trading analysis

class ClaudeClient:
    def __init__(self):
        self.client = anthropic.Anthropic()  # reads ANTHROPIC_API_KEY from env

    async def analyze(self, symbol: str, context: dict,
                      regime: RegimeSignal, data: AggregatedData) -> AIAnalysisResult:
        prompt = PromptBuilder().build(symbol, context, regime, data)

        # Use streaming so narrative appears word-by-word in the commentary dock
        full_text = ""
        with self.client.messages.stream(
            model=MODEL,
            max_tokens=800,
            messages=[{"role": "user", "content": prompt}],
            system=ANALYST_SYSTEM_PROMPT,
        ) as stream:
            for chunk in stream.text_stream:
                full_text += chunk
                # Emit streaming chunk back to C++ for live display
                print(json.dumps({"type": "chunk", "text": chunk}), flush=True)

        # Parse final JSON from the response
        result = json.loads(full_text)
        return AIAnalysisResult(**result)
```

**System prompt** establishes the analyst persona and enforces output format:
- Strict JSON output contract
- No speculation beyond available data
- Clear signal/noise language: *"algorithmic noise"*, *"informed positioning"*, *"event-driven"*
- Always cite which data source supports each claim
- Flag when confidence is low vs high
- Never produce financial advice language

---

## 12. Phased Implementation Roadmap

### Phase 1: Foundation (Build First)
*Get the pipeline working end-to-end with one data source*

1. **`scripts/ai/` skeleton** — `ai_pipeline.py` persistent server, `claude_client.py`, basic `prompt_builder.py`
2. **SEC-only analysis for stocks** — Use Form 4 + screener signals as the first prompt (no heatmap needed)
3. **C++ `AIContextManager`** — Launch Python subprocess, wire stdin/stdout, parse JSON response
4. **Wire `AICommentaryFeedDock`** — Display narrative text (already stubbed, just needs data)
5. **Protocol v0 extension** — `ai_analysis_request` / `ai_analysis_result` messages

**Milestone**: Type a stock symbol, hit "Analyze" → narrative appears in commentary dock.

### Phase 2: Regime Detection
*Make the algorithmic layer do the heavy lifting*

6. **`regime_detector.py`** — Full anomaly flag system, regime classification
7. **`data_aggregator.py`** — Unified async gather across all available sources
8. **Crypto heatmap summary** — Export top liquidity clusters from `HeatmapTwapStreamer` to Python
9. **Contextual triggers** — Auto-trigger on symbol switch, timeframe change, scheduled refresh

**Milestone**: BTC-USD shows "RANGING → DISTRIBUTION bias, 3 ask walls detected at 67,200–67,400"

### Phase 3: Chart Annotations
*Make it visual — paint on the chart*

10. **`AIAnnotation` structs** — Add to `libs/core/protocol/`
11. **`AIAnnotationStore`** — Per-symbol annotation cache in server model
12. **`AIAnnotationLayer`** — QSG render node in `UnifiedGridRenderer`
13. **Zone/Line/Label/EventMarker rendering** — Reuse MSDF text, add rect/line draw
14. **Viewport-aware culling** — Only render annotations in visible range

**Milestone**: Insider selling zones appear as shaded bands on chart. Form 4 dates appear as vertical markers.

### Phase 4: User Query Interface
*Let the user drive the analysis*

15. **Query input in `AICommentaryFeedDock`** — Text field: "Why is volume spiking?" "What do insiders think?"
16. **Query routing** — Free-form user question appended to context, re-runs pipeline
17. **Multi-symbol comparison** — "Compare NVDA vs AMD insider activity"
18. **Watchlist scanning** — Background sweep of watchlist, surface highest-signal symbols
19. **Alert system** — Proactive notifications for anomaly flag thresholds

**Milestone**: User types "Is this breakout real?" → AI explains heatmap absorption, insider stance, and RSI context.

### Phase 5: Intelligence Compounding (Future)
*When data budget allows*

20. **Real-time news** (Finnhub free tier or RSS scraping)
21. **Options flow proxy** via SEC 13F changes
22. **Earnings date detection** from SEC 10-Q patterns
23. **Cross-asset correlation** — Crypto leading indicator for tech stocks
24. **Historical annotation replay** — "What did the AI say before the last major move?"

---

## 13. Key Design Decisions & Rationale

### Why a persistent Python subprocess, not HTTP?
- Avoids Python interpreter startup cost (~300ms) on each request
- Keeps `asyncio` event loop alive (SEC + screener calls are async)
- Simpler than running a full HTTP server for a single client
- Easy to kill/restart without port conflicts

### Why algorithmic regime detection before LLM?
- Prevents hallucination: LLM cannot invent a "bullish regime" if ADX says 15 (no trend)
- Reduces token cost: structured data is cheaper than asking the LLM to re-derive from raw numbers
- Makes the system auditable: you can log `regime_type` separately from the narrative
- Faster: regime detection is <10ms Python, LLM is 1-3s network

### Why JSON output contract from LLM?
- Directly parseable into `AIAnnotation` structs without text processing
- Forces the LLM to be specific (price levels must be numbers, not "around 67k")
- Enables rendering without NLP post-processing in C++

### Why `claude-sonnet-4-6` not Haiku?
- Sonnet has measurably better financial reasoning and structured output reliability
- At <800 tokens per response, cost per analysis is ~$0.01–0.02
- Fast enough for 5-15 min refresh cycles

### Why not stream everything to the LLM in real-time?
- Tick data at 1000+ events/sec would burn API budget instantly
- The value is in the synthesis of aggregated signals, not raw tick replay
- Pre-aggregated snapshots (heatmap summary, screener values) carry the same information density

---

## 14. File Creation Checklist

### New Python files
- [ ] `scripts/ai/__init__.py`
- [ ] `scripts/ai/ai_pipeline.py`
- [ ] `scripts/ai/context_builder.py`
- [ ] `scripts/ai/data_aggregator.py`
- [ ] `scripts/ai/collectors/__init__.py`
- [ ] `scripts/ai/collectors/price_action.py`
- [ ] `scripts/ai/collectors/sec_collector.py`
- [ ] `scripts/ai/collectors/screener_collector.py`
- [ ] `scripts/ai/regime_detector.py`
- [ ] `scripts/ai/prompt_builder.py`
- [ ] `scripts/ai/claude_client.py`
- [ ] `scripts/ai/annotation_extractor.py`
- [ ] `scripts/ai/cache.py`

### New C++ files
- [ ] `libs/core/protocol/AIAnnotation.hpp`
- [ ] `libs/core/servermodel/AIContextManager.hpp`
- [ ] `libs/core/servermodel/AIContextManager.cpp`
- [ ] `libs/gui/render/AIAnnotationLayer.hpp`
- [ ] `libs/gui/render/AIAnnotationLayer.cpp`

### Modified C++ files
- [ ] `libs/core/protocol/` — Add AI message types to protocol enum
- [ ] `libs/core/servermodel/ServerDataModel.hpp` — Add `AIContextManager` member
- [ ] `libs/gui/UnifiedGridRenderer.h` — Add `AIAnnotationLayer` node
- [ ] `libs/gui/widgets/AICommentaryFeedDock.hpp/cpp` — Wire to `AIContextManager` signals
- [ ] `apps/sentinel-server/main.cpp` — Initialize `AIContextManager`

### Config
- [ ] Add `ANTHROPIC_API_KEY` to `.env` documentation
- [ ] Add `ai_refresh_interval_seconds` to server YAML config
- [ ] Add `ai_annotations_enabled` toggle to GUI config

---

## 15. Environment & Dependencies

### Python additions to `scripts/requirements.txt`
```
anthropic>=0.40.0       # Claude API
```

### Environment variables needed
```
ANTHROPIC_API_KEY=sk-ant-...     # Required for AI pipeline
SEC_API_USER_AGENT=...           # Already required for SEC
```

### No new C++ dependencies needed
- Annotation rendering uses existing MSDF + QSG infrastructure
- Claude API is Python-only (no C++ SDK needed)
- JSON parsing via existing `nlohmann-json`

---

*This document is the blueprint. Phase 1 can be started immediately — the SEC data pipeline and TradingView screener are already operational. The first working analysis (stocks, SEC + screener → Claude → commentary dock) requires only the Python AI layer and a thin C++ subprocess wrapper.*
