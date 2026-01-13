# Server/Client Cutover (GPU Heatmap)

## Context Snapshot (read first after compaction)

**Where we are**
- Dummy GPU heatmap is solid: single quad, 8192x8192, smooth pan/zoom, append via ring buffer.
- Live GPU heatmap is **partially working** but unstable: missing columns, viewport pinned, auto‑scroll conflicts.
- Root mismatch: we were rasterizing **LTSE activity slices** (flickery) instead of **resting book snapshots**.

**Current live GPU path (in progress)**
- `SENTINEL_GPU_HEATMAP=1` enables GPU heatmap path in `UnifiedGridRenderer`.
- Columns are enqueued via `heatmapColumnReady`.
- We added a **resting mode**: snapshot full LiveOrderBook on the 100ms timer and rasterize the full book depth into $1 rows.  
  - This bypasses LTSE slices when `SENTINEL_HEATMAP_RESTING=1` or `SENTINEL_GPU_HEATMAP_ONLY=1`.
- Tick aggregation for heatmap rows is currently fixed at **$1 per row**.
- We added **gap fill** in `captureOrderBookSnapshot()` to fill missed 100ms buckets when GPU mode is on.

**Key pain points observed**
- 100ms bucket logs skip time → missing columns.
- Auto‑scroll fights manual pan; horizontal drag often feels pinned.
- Debug marker + force‑full view toggles were used to verify texture writes.
- Logs show LTSE timeframes (100/250/500/1000/2000/5000/10000ms) even though GPU heatmap only uses 100ms.

**Live data flow today (monolith)**
1) MarketDataCore updates DataCache LiveOrderBook.
2) MainWindowGPU wires `liveOrderBookUpdated` → DataProcessor (worker thread).
3) DataProcessor runs 100ms timer and emits GPU columns.

**Target end state (server/client)**
- Server owns ingestion + LiveOrderBook + timers + rasterization.
- Client only receives **pre‑built columns** and renders GPU heatmap.
- No local MarketDataCore or LTSE in client.

**Debug env vars**
- `SENTINEL_GPU_HEATMAP=1`
- `SENTINEL_GPU_HEATMAP_ONLY=1`
- `SENTINEL_HEATMAP_RESTING=1`
- `SENTINEL_HEATMAP_GRID=<size>`
- `SENTINEL_HEATMAP_FILL_GAPS=1`
- `SENTINEL_HEATMAP_RECENTER=<fraction>`
- `SENTINEL_GPU_HEATMAP_DEBUG=1`
- `SENTINEL_GPU_HEATMAP_DEBUG_MARK=1`
- `SENTINEL_GPU_HEATMAP_FORCE_FULL=1`

**Recent code changes**
- DataProcessor: resting snapshot mode + full book rasterization (`rasterizeBookToColumn`).
- DataProcessor: $1 tick size for heatmap.
- UnifiedGridRenderer: GPU heatmap path, column enqueue, timeOffset, viewport init.
- heatmap shader: zero intensity renders black.

**Known gaps**
- Resting snapshot mode still not stable visually; needs verification.
- Panning/auto‑scroll needs cleanup (likely decouple viewport from live head when auto is off).

Goal: make the GUI a **pure client** and move all ingestion/aggregation to the headless server.  
No monolith fallback. Fast, deterministic, and GPU‑first.

## Heatmap Semantics (authoritative spec)

**Mode (default)**
- Time‑Weighted Average (TWAP) of resting liquidity.
- Future modes (not implemented): Snapshot, Max.

**Timeframe alignment**
- Wall‑clock, UTC‑aligned buckets.
- Example: 100ms buckets start at `.000`, `.100`, `.200`, etc.

**State persistence**
- Carry‑forward is mandatory.
- Each bucket starts with the exact resting book at the previous bucket close.

**TWAP math**
- Per price level: `heat = sum(size_i * dt_i) / bucket_duration`.
- If no updates, the last size persists for the full `dt`.

**Live tip**
- The right‑most column shows the current live snapshot while the bucket is open.
- When the bucket closes, it is baked into the historical TWAP value.

**Data contract**
- Dense column buffer (uint8 or float16).
- Metadata: `timestamp`, `minPrice`, `maxPrice`, `tickSize`, `midPrice`, `lastTrade`.

**Price range policy**
- Dynamic hysteresis band centered on mid price.
- Default range: ±20% around mid price.
- Recenter when mid price is within 5% of the edge; emit reset/clear signal.

**Tick size**
- Configurable per symbol on the server (e.g., BTC $0.50, SOL $0.01).
- Choose height to fit GPU texture limits (2048/4096).

**Backfill & drift**
- On connect: server sends last N columns (e.g., 1000).
- If exchange is silent: continue sending carry‑forward columns at bucket cadence.

**Lag strategy**
- Drop older columns; keep client synced to latest live column.

## Phase 1 — Remote‑Only GUI (no local ingestion)

**Intent**: The GUI should never construct `MarketDataCore` or `LocalGridDataSource`.

- Force `SENTINEL_REMOTE=1` for GUI builds/runs.
- Remove `LocalGridDataSource` usage in `MainWindowGPU`.
- Remove `ServiceLocator::registerMarketDataCore` from GUI.
- Disable or rewire widgets that depend on MarketDataCore:
  - `OrderBookDock` (uses MarketDataCore)
  - `MarketDataPanel` (uses MarketDataCore)

**Files**
- `libs/gui/MainWindowGpu.cpp`
- `libs/gui/widgets/ServiceLocator.cpp`
- `libs/gui/widgets/OrderBookDock.cpp`
- `libs/gui/widgets/MarketDataPanel.cpp`
- `libs/gui/datasources/LocalGridDataSource.cpp` (remove from GUI build path)

## Phase 2 — Server emits heatmap slices

**Intent**: Server owns book, timers, and column rasterization.

- Add a server‑side `RestingHeatmapStreamer`:
  - 100ms timer
  - Snapshot `LiveOrderBook`
  - $1 bucket aggregation
  - Emit dense column bytes
- Add protocol message: `heatmap_slice`
  - `symbol`, `timeStart`, `timeEnd`, `tickSize`
  - `minPrice`, `maxPrice`
  - `gridHeight`, `columnBytes`

**Files**
- `libs/core/servermodel/ServerDataModel.cpp/.h`
- `libs/core/protocol/SentinelStreamServer.cpp/.h`
- `libs/core/protocol/SentinelStreamClient.cpp/.h`
- `libs/gui/datasources/RemoteGridDataSource.cpp/.h`

## Phase 3 — Client consumes slices only

**Intent**: Client only uploads GPU columns; no LTSE aggregation in GUI.

- Extend `RemoteGridDataSource` to emit `heatmapColumnReady`.
- `UnifiedGridRenderer` consumes columns and renders single‑quad GPU heatmap.
- Remove/disable client‑side LTSE usage in GPU path.

**Files**
- `libs/gui/datasources/RemoteGridDataSource.cpp/.h`
- `libs/gui/UnifiedGridRenderer.cpp/.h`
- `libs/gui/render/DataProcessor.cpp/.h` (remove LTSE aggregation in client)

## Phase 4 — Backfill / History

**Intent**: Client can request history on connect.

- Add `get_history` request/response.
- Server streams historical columns to prefill texture.

## Cut List (delete or hard‑disable)

- Client MarketDataCore creation (`DataBootstrapper` in GUI app)
- `LocalGridDataSource` in GUI runtime
- CPU heatmap path (`HeatmapStrategy`) once GPU is stable

## Notes / Non‑Goals

- No Qt in core layer (unchanged).
- GPU heatmap remains a **single quad** with streaming column uploads.
- LTSE can remain server‑side for an “activity heatmap” later, but is not required for resting.
