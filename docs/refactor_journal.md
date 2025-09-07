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


