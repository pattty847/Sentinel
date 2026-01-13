## 1. Sentinel v3 Client/Server + Storage Design Spec

### 1.1 Goals

- **Split** Sentinel into:

  - `sentinel-server` (headless daemon, runs 25/8)
  - `sentinel-gui` (Qt GUI client, can crash/restart without gaps)
- **Guarantee continuous OB history** (no gaps unless the exchange is actually down).
- **Store raw ticks short-term** + **pre-aggregated timeframes long-term**.
- **Stream data over a clean binary/WebSocket protocol**, not Qt signals.
- **Minimize invasive surgery** by reusing:

  - `MarketDataCore` (transport/dispatch/auth) 
  - `LiveOrderBook` + dense grid model
  - `LiquidityTimeSeriesEngine` & renderer on the client side 

---

### 1.2 New Process Layout

#### Processes

- **`apps/sentinel-server/`**

  - Headless binary (no QtGui/QML).
  - Uses `libs/core/marketdata` for ingestion.
  - Maintains **server-side RAM model + persistence + WebSocket streamer**.

- **`apps/sentinel-gui/`**

  - Existing Qt entry point.
  - Uses `libs/gui` as-is, but instead of talking to `DataCache` directly, it consumes a **network feed** via a new client module.

#### Shared Libraries

- `libs/core/`:

  - Remains the home of **transport, dispatch, models**, and any **shared protocol & storage logic**.
  - New submodule: `libs/core/servermodel/` for the server-side aggregated data model + persistence.

- `libs/gui/`:

  - Continues to own `UnifiedGridRenderer`, `GridViewState`, `LiquidityTimeSeriesEngine`, etc. 
  - Adds a “remote data source” implementation that feeds these from network instead of direct `DataCache` signals.

---

### 1.3 Market Data Flow: New Big Picture

**Today (monolith):**

Exchange → MarketDataCore → DataCache → Qt signals → DataProcessor → LiquidityTimeSeriesEngine → Renderer

**Target:**

**Server:**

```
Coinbase WS
  └→ MarketDataCore (unchanged transport/dispatch)
      └→ ServerDataModel (RAM rings + persistence)
          ├→ TickBinaryLogger (append-only logs)
          ├→ TimeframeAggregator (100ms, 1s, 5s, 1m slices)
          └→ ClientStreamer (WebSocket, binary)
```

**Client:**

```
SentinelStreamClient (WebSocket)
  └→ RemoteMarketDataSource (local cache in client process)
      └→ DataProcessor / LiquidityTimeSeriesEngine (unchanged API)
          └→ UnifiedGridRenderer / GridSceneNode (same renderer)
```

Key idea: **GUI sees the same shape of data**, but it now comes from a remote source instead of in-process DataCache.

---

### 1.4 Storage Model

We use a hybrid:

- **Raw tick binary logs** (short-retention, Sierra-style)
- **Pre-aggregated slices in SQLite or RocksDB** (longer retention)

#### 1.4.1 Directory Layout

On the machine running `sentinel-server`:

```text
data/
  sentinel.db               # SQLite metadata: symbols, segments, configs
  market/
    BTC-USD/
      ticks/
        2025-12-08_00.bin
        2025-12-08_01.bin
        ...
      agg_100ms/
      agg_1s/
      agg_5s/
      agg_1m/
    ETH-USD/
      ...
```

> Option: swap SQLite for RocksDB later without changing high-level APIs.

#### 1.4.2 Tick Segment Format

For raw order-book snapshots (short-term replay & recon):

```cpp
// File header for each segment
struct SegmentHeader {
    uint32_t magic = 0x53454E54;  // "SENT"
    uint16_t version = 1;
    uint16_t flags;               // compression bits, etc.
    int64_t start_timestamp_ms;
    int64_t end_timestamp_ms;
    uint32_t tick_count;
    uint32_t reserved;
};

// Tick snapshot record
struct TickSnapshotRecord {
    int64_t timestamp_ms;
    uint16_t bid_count;
    uint16_t ask_count;
    // Followed by bid_count * TickLevel
    // Followed by ask_count * TickLevel
};

struct TickLevel {
    int32_t price_index;  // integer index relative to LiveOrderBook min_price/tick_size
    float   quantity;
};
```

- **Price representation**: integer-indexed (as in `LiveOrderBook`) for speed & consistency.
- Segments are written as `.tmp`, fsynced, then atomically `rename`d and indexed in SQLite.

#### 1.4.3 Aggregated Slice Format

Timeframe slices are derived from tick snapshots:

- Base resolution: **100ms** (or 200/250ms if you want to chill).
- Rollups: **1s, 5s, 1m** minimally.
```cpp
struct AggregatedSlice {
    int64_t start_ts_ms;
    int64_t end_ts_ms;
    float total_bid_volume;
    float total_ask_volume;
    // Optional: per-price volume distribution compressed (RLE or bucketed).
    // For heatmap: a compact column format (price_index → volume).
};
```


These are stored:

- In RAM ring buffers (for recent `N` slices).
- Persisted in per-resolution binary segment files (`agg_1s/*.bin`) + indexed in SQLite.

#### 1.4.4 SQLite Schema (Metadata + Segment Index)

```sql
CREATE TABLE symbols (
    id INTEGER PRIMARY KEY,
    symbol TEXT UNIQUE NOT NULL,
    exchange TEXT NOT NULL,
    tick_size REAL NOT NULL,
    min_price REAL,
    max_price REAL,
    created_at INTEGER
);

CREATE TABLE segments (
    id INTEGER PRIMARY KEY,
    symbol_id INTEGER REFERENCES symbols(id),
    resolution_ms INTEGER NOT NULL,      -- 0 = raw ticks, 100, 1000, 5000, etc.
    start_time_ms INTEGER NOT NULL,
    end_time_ms INTEGER NOT NULL,
    file_path TEXT NOT NULL,
    byte_offset INTEGER NOT NULL,
    byte_length INTEGER NOT NULL,
    record_count INTEGER NOT NULL,
    UNIQUE(symbol_id, resolution_ms, start_time_ms)
);

CREATE INDEX idx_segments_symbol_time
ON segments(symbol_id, resolution_ms, start_time_ms);
```

SQLite is **only** metadata + indexing. Bulk data lives in append-only binary files.

---

### 1.5 Server RAM Model

New module: `libs/core/servermodel/`.

```cpp
struct SymbolHotData {
    std::string symbol;
    LiveOrderBook liveBook;                      // existing dense O(1) OB :contentReference[oaicite:6]{index=6}  

    RingBuffer<TickSnapshot, N_TICKS>;          // recent raw snapshots, e.g. 30–60 min

    std::unordered_map<int64_t, RingBuffer<AggregatedSlice, N_SLICES>>
        aggByResolution;                        // keys: 100ms, 1000, 5000, 60000, etc.
};

class ServerDataModel {
public:
    SymbolHotData& ensureSymbol(const std::string& symbol);
    void onBookSnapshot(const BookSnapshotEvent& ev);
    void onBookUpdate(const BookUpdateEvent& ev);
    void onTrade(const TradeEvent& ev);
    // Accessors for streamer and persistence
};
```

- `MarketDataCore` dispatches events to `ServerDataModel` instead of just `DataCache`.
- `ServerDataModel` is the **single source of truth** for live OB and recent history in the server process.

---

### 1.6 Aggregation & Retention Policy

- **Base tick snapshots**: every 100ms (configurable).

- For each symbol:

  - Keep raw tick snapshots in RAM for, say, **30–60 minutes**.
  - Keep higher timeframes in RAM for longer (1–12 hours).
  - Persist all slices to disk as segments for replay.

- Background jobs:

  - Roll raw → 100ms slices (if you don’t want to snapshot every 100ms directly).
  - Roll 100ms → 1s → 5s → 1m.
  - Delete old raw tick segments beyond retention (e.g., >3 days).

---

### 1.7 Streaming Protocol

Transport: **WebSocket over TCP**, implemented with Boost.Beast (already present).

**v0**: JSON for dev / debugging.

**v1**: Compact binary framing with the same message model.

#### 1.7.1 Basic Message Types

- Client → Server:

  - `subscribe` (symbol, timeframe, feeds)
  - `unsubscribe`
  - `snapshot_request`
  - `ping`

- Server → Client:

  - `subscribe_ack`
  - `snapshot`
  - `slice_batch` (for aggregated heatmap columns)
  - `tick_delta` (live incremental updates)
  - `pong`
  - `error`

Example JSON v0:

```json
{ "type": "subscribe",
  "symbol": "BTC-USD",
  "timeframe_ms": 1000,
  "feeds": ["heatmap", "trades"],
  "lookback_ms": 1_800_000
}
```

Server response:

```json
{
  "type": "snapshot",
  "symbol": "BTC-USD",
  "timeframe_ms": 1000,
  "start_ts_ms": 1733650000000,
  "end_ts_ms": 1733651800000,
  "slices": [ ... ]   // AggregatedSlice array, serialized
}
```

Then:

```json
{
  "type": "slice_batch",
  "symbol": "BTC-USD",
  "timeframe_ms": 1000,
  "slices": [ ... ]   // newly appended slices as time advances
}
```

Binary v1 will wrap these in `MessageHeader { length, type, flags, sequence }` etc., but Gemini can wire that up later.

---

### 1.8 Client Integration

We don’t want to rewrite `LiquidityTimeSeriesEngine` or your renderer.

Instead:

1. Introduce an abstraction in `libs/gui`:
   ```cpp
   class IGridDataSource {
   public:
       virtual ~IGridDataSource() = default;
       virtual GridDataSnapshot currentSnapshot(const QString& symbol,
                                                Timeframe tf) = 0;
       virtual void subscribe(const QString& symbol, Timeframe tf) = 0;
       // maybe signals/callbacks when new slices arrive
   };
   ```

2. Implement `RemoteGridDataSource` (network-backed):
   ```cpp
   class RemoteGridDataSource : public QObject, public IGridDataSource {
       Q_OBJECT
   public:
       explicit RemoteGridDataSource(QObject* parent = nullptr);
       void connectToServer(const QUrl& url);
   
       GridDataSnapshot currentSnapshot(const QString& symbol,
                                        Timeframe tf) override;
   
       void subscribe(const QString& symbol, Timeframe tf) override;
   
   signals:
       void slicesUpdated(const QString& symbol, Timeframe tf);
   
   private:
       SentinelStreamClient m_client;   // WS client
       // local buffer of slices per symbol/timeframe
   };
   ```

3. Update `DataProcessor` / `LiquidityTimeSeriesEngine` ctor to take an `IGridDataSource&` instead of pulling straight from `DataCache`.

4. For now, you can keep a **LocalGridDataSource** implementation that reads from `DataCache` for monolithic mode. That makes migration gradual.

---

### 1.9 Server Startup & Warmup

When `sentinel-server` starts:

1. Open SQLite and load `symbols` table for config.
2. For each enabled symbol:

   - Load last few segments of 100ms/1s slices from disk.
   - Populate `SymbolHotData` ring buffers with recent history.
   - Initialize `LiveOrderBook` from last snapshot (or request snapshot from exchange if needed).

3. Start `MarketDataCore` and connect to Coinbase.
4. Start `ClientStreamer` WebSocket server and accept clients.

On reconnect after a crash, clients:

- Re-subscribe.
- Receive an initial snapshot window from the server’s RAM model.
- Transition to live streaming.

---

### 1.10 Migration Strategy (High-Level)

Phases for Gemini to plan concretely:

1. **Introduce Protocol & DataSource abstractions** in code without changing runtime behavior.
2. **Add sentinel-server app**:

   - Reuse `MarketDataCore`.
   - Add `ServerDataModel`, `TickBinaryLogger`, `TimeframeAggregator`.
   - Add a primitive WebSocket streamer (JSON v0).

3. **Add RemoteGridDataSource** on client side and a config toggle:

   - “Use remote server” vs “Use local MarketDataCore”.

4. Flip default to remote mode once stable; deprecate direct GUI ↔ DataCache integration.

Gemini’s job will be to map this onto:

- **Existing files & namespaces** from the two docs
- Add new modules in the right places
- Wire the refactor in multiple stages.

---

## 2. Prompt for Gemini 3 (you paste this)

Here’s the big boy prompt. You can tweak names, but this is the core:

---

**PROMPT FOR GEMINI 3: SENTINEL CLIENT/SERVER + STORAGE IMPLEMENTATION PLAN**

You are refactoring a real C++20/Qt6 trading terminal called **Sentinel**.

I’m splitting it into a **headless server** and a **Qt GUI client**, with a new storage & streaming layer. Your job is to:

- Map the target design onto the **existing codebase.**
- Decide what migrates, what stays, and what becomes shared.
- Produce a **detailed, code-aware implementation plan** (files, classes, phases).
- Stay consistent with the documented architecture and my build layout.

---

### 0. Current Codebase Docs (READ THESE FIRST)

Use these as the source of truth about how the system is built today:

1. **ARCHITECTURE.md** – high-level layout, layers, render pipeline:
```markdown
[PASTE FULL CONTENTS OF ARCHITECTURE.md HERE]
```

2. **MARKETDATA_ARCHITECTURE.md** – MarketDataCore, WsTransport, DataCache, LiveOrderBook, threading, etc.:
```markdown
[PASTE FULL CONTENTS OF MARKETDATA_ARCHITECTURE.md HERE]
```


You must anchor all your recommendations to these existing modules, namespaces, and directories. Do **not** invent random new structures that contradict these docs.

---

### 1. Target Design (TO IMPLEMENT)

Here is the **target architecture** I want you to implement or plan in detail.

```markdown
[PASTE THE FULL DESIGN SPEC FROM CHATGPT HERE – EVERYTHING ABOVE THIS PROMPT]
```

Treat this as the blueprint: server vs client, storage model, WebSocket streaming, RAM model, etc.

---

### 2. Your Tasks

Given:

- The **current design** (ARCHITECTURE + MARKETDATA_ARCHITECTURE)
- The **target design** (Sentinel v3 client/server + storage spec)

I want you to produce a concrete, code-aware plan with the following sections:

---

#### A. Current → Target Mapping

1. For each major component in the current design:

   - `MarketDataCore`
   - `DataCache`
   - `LiveOrderBook`
   - `LiquidityTimeSeriesEngine`
   - `UnifiedGridRenderer` / `GridViewState`
   - `BeastWsTransport`, `SubscriptionManager`, `Authenticator`

Explain:

   - **Where it lives today** (file paths / namespaces).
   - **Whether it should live in the server, client, or shared lib** in the new architecture.
   - Any **API surface changes** needed (e.g., DataCache → ServerDataModel).

2. Explicitly list:

   - Components that **stay in `libs/core`** as shared infrastructure.
   - Components that must be **moved or wrapped** for `sentinel-server`.
   - Components that remain strictly **GUI-only**.

---

#### B. New Modules & Files

Based on the target design, define:

1. New **modules/namespaces** and their proposed locations, for example:

   - `libs/core/servermodel/`

     - `ServerDataModel.hpp/.cpp`
     - `SymbolHotData.hpp`
     - `TickBinaryLogger.hpp/.cpp`
     - `TimeframeAggregator.hpp/.cpp`

   - `libs/core/protocol/`

     - `SentinelStreamProtocol.hpp` (message enums, headers, serialization)
     - `SentinelStreamServer.hpp/.cpp` (WebSocket server)
     - `SentinelStreamClient.hpp/.cpp` (client-side WebSocket)

   - `apps/sentinel-server/`

     - `main.cpp`
     - `SentinelServerApp.hpp/.cpp`

   - `libs/gui/datasources/`

     - `IGridDataSource.hpp`
     - `RemoteGridDataSource.hpp/.cpp`
     - Optionally `LocalGridDataSource` (for monolith mode).

2. For each new file:

   - Responsibilities.
   - Key public methods.
   - Which existing classes it will interact with.

---

#### C. Storage & Aggregation Implementation Plan

Using the spec’s storage model:

1. Design **C++ types** and **APIs** for:

   - Tick segment writer/reader.
   - AggregatedSlice structures.
   - SQLite metadata/index access layer.

2. Show:

   - Where you will hook this into the current pipeline:

     - e.g., after `LiveOrderBook::applyUpdate()` or in `MarketDataCore` handlers.
   - How aggregation will be done:

     - raw tick → 100ms → 1s → 5s → 1m.
   - How retention and cleanup will work (which component owns it).

3. Explain how `sentinel-server` will warm up:

   - On startup, reading recent segments.
   - Reconstructing RAM ring buffers for `ServerDataModel`.
   - Re-initializing `LiveOrderBook` for each symbol.

---

#### D. Streaming Path & Client Integration

1. Define the **WebSocket streaming API** concretely:

   - Exact JSON v0 messages (subscribe, snapshot, slice_batch, tick_delta, error).
   - Proposed binary v1 framing (structs, enums, sequences).

2. Specify how `SentinelStreamServer` will:

   - Subscribe clients to specific symbols/timeframes.
   - Serve an initial snapshot (from RAM + disk).
   - Push incremental updates as new slices arrive.

3. On the client side (`libs/gui`):

   - Show how `RemoteGridDataSource` will be implemented using `SentinelStreamClient`.
   - Show how `DataProcessor` / `LiquidityTimeSeriesEngine` will be adapted:

     - e.g., add an `IGridDataSource&` parameter so it can work with remote or local data.
   - Ensure this integration *does not* break the existing renderer abstractions (`UnifiedGridRenderer`, `GridSceneNode`, etc.).

---

#### E. Phased Migration Plan (Step-by-Step)

I want a **multi-phase plan** where each phase leaves the project in a compiling, runnable state.

For each phase, describe:

- **Goal**
- **Files touched**
- **New classes introduced**
- **What still uses old behavior**
- **How to test it locally**

Suggested phase outline (you can refine):

1. **Phase 0 — Abstractions Only**

   - Introduce `IGridDataSource` in `libs/gui`.
   - Keep existing direct DataCache usage working via `LocalGridDataSource`.

2. **Phase 1 — Add sentinel-server Skeleton**

   - Add `apps/sentinel-server/` with a simple `main` that wires `MarketDataCore` → `ServerDataModel` (no persistence yet).
   - Add a trivial `SentinelStreamServer` that streams JSON snapshots from ServerDataModel.

3. **Phase 2 — Client Remote Mode**

   - Add `RemoteGridDataSource` and `SentinelStreamClient`.
   - Add a runtime toggle to `sentinel-gui` to choose between local vs remote data.
   - Confirm that in remote mode, the UI still renders books and heatmaps correctly.

4. **Phase 3 — Persistence & Aggregation**

   - Implement TickBinaryLogger + TimeframeAggregator + SQLite index.
   - Wire them into ServerDataModel.
   - Add server warmup from disk and confirm basic replay.

5. **Phase 4 — Optimization & Binary Protocol**

   - Switch from JSON to binary framing for high-frequency streaming.
   - Add compression or downsampling options if needed.

6. **Phase 5 — Cleanup / Deprecate Old Path**

   - Remove or freeze the legacy GUI-direct-to-MarketDataCore path.
   - Document the new architecture as the default.

---

#### F. Pitfalls & Refactor Risks

Call out the main **risk areas** and how to mitigate them:

- Threading: avoiding races between MarketDataCore, ServerDataModel, and streamer threads.
- Qt signal changes: not breaking existing GUI event flow unexpectedly.
- Backward compatibility: keeping a working monolith mode while remote mode stabilizes.
- Performance: making sure aggregation and logging don’t stall the I/O thread.

---

### 3. Output Style

- Target: **senior C++/Qt dev** who already knows this codebase.
- Provide:

  - Concrete class names, namespaces, and approximate file paths.
  - Clear bullet lists, not huge prose blocks.
- Assume I will use your plan as a **blueprint for implementation**, and will ask you later for detailed headers/implementations per class.

---

Use EVERYTHING above to produce a cohesive, realistic implementation plan that respects the current architecture and guides the migration to the new server/client + storage + streaming model.