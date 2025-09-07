### Phase 0 — Duplicates and Ownership (Focused Areas)

| file | function_signature | line_span | duplicate_of? | suggested_owner |
|---|---|---|---|---|
| libs/gui/UnifiedGridRenderer.cpp | QPointF UnifiedGridRenderer::worldToScreen(qint64 timestamp_ms, double price) const | 329-349 | CoordinateSystem::worldToScreen | CoordinateSystem.cpp |
| libs/gui/UnifiedGridRenderer.cpp | QPointF UnifiedGridRenderer::screenToWorld(double screenX, double screenY) const | 350-369 | CoordinateSystem::screenToWorld | CoordinateSystem.cpp |
| libs/gui/CoordinateSystem.cpp | QPointF CoordinateSystem::worldToScreen(int64_t timestamp_ms, double price, const Viewport& viewport) | 16-34 | — | CoordinateSystem.cpp |
| libs/gui/CoordinateSystem.cpp | QPointF CoordinateSystem::screenToWorld(const QPointF& screenPos, const Viewport& viewport) | 35-49 | — | CoordinateSystem.cpp |
| libs/gui/CoordinateSystem.cpp | QMatrix4x4 CoordinateSystem::calculateTransform(const Viewport& viewport) | 50-62 | GridViewState::calculateViewportTransform (overlap) | CoordinateSystem.cpp |
| libs/gui/render/GridViewState.cpp | QMatrix4x4 GridViewState::calculateViewportTransform(const QRectF& itemBounds) const | 61-97 | CoordinateSystem::calculateTransform (overlap) | CoordinateSystem.cpp |
| libs/gui/render/GridViewState.cpp | void GridViewState::setViewport(qint64 timeStart, qint64 timeEnd, double priceMin, double priceMax) | 24-53 | CoordinateSystem::validateViewport (semantically similar) | GridViewState.cpp |
| libs/gui/CoordinateSystem.cpp | bool CoordinateSystem::validateViewport(const Viewport& viewport) | 63-69 | GridViewState::setViewport (semantically similar) | GridViewState.cpp |
| libs/gui/render/DataProcessor.cpp | QRectF DataProcessor::timeSliceToScreenRect(const LiquidityTimeSlice& slice, double price) const | 438-479 | Uses/partially duplicates world→screen mapping | CoordinateSystem.cpp |
| libs/gui/render/DataProcessor.cpp | int64_t DataProcessor::suggestTimeframe(qint64 timeStart, qint64 timeEnd, int maxCells) const | 497-500 | LTSE::suggestTimeframe | LiquidityTimeSeriesEngine.cpp |
| libs/core/LiquidityTimeSeriesEngine.cpp | int64_t LiquidityTimeSeriesEngine::suggestTimeframe(int64_t viewStart_ms, int64_t viewEnd_ms, int maxSlices) const | 174-229 | DataProcessor::suggestTimeframe | LiquidityTimeSeriesEngine.cpp |
| libs/gui/UnifiedGridRenderer.cpp | void UnifiedGridRenderer::setPriceResolution(double resolution) | 277-288 | Duplicates DataProcessor::setPriceResolution | LiquidityTimeSeriesEngine.cpp (via DataProcessor) |
| libs/gui/render/DataProcessor.cpp | void DataProcessor::setPriceResolution(double resolution) | 480-486 | LTSE internal resolution | LiquidityTimeSeriesEngine.cpp |
| libs/core/LiquidityTimeSeriesEngine.cpp | double LiquidityTimeSeriesEngine::quantizePrice(double price) const | 535-536 | Related to resolution handling | LiquidityTimeSeriesEngine.cpp |
| libs/gui/render/DataProcessor.cpp | void DataProcessor::updateVisibleCells() | 272-351 | Overlaps with UGR::updateVisibleCells intent | DataProcessor.cpp |
| libs/gui/UnifiedGridRenderer.cpp | void UnifiedGridRenderer::updateVisibleCells() | 177-194 | Pass-through to DataProcessor | DataProcessor.cpp |
| libs/gui/UnifiedGridRenderer.cpp | void UnifiedGridRenderer::setGridResolution(int timeResMs, double priceRes) | 543-543 | Pass-through to setPriceResolution | DataProcessor.cpp |
| libs/gui/UnifiedGridRenderer.cpp | void UnifiedGridRenderer::setViewport(qint64 timeStart, qint64 timeEnd, double priceMin, double priceMax) | 542-542 | Pass-through to onViewChanged | GridViewState.cpp |
| libs/gui/UnifiedGridRenderer.cpp | void UnifiedGridRenderer::addTrade(const Trade& trade) | 541-541 | Pass-through to DataProcessor::onTradeReceived | DataProcessor.cpp |

Notes:
- Overlaps indicate duplicated responsibility rather than byte-identical code.
- Suggested owner centralizes the responsibility to a single module per Core Principles.


