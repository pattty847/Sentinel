Sentinel Rendering Features Plan

Scope: Volume Profile (pinned bottom bars), Footprint/TPO-style charts, Candlesticks with Coinbase historical backfill, Trade struct extensions, LTSE dynamic zoom/timeframe debug.

1) Issue outline: Dense→Sparse Order Book conversion
- Problem: DataProcessor converts dense `LiveOrderBook` to sparse `OrderBook` for LTSE. This duplicates work and loses O(1) characteristics.
- Constraints: LTSE expects sparse inputs today; API not ready for dense arrays.
- Risks: Performance regressions if we widen sparse conversion band; memory pressure if we keep dense and sparse simultaneously.
- Proposal: Phase A keep conversion but parameterize conversion band by midprice and viewport; Phase B add dense ingestion path to LTSE (tick-indexed), removing conversion.

2) Volume Profile (bottom-pinned bars)
- Data source: `LiquidityTimeSeriesEngine` visible slices aggregated into per-time bucket volume (sum of trade sizes or liquidity proxy).
- Implementation steps:
  - Aggregate per-slice bid+ask totals across visible range -> vector of (timeStartMs, volume).
  - Normalize volume to screen-height of profile strip; map to bars spanning the bottom area.
  - Render path: `UnifiedGridRenderer::updateVolumeProfile()` computes `m_volumeProfile` then `GridSceneNode::updateVolumeProfile()` builds geometry.
  - Interaction: Toggle via existing `setShowVolumeProfile(bool)`.
- Open questions: Use trades-only or liquidity-derived volume; smoothing; color by delta.

3) Trade struct extensions (computable fields)
- Add derived metrics cached alongside `Trade`:
  - CVD contribution: `signedSize = size * (side==Buy ? +1 : -1)` and cumulative per product.
  - Delta at time bucket: bids vs asks volume within slice.
  - Imbalance metrics: (ask - bid) / (ask + bid) per price band.
  - VWAP contribution fields: price*size and size accumulators for rolling VWAP.
- Optional authenticated fields (if available): taker/maker ids, profiles, fee rates.
- Storage: Do not bloat `Trade`; prefer side stores in processors; keep `Trade` DTO lean; compute and persist in `StatisticsProcessor` or LTSE slice metadata.

4) Footprint/TPO-style and ExoCharts-like extensions
- Building blocks present: LTSE provides time slices and tick-size price bands.
- Footprint bars: per slice, per price level metrics exist (bidMetrics/askMetrics) with total/avg/max/resting.
- We can render per-price rectangles per slice with color mapping for delta/imbalance; switch strategy mode.
- TPO-like: aggregate counts of touches per price; requires additional metric in `LiquidityTimeSlice` (touchCount per price level).
- Risks: GPU geometry size; must cap `maxCells` and use culling by viewport.

5) Candlesticks with Coinbase historical backfill
- API: Coinbase REST candles require intervals >=1m; need backfill loader separate from WS.
- Plan:
  - Add `HistoricalDataFetcher` (Qt network or beast HTTP) in `libs/core/marketdata/rest/` with rate-limit handling.
  - Store OHLCV in lightweight cache and expose via adapter to `DataProcessor`.
  - `CandleStrategy` already exists; feed it with OHLCV-derived cells mapped to screen via `CoordinateSystem`.
  - Support intervals above 1m (1m, 5m, 15m, 1h, etc.), integrate with timeframe property.
- Considerations: Stitch REST backfill with live WS; align timestamps; avoid gaps.

6) LTSE dynamic zoom/timeframe (debug scaffolding exists)
- Current: `GridViewState` handles zoom; `DataProcessor` suggests timeframe via `LTSE::suggestTimeframe` based on viewport span.
- Bug to debug: auto-suggestion not always updating on zoom; ensure `viewportChanged` triggers recompute and that manual override timeout resets.
- Action: add targeted logs and verify `viewportInitialized`/`viewportChanged` flow; add unit to test suggestion stability.

Next Steps
- Decide on data source for volume profile (trades vs liquidity). If trades, ensure trade ingestion path is wired to `DataProcessor` accumulation.
- Confirm whether to pursue dense ingestion in LTSE (Phase B) vs maintain conversion for now (Phase A).
- Spec REST fetcher interface and candle timeframes to support.


