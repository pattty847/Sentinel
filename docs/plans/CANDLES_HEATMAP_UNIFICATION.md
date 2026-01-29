# Candlestick + Heatmap Integration Plan

Last updated: 2026-01-29

## Goal
Layer OHLCV candlesticks on top of (or beside) the GPU heatmap in a unified chart widget. Both share the same viewport (price/time axes, zoom/pan). TradingView-style: candles compress/expand with zoom.

## Scope decisions
- Rename pass (HeatmapDock → ChartDock, etc.) is deferred.
- Candle TF defaults to heatmap TF but can be independent (lowest = 1s).
- Server’s lowest cached TF is 1s.

---

## Architecture Decision: QML Overlay (not integrated nodes)
Keep `CandlestickBatched` as a separate `QQuickItem` overlaid at `z:2` in `DepthChartView.qml`.
Viewport sync via QML property bindings from `UnifiedGridRenderer`.

**Why not integrate into UnifiedGridRenderer:**
- UGR is already large and dense (texture + glyph logic).
- Candles are simple vertex-color quads; different rendering strategy.
- Two `updatePaintNode()` calls are fine; candles are low-geometry.
- Mouse handling is easier with overlayMode (no click capture).

---

## Phase 1: Viewport-Driven Candle Rendering (MVP)
Render candles aligned to the same viewport as the heatmap (demo data OK).

### 1.1 Add viewport properties to CandlestickBatched
**File:** `libs/gui/render/CandlestickBatched.hpp`

Add Q_PROPERTY declarations:
- `visibleTimeStart` (qint64, WRITE + NOTIFY viewportPropsChanged)
- `visibleTimeEnd` (qint64, WRITE + NOTIFY viewportPropsChanged)
- `viewMinPrice` (double, WRITE + NOTIFY viewportPropsChanged)
- `viewMaxPrice` (double, WRITE + NOTIFY viewportPropsChanged)
- `panVisualOffset` (QPointF, WRITE + NOTIFY panVisualOffsetChanged)
- `candleTimeframeMs` (int, WRITE + NOTIFY candleTimeframeMsChanged)
- `overlayMode` (bool, WRITE + NOTIFY overlayModeChanged)

Add corresponding private members.

**overlayMode:**
- If true, call `setAcceptedMouseButtons(Qt::NoButton)`.
- Explicitly enable hover events with `setAcceptHoverEvents(true)` so tooltips still work.

### 1.2 Refactor `updatePaintNode()` for viewport coordinates
**File:** `libs/gui/render/CandlestickBatched.cpp`

Replace local coordinate math with viewport-driven positioning:

- X from timestamp:
  `timeToX(ts) = (ts - timeStart) / visibleDuration * width`
- Y from price:
  `priceToY(p) = (1 - (p - minPrice) / priceRange) * height`
- Apply `panVisualOffset` using same math as UGR (time/price pixels → units).
- Candle width from zoom:
  `width / (visibleDuration / candleTimeframeMs) * 0.7`, clamp `[1, 40]`.
- Cull candles outside `[timeStart - timeframe, timeEnd + timeframe]`.
- Use binary search on candle timestamps for O(log n) cull range.

Keep existing `addQuad()` + `CandleRootNode` structure.

### 1.3 Update CandleChartView.qml to forward viewport
**File:** `libs/gui/qml/CandleChartView.qml`

Add property aliases forwarding viewport props to `CandlestickBatched`:
- `visibleTimeStart`, `visibleTimeEnd`, `viewMinPrice`, `viewMaxPrice`, `panVisualOffset`, `candleTimeframeMs`.

Set `overlayMode: true` on the inner `CandlestickBatched`.

### 1.4 Wire viewport bindings in DepthChartView.qml
**File:** `libs/gui/qml/DepthChartView.qml`

Update overlay block:

```
CandleChartView {
    id: candleOverlay
    anchors.fill: unifiedGridRenderer
    visible: root.showCandles
    z: 2
    visibleTimeStart: unifiedGridRenderer.visibleTimeStart
    visibleTimeEnd: unifiedGridRenderer.visibleTimeEnd
    viewMinPrice: unifiedGridRenderer.minPrice
    viewMaxPrice: unifiedGridRenderer.maxPrice
    panVisualOffset: unifiedGridRenderer.panVisualOffset
    candleTimeframeMs: root.candleTimeframeMs
}
```

Add `candleTimeframeMs` property to `DepthChartView.qml` root, defaulting to `unifiedGridRenderer.timeframeMs` (locked to heatmap TF initially).

### 1.5 Test with demo data
Update demo generator so timestamps align with current heatmap time range:
- Accept a `startTimestampMs` OR
- Generate from `Date.now()` backwards.

**Verify:**
- Candles align with grid
- Zoom in/out changes candle width
- Pan moves both layers
- Heatmap still receives mouse input (overlay mode)

---

## Phase 2: Real Data Connection

### 2.1 CandleDataProvider (bridge)
**New files:** `libs/gui/render/CandleDataProvider.hpp/.cpp`

A lightweight GUI-side bridge that:
- Owns a `TimeframeAggregator` (core C++ only) or a minimal GUI-side equivalent
- Lives on GUI thread
- Receives trades via queued slot
- Maintains `std::vector<CandleData>` sorted by timestamp
- Emits `candlesUpdated()` on changes

**Note:** Core stays pure C++ (no Qt). If `TimeframeAggregator` is Qt-dependent, implement a minimal GUI-side aggregator instead.

### 2.2 Wire into pipeline
Wire `IGridDataSource::tradeReceived` → `CandleDataProvider::onTradeReceived` (QueuedConnection).
Expose provider to QML or set directly on `CandlestickBatched`.

### 2.3 Direct candle data setter
Add to `CandlestickBatched`:
- `setCandlesDirect(const std::vector<CandleData>& candles)` or
- `setCandleDataProvider(CandleDataProvider* provider)`

Connect `candlesUpdated` to refresh.

### 2.4 (Later) Server OHLCV history
Extend WebSocket protocol with `ohlcv_bar` / `ohlcv_history` messages.
Server sends historical bars on subscribe + live updates.

---

## Phase 3: Polish
- Volume bars (third geometry node, bottom 15% of chart).
- Crosshair (separate QQuickItem at z:3, shared between layers).
- Candle appearance: doji handling, min body height, opacity control.
- Candle TF selector on toolbar (independent from heatmap TF).

---

## Risks & Mitigations
- **Viewport sync jitter:** if QML bindings jitter, add direct C++ signal wiring.
- **Perf with 10k candles:** cull to visible range; binary search range.
- **Mouse blocking:** overlayMode disables mouse buttons, keep hover enabled.
- **Data duplication:** consolidate `CandleData`/`OHLCVBar` later if needed.
- **No history in MVP:** acceptable for Phase 1.

---

## Verification
1) Phase 1: demo data aligned, zoom/pan synced, no input conflict.
2) Phase 2: live candles from trade stream, new bar at TF boundary.
3) Phase 3: volume bars + crosshair over both layers.
