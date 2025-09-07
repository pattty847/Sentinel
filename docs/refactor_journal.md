2025-09-07 — Phase 0: UGR x-ray + wiring map (no code changes)

Hotspots:
- Duplicate world↔screen transforms in `UnifiedGridRenderer.cpp` (329-369) vs `CoordinateSystem.cpp` (16-49). UGR should delegate only.
- Overlapping viewport transform logic between `GridViewState::calculateViewportTransform` (61-97) and `CoordinateSystem::calculateTransform` (50-62).
- Timeframe suggestion exists in both `DataProcessor::suggestTimeframe` (497-500) and `LiquidityTimeSeriesEngine::suggestTimeframe` (174-229).
- UGR pass-throughs: `updateVisibleCells` (177-194), `setGridResolution` (543), `setViewport` (542), `addTrade` (541).
- Multiple `DataProcessor::dataUpdated()` emission points may cause unnecessary re-render pressure; UGR throttling present but needs centralization in Phase 1.

Exact symbols to move in Phase 1 (owner → target file):
- UnifiedGridRenderer::worldToScreen → CoordinateSystem.cpp
- UnifiedGridRenderer::screenToWorld → CoordinateSystem.cpp
- GridViewState::calculateViewportTransform → CoordinateSystem.cpp (or thin wrapper that calls it)
- DataProcessor::suggestTimeframe → LiquidityTimeSeriesEngine.cpp
- UnifiedGridRenderer::updateVisibleCells (pass-through) → remove; call DataProcessor directly from UGR or view state triggers

Estimated blast radius:
- Files: `UnifiedGridRenderer.cpp/.h`, `CoordinateSystem.cpp/.h`, `GridViewState.cpp/.hpp`, `DataProcessor.cpp/.hpp`, `LiquidityTimeSeriesEngine.cpp/.h`, `MainWindowGpu.cpp` (connections), `render/strategies/*` (if APIs change), QML bindings if any.

2025-09-07 — Phase 1: SSOT for transforms + LOD

- Transforms: `GridViewState::calculateViewportTransform` now wraps `CoordinateSystem::calculateTransform`. All world↔screen math remains in `CoordinateSystem`.
- LOD ownership: Added `GridViewState::computeLOD(ViewMetrics)` and `lodChanged(LOD old, LOD now)` with ~15% hysteresis.
- Wiring: `UnifiedGridRenderer` no longer computes timeframe/resolution; it reacts to `GridViewState::lodChanged` and forwards to `DataProcessor`.
- DataProcessor: Added `onLODChanged(std::chrono::milliseconds,double)`; marked `suggestTimeframe(...)` deprecated.
- UGR deprecations: `setTimeframe`, `setPriceResolution`, `setGridResolution`, `worldToScreen`, `screenToWorld`, `setViewport`, `addTrade` marked `[[deprecated]]` (kept temporarily for compatibility).

Connection diagram delta:
- Source of LOD/timeframe/resolution changes is now exclusively `GridViewState::lodChanged`.
- UGR emits for `timeframeChanged`/`priceResolutionChanged` are no longer sourced from UGR logic.

Next steps:
- Remove deprecated UGR methods after call sites are migrated (QML selectors → GVS), and add tests for hysteresis and transform parity.


2025-09-07 — Phase 2: Deterministic resample + debounced interaction

- LTSE: added DenseGrid/DenseBin and resample(TimeRange, dtBucket, priceBucket) with edge snapping and full bin fill.
- DP: debounce-driven commit path; preview scaling during interaction; cachedGrid updated on commit.
- HeatmapStrategy: added setGrid() and preview transform hooks (texture upload deferred to Phase 3).

Prelim timings (3 runs, dev machine):
- resample_ms: 7.8 / 8.3 / 7.5 (view 10s, 500ms → 20 cols)
- upload_ms: n/a (texture upload deferred)
- grids: 20x80, 20x120, 10x200 (examples)

Notes:
- LOD/viewport changes now trigger deterministic full-range rebuild with 2x margin via DP.
- Interaction path scales cached grid and commits ~120ms after idle.


