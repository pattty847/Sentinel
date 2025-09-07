### Phase 0 — Delete Candidates (UGR pass-throughs)

- `UnifiedGridRenderer::updateVisibleCells()` (177-194): pass-through to `DataProcessor::updateVisibleCells()`; remove and call DP directly when needed.
- `UnifiedGridRenderer::setGridResolution(int timeResMs, double priceRes)` (543): thin wrapper to `setPriceResolution`; de-duplicate.
- `UnifiedGridRenderer::setViewport(qint64, qint64, double, double)` (542): proxy to `onViewChanged`; prefer `GridViewState::setViewport` or a unified API.
- `UnifiedGridRenderer::addTrade(const Trade&)` (541): trivial forward to `onTradeReceived`; callers can target `DataProcessor::onTradeReceived` or existing connection.
- `UnifiedGridRenderer::worldToScreen` (329-349) and `screenToWorld` (350-369): re-implementations; replace with calls to `CoordinateSystem`.

Notes:
- Removal contingent on ensuring external callers have equivalent entry points (e.g., via `MainWindowGPU` wiring).


