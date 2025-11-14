# Data Pipeline Flow (Current Implementation)

This document traces every transformation from raw order book traffic to the Qt Scene Graph node that eventually uploads GPU geometry.

## 1. Live Order Book Ingestion (DataProcessor thread)

### 1.1 Dense ingestion path
1. `DataProcessor::onLiveOrderBookUpdated` (`libs/gui/render/DataProcessor.cpp:262-383`) receives `BookDelta` bursts plus a symbol key.
2. The handler asks `DataCache` for the in-memory `LiveOrderBook` and calls `captureDenseNonZero` to build a bounded view of non-zero bid/ask levels (`libs/core/marketdata/model/TradeData.h:194-206`). This returns spans of `(index, quantity)` pairs plus metadata (`minPrice`, `tickSize`, timestamp).
3. `LiquidityTimeSeriesEngine::addDenseSnapshot` (`libs/core/LiquidityTimeSeriesEngine.cpp:58-78`) converts every span entry into `OrderBookSnapshot` map entries. Prices are reconstructed as `minPrice + idx * tickSize`, quantized, and aggregated.
4. Every dense snapshot is appended to `m_snapshots` and run through `updateAllTimeframes`, populating all configured time slices. Cleanup prunes old history each call.

### 1.2 Sparse fallback path
If dense ingestion produces no levels, the same handler:
1. Computes mid-price and view band using `BandMode` (`DataProcessor.cpp:303-336`).
2. Walks the dense arrays in-band, populating a temporary sparse `OrderBook` via sequential loops (`lines 337-361`).
3. Ensures at least top-of-book levels exist, then calls the sparse `LiquidityTimeSeriesEngine::addOrderBookSnapshot` overload (`lines 372-382`).

### 1.3 Timer-driven snapshots
Independent of live updates, `captureOrderBookSnapshot` (`DataProcessor.cpp:149-198`) runs every 100 ms to enforce uniform time buckets. It copies the latest sparse `OrderBook`, stamps `timestamp` to the bucket start, and pushes into the engine for each missed interval. This ensures there is always an entry every 100 ms even if real traffic stalls—see the recorded trace in `liquidity_timeseries_log.txt`, where `NEW SLICE 100ms` entries step monotonically in 100 ms increments.

**Threading:** All ingestion work above runs on the dedicated `DataProcessor` `QThread`. The Qt timer and live-order-book slot execute there, guarding shared state via `m_dataMutex`.

## 2. LiquidityTimeSeriesEngine Aggregation

1. Each snapshot is quantized and forwarded to `updateAllTimeframes` (implementation in `LiquidityTimeSeriesEngine.cpp`, lines 187+). The engine maintains per-timeframe deques `m_timeSlices` and mutable `m_currentSlices`.
2. For each timeframe, `addSnapshotToSlice` accumulates `PriceLevelMetrics` for every tick into contiguous vectors (`LiquidityTimeSeriesEngine.h:95-120`). Metrics track running sums, peaks, min/max timestamps, and `snapshotCount`.
3. When a slice covers its full duration, `finalizeLiquiditySlice` seals it and moves it into the deque, keeping at most `m_maxHistorySlices` entries per timeframe.
4. `suggestTimeframe` (`LiquidityTimeSeriesEngine.cpp:194-207`) estimates the optimal aggregation interval for a given viewport to cap the total number of slices (<2000 for renderer requests). `DataProcessor::updateVisibleCells` calls this suggestion whenever no manual override exists.

**Threading:** The engine lives inside the `DataProcessor` thread. All reads and writes occur synchronously there, so returned slice pointers are stable until the worker releases them.

## 3. Viewport-Aligned Slice Selection

1. `DataProcessor::updateVisibleCells` (`libs/gui/render/DataProcessor.cpp:402-548`) is invoked after every ingestion action and also from the GUI thread when dirty flags demand it.
2. The method gates rebuilds by tracking `GridViewState::getViewportVersion` and clears caches when the viewport changes (`lines 410-418`).
3. It picks an active timeframe (auto-suggest or manual), then calls `LiquidityTimeSeriesEngine::getVisibleSlices` with `timeStart/timeEnd` from `GridViewState` (`lines 442-449`).
4. Visible slices are processed either as a full rebuild (clear `m_processedTimeRanges`, iterate every slice) or as append-only (skip any slice whose `(start,end)` tuple already exists in the set) per lines 486-517. This prevents duplication because the LTSE reuses slice objects.

**Data copied:** Slices themselves are not copied—`createCellsFromLiquiditySlice` receives `const LiquidityTimeSlice&` references directly, so this stage only iterates data inside the engine.

## 4. Cell Creation (DataProcessor thread)

1. `createCellsFromLiquiditySlice` (`DataProcessor.cpp:550-585`) loops over `slice.bidMetrics` and `slice.askMetrics`. For every metrics entry whose `snapshotCount > 0`, it reconstructs a price using `slice.minTick`/`tickSize` and enforces viewport price bounds before calling `createLiquidityCell`.
2. `createLiquidityCell` (`lines 587-616`) performs final world-space culling (time/price against viewport), then writes a `CellInstance` with:
   * Time bounds from the slice,
   * Price min/max = `price ± tickSize/2`,
   * `liquidity` equal to whichever display metric `slice.getDisplayValue` returned (display mode currently hardcoded to 0/`Average`),
   * Color derived from side and an alpha computed from normalized liquidity (`liquidity / 1000.0`, clamped).
3. Each `CellInstance` is appended to `m_visibleCells` (contiguous AoS). No reallocation happens during append-mode updates if capacity already fits ~30 k entries.
4. Once all slices are handled, `DataProcessor` copies the vector into a snapshot pointer (`std::make_shared<std::vector<CellInstance>>(m_visibleCells)` in lines 531-536) for the renderer to consume atomically.

## 5. Renderer Consumption (GUI/Render threads)

1. On the GUI thread, `UnifiedGridRenderer::updateVisibleCells` (`libs/gui/UnifiedGridRenderer.cpp:146-158`) swaps in the latest published pointer by value-copying vector contents into `m_visibleCells`.
2. `updatePaintNode` (`libs/gui/UnifiedGridRenderer.cpp:473-577`) runs on the Qt render thread. Dirty flags control whether it must fetch new cell data (`updateVisibleCells`) or simply re-run material/transform updates.
3. When geometry is dirty or an append is pending, a new `GridSliceBatch` is created with the copied cells, the recent trade list, and the viewport snapshot (lines 502-528).

**Data copies at this boundary:** The `std::vector<CellInstance>` is copied twice—once into the shared snapshot, once from snapshot into `m_visibleCells` on the GUI side, then again when `GridSliceBatch` is constructed by value.

## 6. Geometry Generation (Qt render thread)

1. `GridSceneNode::updateLayeredContent` (`libs/gui/render/GridSceneNode.cpp:48-100`) delegates to `HeatmapStrategy` when the heatmap layer is enabled. Each call destroys prior child `QSGNode`s and replaces them with freshly built geometry.
2. `HeatmapStrategy::buildNode` (`libs/gui/render/strategies/HeatmapStrategy.cpp:25-150`) executes the hot path:
   * Calculates how many cells survive `minVolumeFilter` (lines 42-51).
   * Chunks work into ≤60 000 vertices per node (lines 55-70) by streaming pointers to `CellInstance` records.
   * For every chunk, allocates `QSGGeometry` with the default colored `Point2D` layout (lines 87-101).
   * Iterates all cells: compute per-cell intensity/color (`lines 103-113`), convert world coordinates to screen coordinates via `CoordinateSystem::worldToScreen` twice per cell (`lines 115-121`), and emit six vertices (two triangles) with identical RGBA per vertex (`lines 123-131`).
   * Each node is marked dirty so Qt copies the vertex buffer into the scene graph (`line 134`). Qt’s renderer uploads the buffer to GPU memory before drawing.
3. The method logs aggregate stats every 30 frames via `HEATMAP CHUNKS: cells=X verts=Y chunks=Z` (`lines 142-147`).

## 7. GPU Submission

* Once `GridSceneNode` finishes building its child nodes, `UnifiedGridRenderer::updatePaintNode` reports `UGR paint: total=… cache=… content=… cells=…` (`libs/gui/UnifiedGridRenderer.cpp:559-563`). `cacheUs` covers `updateVisibleCells` + batch construction; `contentUs` covers `GridSceneNode::updateLayeredContent` through all strategy calls.
* Qt Quick’s scene graph renderer handles the actual OpenGL/RHI upload the next time the render loop runs, using the vertex data allocated by `QSGGeometry`.

## Thread Summary

| Stage | Thread | Key structures |
| --- | --- | --- |
| Live order book updates | DataProcessor worker (`QThread`) | `LiveOrderBook`, `OrderBook`, `OrderBookSnapshot` |
| Liquidity aggregation | DataProcessor worker | `LiquidityTimeSlice`, `m_timeSlices` |
| Cell creation | DataProcessor worker | `std::vector<CellInstance> m_visibleCells` |
| Snapshot publication | DataProcessor worker → GUI | `std::shared_ptr<const std::vector<CellInstance>>` |
| Renderer fetch | GUI thread | `UnifiedGridRenderer::m_visibleCells` |
| Geometry build | Qt render thread | `GridSliceBatch`, `QSGGeometry` |

Each hand-off either copies the AoS cell buffer (`std::vector<CellInstance>`) or reuses pointers while holding locks (`DataProcessor::m_snapshotMutex` around the published snapshot).
