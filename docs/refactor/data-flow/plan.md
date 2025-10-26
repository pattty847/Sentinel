
# Sentinel Market Data Optimization Plan

Version: 1.0 (Senior Review)
Date: 2025-10-23

---

## Goals
- BookMap-level smoothness and latency.
- Dense, O(1) data path end-to-end (no dense↔sparse churn).
- Dynamic timeframe and tick resolution with user overrides.
- Render-time banding (no ingestion-time loss).

---

## Core Findings (Corrected)
- Missing columns come from band-filtered snapshots + timer carry-forward; some snapshots are empty or misaligned with mid → blank time buckets.
- Ingestion performs dense→sparse conversion; LTSE re-densifies via maps, then densifies again into tick vectors → wasted CPU and allocations.
- Nested locking in cache widens critical sections; per-symbol concurrency is artificially limited.
- Dispatcher at ~50µs is fine; not the bottleneck.
- LTSE stores redundant map-based snapshots; use dense updates directly instead.

---

## Phase Plan (Step-by-step)

### Phase 0 (Safety & UX)
1) Guard empty snapshots in LTSE
- In `LiquidityTimeSeriesEngine::addSnapshotToSlice`, if both bids/asks empty, return early.
- If you must advance time, create slice with prior tick range and zero metrics; do not derive invalid min/max.

2) Move banding to render-time
- In `DataProcessor::onLiveOrderBookUpdated`, stop creating band-limited sparse books.
- Keep `m_latestOrderBook` only for UI/compat, but sourced from dense → sparse conversion limited to viewport during rendering, not ingestion.

Impact: Eliminates vertical black columns from empty snapshots; preserves history.

### Phase 1 (Dense Event-Driven Ingestion)
1) LTSE dense API
- Add: `void addDenseOrderBookUpdate(const LiveOrderBook& book, std::span<const BookDelta> deltas);`
- For each timeframe: update current slice immediately on update; finalize slice when timestamp crosses boundary (e.g., 100ms, 250ms...).
- Remove map-based `OrderBookSnapshot` usage in the hot path.

2) DataProcessor wiring
- In `onLiveOrderBookUpdated`, call `ltse.addDenseOrderBookUpdate(liveBook, deltas);`
- Emit `dataUpdated()` to trigger render.
- Keep the 100ms timer temporarily but make it a no-op for LTSE (will remove in Phase 3).

Impact: Removes dense↔sparse churn; aligns updates with data arrival; lowers latency.

### Phase 2 (Lock Contention Reduction)
1) Flatten locking
- Add `LiveOrderBook::applyUpdatesNoLock(...)`.
- In `DataCache::applyLiveOrderBookUpdates`, hold only the cache's per-symbol lock and call `applyUpdatesNoLock`.
- Alternatively, restructure cache to a per-symbol mutex without outer unique_lock (preferred long-term).

Impact: Shorter critical sections; better multi-symbol scalability.

### Phase 3 (Remove Timer, Tighten Data Flow)
1) Remove 100ms sampler
- Delete `m_snapshotTimer` usage for LTSE ingestion; rely on event-driven updates.
- If visual continuity requires time-bucket alignment, LTSE should synthesize carry-forward updates internally when completing a slice.

2) Historical correctness
- LTSE retains full dense history per timeframe; rendering queries apply viewport and band intersection.

Impact: No stale carry-forward; fewer missed buckets; lower CPU.

### Phase 4 (User Controls & Rebinning)
1) Dynamic timeframe control
- Expose add/remove timeframe APIs, surfaced to QML (TradingView-like).
- Suggestion logic remains as default; manual override persists until reset.

2) Dynamic tick resolution
- Expose price resolution; when changed, rebin existing slices efficiently: create new vectors at new tick size and map prior ticks.
- Optionally reinitialize `LiveOrderBook` if the core tick changes (costly—prefer LTSE-only rebin for rendering).

3) Render band control
- Expose `renderBandWidth` (0 = full book).
- Apply band intersection during `updateVisibleCells` before geometry build.

Impact: UX parity with pro tools; performance tunable per machine.

### Phase 5 (Polish & GPU)
1) Best bid/ask O(1)
- Track `m_bestBidIndex/m_bestAskIndex` in `LiveOrderBook`; update on each level change.
- Replace scans in DataProcessor with O(1) getters.

2) GPU batching
- Precompute per-slice vertex buffers; rebuild only when slice finalizes or viewport/timeframe changes.
- Keep uniform uploads small; prefer index buffers for quads.

3) Cleanup
- Remove LTSE map-based snapshot store.
- Remove ingestion-time sparse DTO usage on hot path.

---

## File-by-File Edits (Concise)

- libs/gui/render/DataProcessor.cpp
  - onLiveOrderBookUpdated: remove dense→sparse banding; call ltse.addDenseOrderBookUpdate(liveBook, deltas); still update m_latestOrderBook for UI if needed (build sparse from viewport-band at render-time instead).
  - updateVisibleCells: compute band = min(viewport, userRenderBand); request slices with price-range hint; build cells only within band.
  - startProcessing/stopProcessing: stop using 100ms timer for LTSE ingestion (Phase 3).

- libs/core/LiquidityTimeSeriesEngine.h/.cpp
  - Add addDenseOrderBookUpdate API.
  - Maintain current slice per timeframe; finalize on boundary; write metrics O(1) by tick index.
  - addSnapshotToSlice: guard empty snapshots; if empty, either skip or write zeros without changing tick bounds.
  - Remove map-based snapshot persistence from hot path; keep only aggregated slices.
  - Expose setPriceResolution, add/remove timeframe, and price-range query for visible slices.

- libs/core/marketdata/cache/DataCache.cpp / model/TradeData.h
  - Add LiveOrderBook::applyUpdatesNoLock.
  - DataCache::applyLiveOrderBookUpdates: hold only per-symbol lock (or restructure to per-symbol mutex); call no-lock variant.
  - Optional: add getBestBid/Ask O(1).

- libs/gui/UnifiedGridRenderer.cpp
  - Ensure updateVisibleCells is called via BlockingQueuedConnection only when needed; avoid redundant calls per frame.
  - Keep GridViewState authoritative over viewport size/time range.

---

## Testing Matrix

- Unit (marketdata)
  - LiveOrderBook applyUpdatesNoLock thread safety (single-writer, multi-reader).
  - LTSE addDenseOrderBookUpdate aggregates correctly across timeframes.
  - Rebin correctness when price resolution changes.

- Integration
  - End-to-end: updates → LTSE → renderer (no banding at ingest), verify no blank columns on mid drift.
  - Zoom/LOD switching: suggestion vs manual override.
  - Multi-symbol throughput under load.

- Performance
  - Update latency: target < 150µs (ingest→LTSE).
  - Frame budget: < 16.67ms @ 60 FPS (geometry ≤ 5ms).
  - Memory: < 600MB typical (BTC full book dense + slices).

---

## Rollout Order
1) Phase 0, 1 (1–2 days): fix missing columns; dense ingestion.
2) Phase 2 (0.5–1 day): flatten locking.
3) Phase 3 (0.5 day): remove timer ingestion.
4) Phase 4 (1–2 days): user controls + rebin.
5) Phase 5 (1–2 days): O(1) best bid/ask; GPU batching; cleanup.

---

## Acceptance Criteria (BookMap bar)
- No blank time columns under any mid-price drift or quiet periods.
- Smooth 60 FPS with typical BTC feed on a modern Mac (M-series).
- < 150µs update path median, < 500µs p99.
- Dynamic tick/timeframes usable and responsive; band changes apply to full history instantly.
