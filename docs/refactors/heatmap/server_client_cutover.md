# Server/Client Cutover (GPU Heatmap)

## Context Snapshot (read first after compaction)

**Where we are**
- Dummy GPU heatmap is solid: single quad, 8192x8192, smooth pan/zoom, append via ring buffer.
- Live GPU heatmap is **working** in server/client mode: stable 60 FPS, no missing columns.
- Root mismatches resolved: server is the only column producer; client only renders incoming slices.

**Current live GPU path**
- Server emits **TWAP resting liquidity** columns (`u8` intensity).
- Client consumes `heatmap_slice` and renders a single-quad heatmap.
- Timeframe is locked by `SENTINEL_HEATMAP_TF` (default 100ms).
- Tick size and grid height are authoritative from server.

**Resolved pain points**
- FPS drops caused by dual column producers (server 2048 vs local 8192) are fixed by disabling local producers.
- Timeframe flapping fixed by locking to `SENTINEL_HEATMAP_TF`.
- Asks/bids now render with distinct colors (u8 side encoding).

**Live data flow today (monolith)**
1) MarketDataCore updates DataCache LiveOrderBook.
2) MainWindowGPU wires `liveOrderBookUpdated` → DataProcessor (worker thread).
3) DataProcessor runs 100ms timer and emits GPU columns.

**Target end state (server/client)**
- Server owns ingestion + LiveOrderBook + timers + rasterization.
- Client only receives **pre‑built columns** and renders GPU heatmap.
- No local MarketDataCore or LTSE in client.

**Minimal env vars**
- `SENTINEL_GPU_HEATMAP=1` (client)
- `SENTINEL_HEATMAP_TF=100` (server + client)
- `SENTINEL_HEATMAP_TICK_SIZE=<value>` (server)
- `SENTINEL_HEATMAP_GRID=<size>` (server)
- `SENTINEL_GPU_HEATMAP_DEBUG=1` (optional client)

**Recent code changes**
- Server: `HeatmapTwapStreamer` emits TWAP columns (`u8` encoded, asks/bids split by range).
- Client: `DataProcessor` ignores local LTSE columns in remote mode.
- Renderer: resizes texture to server grid height; ring-buffer upload only.
- Shader: low-intensity visibility floor for asks (avoids invisible reds).

**Known gaps**
- Consolidate debug logging into a single file-based stream.
- Re-enable recenter with wide hysteresis once stable.
- Tick size presets per symbol (server).

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

- GUI is remote‑only by default (no `SENTINEL_REMOTE` toggle).
- Remove `LocalGridDataSource` usage in `MainWindowGPU`.
- Do not register `MarketDataCore`/`DataCache` in `ServiceLocator`.
- Disable or rewire widgets that depend on MarketDataCore:
  - `OrderBookDock` (kept, currently inert)
  - `MarketDataPanel` (removed from GUI build)

**Files**
- `libs/gui/MainWindowGpu.cpp`
- `libs/gui/widgets/ServiceLocator.cpp`
- `libs/gui/widgets/OrderBookDock.cpp`
- `libs/gui/widgets/MarketDataPanel.cpp` (removed)
- `libs/gui/datasources/LocalGridDataSource.cpp` (remove from GUI build path)

## Phase 2 — Server emits heatmap slices

**Intent**: Server owns book, timers, and column rasterization.

- Add a server‑side `RestingHeatmapStreamer`:
  - 100ms timer
  - Snapshot `LiveOrderBook`
  - $1 bucket aggregation
  - Emit dense column bytes
  - **TWAP resting liquidity implemented** (time‑weighted average per row)
  - Emits `heatmap_slice` with base64 `u8` column payload
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
  - **Done:** client consumes `heatmap_slice` and uploads columns directly.
  - **Done:** non‑100ms slices ignored to avoid flicker.

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
- `MarketDataPanel` removed from GUI build/layout (OrderBookDock kept inert)

## Notes / Non‑Goals

- No Qt in core layer (unchanged).
- GPU heatmap remains a **single quad** with streaming column uploads.
- LTSE can remain server‑side for an “activity heatmap” later, but is not required for resting.
- Server currently runs with **recenter disabled** to prevent texture rebuild flicker.
