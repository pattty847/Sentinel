# Tick-Based Candle Strategy

This document explains how to implement tick-driven candlesticks that reuse Sentinel's existing market data pipeline and rendering stack. It covers trade-count candles, time-aligned fallback behavior, and how to feed the GUI `CandleStrategy` without breaking current data flow contracts.

## Goals
- Generate OHLCV candles from the live trade stream with deterministic boundaries.
- Support **trade-count mode** (N trades per candle) and **time-seeded mode** (aligns to wall-clock boundaries to interoperate with timeframes).
- Preserve compatibility with `MarketDataCore` dispatch/cache layers and the GUI `VolumeCandles` render mode.

## Existing Building Blocks
- **Trade ingress:** `MarketDataCore` emits `tradeReceived(Trade)` from the worker thread after the dispatch layer normalizes exchange messages.【F:docs/MARKETDATA_ARCHITECTURE.md†L13-L64】
- **Caching:** `DataCache` keeps a thread-safe ring buffer of recent trades per product, which can be sampled without blocking dispatch.【F:docs/MARKETDATA_ARCHITECTURE.md†L64-L86】
- **Rendering hook:** The Qt renderer already exposes a `VolumeCandles` strategy (`CandleStrategy`) that expects each grid cell to carry OHLC data via `CellInstance::customData`.【F:libs/gui/render/strategies/CandleStrategy.hpp†L3-L35】【F:libs/gui/render/strategies/CandleStrategy.hpp†L62-L77】
- **Mode plumbing:** `UnifiedGridRenderer` constructs `CandleStrategy` and switches to it when `RenderMode::VolumeCandles` is selected, so no new render wiring is needed.【F:libs/gui/UnifiedGridRenderer.cpp†L421-L465】

## Data Model
Each candle should populate the following fields to satisfy `CandleStrategy` assumptions:
- `open`, `high`, `low`, `close` (double)
- `volume` (double) — optional but useful for sizing/weighting
- `tradeCount` (int)
- `startTime`/`endTime` (timestamp) — for alignment and debugging

The values are written into a candle buffer (ring or deque) keyed by product. When pushed to the GUI, map them onto the `customData` payload of the target `CellInstance` rows.

## Aggregation Flow
1. **Subscribe to trades:** On initialization, register a slot for `tradeReceived(Trade)` within `MarketDataCore`. The callback runs on the worker thread; forward into a strand or queue before mutating candle state to stay thread-safe with GUI consumers.
2. **Load per-product state:** Maintain a struct containing
   - active candle accumulator
   - candle history buffer (bounded)
   - configuration: `tradeTarget` (N) and `alignmentWindow` (optional timeframe)
3. **Update accumulator on trade:**
   - Initialize `open/high/low/close` from the first trade in a candle.
   - Update `high`/`low` via `std::max`/`std::min`.
   - Set `close` to the latest trade price; increment `tradeCount`; add size to `volume`.
4. **Boundary checks:**
   - **Trade-count mode:** When `tradeCount == tradeTarget`, finalize the candle and start a new accumulator seeded with the next trade.
   - **Time-seeded mode:** Track `startTime`. If `alignmentWindow` is set (e.g., 1s, 5s), finalize when `trade.timestamp >= startTime + alignmentWindow`. Carry over excess trades to the next candle.
   - Allow both to coexist by treating time alignment as an upper bound and trade-count as a hard cap; whichever triggers first closes the candle.
5. **Finalize & publish:**
   - Append the completed candle to the history buffer (drop the oldest if over capacity).
   - Emit a lightweight signal (e.g., `tickCandleReady(productId, Candle)`) on the GUI thread using Qt's `invokeMethod` with `Qt::QueuedConnection`, or expose a `getLatestCandles(productId)` accessor that the renderer polls.
6. **Surface to renderer:** During `IDataAccessor` reads for `VolumeCandles`, map each candle onto consecutive `CellInstance` entries. Populate `customData` with the OHLC fields expected by `CandleStrategy::extractOHLC`.

## Configuration Modes
- **Trade-count candles (primary):**
  - Config option: `tickCandle.tradeTarget` (default: 50 trades).
  - Boundary: close after exactly `tradeTarget` trades; use last trade timestamp as `endTime`.
  - Resets on reconnect or product change.
- **Time-aligned candles (secondary):**
  - Config option: `tickCandle.alignmentWindowMs` (default: disabled).
  - Boundary: floor the first trade timestamp to the next window boundary to seed `startTime`; close when crossing boundary even if `tradeTarget` not reached (set `tradeCount` to the actual number observed).
  - Useful for blending into time-based charts when a matching timeframe exists.

## Failure & Edge Handling
- **Sparse markets:** If a window closes with zero trades, skip emitting a candle rather than duplicating the previous close.
- **Burst protection:** Cap the history buffer size (e.g., 10k candles) to avoid unbounded memory growth during high-volume bursts.
- **Thread safety:** All cache mutations stay on the worker strand; GUI fetches copy or view the immutable history snapshots.
- **Backfill on reconnect:** Seed the first candle after reconnect using the first post-reconnect trade; do not stitch pre-disconnect data to avoid misleading gaps.

## Implementation Checklist
- [ ] Add a `TickCandleAggregator` in the market data layer with per-product state and configuration.
- [ ] Wire `MarketDataCore::tradeReceived` to feed the aggregator on the worker thread.
- [ ] Expose a thread-safe accessor (`getTickCandles(productId)`) or signal to the GUI layer.
- [ ] Extend the GUI data accessor used by `VolumeCandles` to read tick candles and populate `CellInstance::customData` with OHLC fields.
- [ ] Add configuration surface (CLI flag or settings panel) for `tradeTarget` and optional `alignmentWindowMs`.
- [ ] Document the behavior in user-facing settings if required.

This approach keeps the entire pipeline consistent with existing data streams and rendering contracts while enabling both trade-count and time-seeded tick candles.