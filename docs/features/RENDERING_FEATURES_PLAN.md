Sentinel Rendering Features Plan

**NOTE:** Detailed Footprint/TPO architecture and implementation blueprint defers to `docs/FOOTPRINT_TPO_BLUEPRINT.md`.

Scope: Volume Profile (pinned bottom bars), Footprint/TPO-style charts, Candlesticks with Coinbase historical backfill, Trade struct extensions, LTSE dynamic zoom/timeframe debug.

## Ownership & Boundaries
- **Core (`libs/core/`)**: `FootprintGrid`, `FootprintProcessor` — pure C++, no Qt dependencies
- **GUI (`libs/gui/`)**: `FootprintStrategy`, `HistoricalDataFetcher`, wiring in `UnifiedGridRenderer`/`DataProcessor` — Qt adapters and rendering
- **Data Flow**: Exchange → MarketDataCore → DataProcessor (owns processors) → strategies → UnifiedGridRenderer

1) Issue outline: Dense→Sparse Order Book conversion
- Problem: DataProcessor converts dense `LiveOrderBook` to sparse `OrderBook` for LTSE. This duplicates work and loses O(1) characteristics.
- Constraints: LTSE expects sparse inputs today; API not ready for dense arrays.
- Risks: Performance regressions if we widen sparse conversion band; memory pressure if we keep dense and sparse simultaneously.
- **Current approach**: Phase A (parameterized band by midprice and viewport) is active. Phase B (dense tick-indexed LTSE ingestion) deferred to avoid scope creep during footprint implementation.

2) Volume Profile (bottom-pinned bars)
- **Data source (MVP)**: Trades-only for initial implementation; aggregate trade sizes per time bucket. Liquidity-derived volume optional enhancement later.
- Implementation steps:
  - Aggregate per-bucket bid+ask trade volumes across visible range → vector of (timeStartMs, buyVol, sellVol).
  - Normalize volume to screen-height of profile strip; split bars by buy (green top) / sell (red bottom) portions.
  - Render path: `FootprintProcessor::computeVolumeProfile()` aggregates cells → `UnifiedGridRenderer::updateVolumeProfile()` computes `m_volumeProfile` → `GridSceneNode::updateVolumeProfile()` builds geometry.
  - Interaction: Toggle via existing `setShowVolumeProfile(bool)`.
- Future enhancements: Smoothing, color by delta imbalance, hybrid liquidity+trade metrics.

3) Trade struct extensions (computable fields)
- Add derived metrics cached alongside `Trade`:
  - CVD contribution: `signedSize = size * (side==Buy ? +1 : -1)` and cumulative per product.
  - Delta at time bucket: bids vs asks volume within slice.
  - Imbalance metrics: (ask - bid) / (ask + bid) per price band.
  - VWAP contribution fields: price*size and size accumulators for rolling VWAP.
- Optional authenticated fields (if available): taker/maker ids, profiles, fee rates.
- Storage: Do not bloat `Trade`; prefer side stores in processors; keep `Trade` DTO lean; compute and persist in `StatisticsProcessor` or LTSE slice metadata.

4) Footprint/TPO-style and ExoCharts-like extensions
**See `docs/FOOTPRINT_TPO_BLUEPRINT.md` for detailed architecture.**

### Phase 1–2: Live-Only Footprint (MVP)
- **No historical backfill** — build footprint from live trades only; daemon for historical data deferred to Phase 3+.
- Architecture:
  - `FootprintGrid`: Circular time buffer (bounded `maxTimeBuckets`), sliding price window (bounded `maxPriceLevels`), row-major cell layout.
  - `FootprintProcessor` (core, Qt-free): Aggregates trades into time×price buckets; owned by `DataProcessor`.
  - `FootprintStrategy` (GUI): Implements `IRenderStrategy`; pulls `FootprintSlice` from `DataProcessor`; builds QSG quads with 95th-percentile delta normalization.
- Data flow: Trade → `StatisticsController` → `DataProcessor` → `FootprintProcessor::onTrade()` → strategy queries via `getVisibleFootprint()`.
- Color mapping: 95th percentile of visible deltas (not raw min/max) for robust normalization; diverging palette (ExoCharts-style muted green/red).
- Rendering: Strategy-based QSG geometry (no raw OpenGL VBOs in `UnifiedGridRenderer`); viewport culling via `CoordinateSystem`.
- Delta modes: Raw, Cumulative, Imbalance, Stacked (configurable).
- Price chunking: Use instrument tick size from product metadata; dynamic viewport-aware chunking optional later.
- TPO profile: Aggregate touch counts per price level across time periods; mark POC/Value Area; render as side panel histogram.
- Risks: GPU geometry size capped by `maxCells` and aggressive viewport culling.

5) Candlesticks with Coinbase historical backfill
**DEFERRED until after Phase 1–2 Footprint ships.**

- API: Coinbase REST candles require intervals >=1m; need backfill loader separate from WS.
- Plan:
  - Add `HistoricalDataFetcher` in `libs/gui/network/` (Qt network, NOT in core) with rate-limit handling (10 req/sec public API).
  - GUI caches OHLCV in `DataProcessor` candle cache; no Qt types cross into core.
  - `CandleStrategy` already exists; feed it with OHLCV-derived cells mapped to screen via `CoordinateSystem`.
  - Support intervals above 1m (1m, 5m, 15m, 1h, etc.), integrate with timeframe property.
- Considerations: Stitch REST backfill with live WS; align timestamps; avoid gaps. Mock network calls in tests (no live HTTP in CI).

6) LTSE dynamic zoom/timeframe (debug scaffolding exists)
- Current: `GridViewState` handles zoom; `DataProcessor` suggests timeframe via `LTSE::suggestTimeframe` based on viewport span.
- Bug to debug: auto-suggestion not always updating on zoom; ensure `viewportChanged` triggers recompute and that manual override timeout resets.
- Action: add targeted logs and verify `viewportInitialized`/`viewportChanged` flow; add unit to test suggestion stability.

## Footprint/TPO Roadmap (Live-First)

### Phase 1: Core Footprint Grid & Processor (Live-Only)
- Implement `FootprintGrid` with circular time buffer and sliding price window (bounded memory).
- Implement `FootprintProcessor` in core (Qt-free); aggregate trades into cells.
- Wire `DataProcessor` to own `FootprintProcessor` and forward trades + viewport changes.
- Add unit tests for ring buffer wrap-around, price window management, delta modes.

### Phase 2: Footprint Rendering Strategy
- Implement `FootprintStrategy` (QSG quads) with 95th-percentile normalization.
- Add `RenderMode::Footprint` to `UnifiedGridRenderer` and wire strategy selection.
- Integrate with existing viewport culling and `CoordinateSystem` transforms.
- Add UI controls for delta mode, timeframe, color scheme.

### Phase 3: Volume Profile & TPO Extensions
- Volume profile bottom histogram from footprint cell aggregation.
- TPO profile side panel with POC/Value Area computation.
- Cell picking and tooltips on hover.

### Phase 4+: Historical Backfill (Deferred)
- Build daemon for 24/7 orderbook snapshot + trade collection.
- Store pre-computed footprint grids in TimescaleDB/ClickHouse.
- Add `loadHistoricalGrid()` method to warm-start footprint on product switch.
- Implement `HistoricalDataFetcher` in GUI for REST candle backfill.

## Next Steps
- Finish current branch fix; merge to `main`.
- Branch `feature/tpo-footprint` and implement per `docs/FOOTPRINT_TPO_BLUEPRINT.md`.
- Trades-only volume profile for MVP (liquidity-derived deferred).
- Maintain Phase A dense→sparse conversion (Phase B LTSE dense ingestion deferred).


