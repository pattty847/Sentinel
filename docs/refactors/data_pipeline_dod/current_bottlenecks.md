# Instrumented Bottlenecks (Current State)

## Frame Budget Breakdown

`UnifiedGridRenderer::updatePaintNode` emits a structured log every frame (`libs/gui/UnifiedGridRenderer.cpp:559-563`):

```
UGR paint: total=<totalUs>microseconds cache=<cacheUs>microseconds content=<contentUs>microseconds cells=<cellsCount>
```

* **`cacheUs`:** Time spent inside `updateVisibleCells()` and `GridSliceBatch` construction (lines 499-505). This covers the main thread’s copy of the `std::vector<CellInstance>`, so it is the proxy for `DataProcessor::updateVisibleCells` latency plus the GUI copy overhead.
* **`contentUs`:** Duration of `GridSceneNode::updateLayeredContent(...)` (lines 506-534), which in turn runs `HeatmapStrategy::buildNode`. This number therefore measures vertex generation and Qt scene-graph churn.
* **`cells`:** Snapshot of `m_visibleCells.size()` after cache refresh (lines 519/535). With typical 20–30 k cells, `contentUs` scales roughly linearly because each cell produces six vertices.
* **Frame budget context:** 60 FPS implies a 16.67 ms (= 16 670 µs) total allowance. `totalUs` already sums `cacheUs + contentUs + transform/misc`, so keeping the logged total under this threshold is the gating requirement called out in the spec.

## Dirty-Flag Triggers

Several logs pinpoint why a given frame performs expensive work:

* `FULL GEOMETRY REBUILD (mode/LOD/timeframe changed)` at `libs/gui/UnifiedGridRenderer.cpp:497` fires whenever `m_geometryDirty` flips or the QSG node is created. This forces a complete `updateVisibleCells` call plus full heatmap rebuild.
* `APPEND PENDING (rebuild from snapshot)` (`line 521`) indicates that `m_appendPending` was set by user zoom/pan gestures (`UnifiedGridRenderer.cpp:321-322, 391, 646`) and that the renderer is now fetching the worker’s most recent snapshot. Even though only new slices should be appended, the renderer still rebuilds heatmap geometry from scratch, so the ensuing `contentUs` will reflect the full cell count.
* `MATERIAL UPDATE (intensity/palette)` (`line 538`) reruns strategies without re-fetching cells, so only shader-constant-load work occurs.
* `TRANSFORM UPDATE (pan/zoom)` (`line 549`) toggles the scene-node matrix without touching geometry, so this log usually corresponds to cheap frames.

Understanding which of these messages triggered a slow `UGR paint` log gives immediate insight into whether the bottleneck is data-fetch (`cache`) or geometry (`content`).

## Heatmap Strategy Logs

`HeatmapStrategy::buildNode` contributes two relevant logs:

* `HEATMAP EXIT: Returning nullptr - batch is empty` (`libs/gui/render/strategies/HeatmapStrategy.cpp:32-35`) explains why `contentUs` can drop to zero while `cacheUs` remains non-zero—no cells survived `minVolumeFilter`.
* `HEATMAP CHUNKS: cells=X verts=Y chunks=Z` (`lines 142-147`) fires every 30 frames with the number of surviving cells, the emitted vertex count (`6 * cells`), and how many QSG child nodes were produced (each ≤60 000 vertices). Comparing `verts` to the `cells` count in the UGR log confirms that `contentUs` grows with vertex count, and that GPUs are fed ~150 k vertices whenever ~25 k cells survive filters.

## DataProcessor Logging

`libs/gui/render/DataProcessor.cpp` emits several breadcrumbs for the worker-side hot paths:

* `LTSE QUERY: timeframe=<ms>, window=[start-end]` and `LTSE RESULT: Found <n> slices` (`lines 446-450`) document how many `LiquidityTimeSlice` objects are sampled for the current viewport. With a 30 s × 100 ms view, the log shows roughly 300 slices, aligning with the iteration counts described in `hot_paths.md`.
* `SLICE PROCESSING: Processed <processedSlices>/<visibleSlices> (rebuild|append)` plus `DATA PROCESSOR COVERAGE Slices:<visible> TotalCells:<m_visibleCells.size()> ActiveTimeframe:<ms>` (`lines 524-529`) tie slice counts to cell counts. When `TotalCells` balloons toward 30 k, the renderer’s `cells` readout matches, confirming the AoS buffer size handed to the GPU path.
* `APPEND PENDING` is set by GUI interactions, but the worker publishes snapshots only when `changed` evaluates true (`lines 522-538), so the presence (or absence) of the signal `dataUpdated()` marks whether the renderer should expect a costly append.

Although `updateVisibleCells` itself is not directly timed, combining `SLICE PROCESSING` counts with the `cacheUs` metric from the render thread produces an end-to-end view of how much wall-clock time slice expansion + copying consumes per frame.

## Liquidity Engine Diagnostics

* Every time the LTSE finalizes a slice, `liquidity_timeseries_log.txt` records entries such as `NEW SLICE 100ms: [1762201842500-1762201842600]`. These lines originate from `LiquidityTimeSeriesEngine` instrumentation (enabled elsewhere) and confirm that snapshots are produced exactly every 100 ms (base timeframe) with additional entries for 250 ms, 500 ms, 1 s, etc. This steady cadence explains why `visibleSlices` grows whenever the viewport widens and why `m_processedTimeRanges` pruning is necessary.
* `DataProcessor::onLiveOrderBookUpdated` logs `DataProcessor processing dense LiveOrderBook - bids:<N> asks:<N>` (`line 294`) when falling back to sparse conversion. Seeing this log concurrently with `HEATMAP EXIT` indicates the ingest path is starved of liquidity despite deltas arriving, a signal that banding or filtering may be too aggressive (documented here for tracing, not to prescribe fixes).

## Sample Timing Table

While actual numbers depend on runtime measurements, the logging described above supplies the structure for the requested breakdown:

| Stage | Source | Measurement |
| --- | --- | --- |
| `DataProcessor::updateVisibleCells` + GUI copy | `UGR paint cache=<cacheUs>` (`libs/gui/UnifiedGridRenderer.cpp:559-562`) | Microseconds spent refreshing cached cells |
| Geometry rebuild (`HeatmapStrategy::buildNode` via `GridSceneNode::updateLayeredContent`) | `UGR paint content=<contentUs>` (`same file, line 562`) | Microseconds spent generating and uploading vertex buffers |
| Heatmap chunking granularity | `HEATMAP CHUNKS` (`libs/gui/render/strategies/HeatmapStrategy.cpp:142-147`) | Cells kept, vertices emitted, chunk count |
| Snapshot append trigger | `APPEND PENDING` (`libs/gui/UnifiedGridRenderer.cpp:521`) + `SLICE PROCESSING` (`libs/gui/render/DataProcessor.cpp:524-529`) | Indicates whether a viewport interaction forced a rebuild vs append |
| Slice availability | `LTSE RESULT: Found <n> slices` (`libs/gui/render/DataProcessor.cpp:446-450`) | Shows how many slices feed `createCellsFromLiquiditySlice` |

Mapping these logs per frame reveals where CPU budget is spent without speculating on optimizations: heavy `cacheUs` + large slice counts points at ingestion-side work, while elevated `contentUs` + `HEATMAP CHUNKS` vertices highlights GPU upload pressure.
