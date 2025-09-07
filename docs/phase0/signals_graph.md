### Phase 0 — Signals/Slots Wiring Map

Mermaid graph of key interactions (zoom/scroll, viewport changes, timeframe/price resolution, render invalidation, resample triggers):

```mermaid
graph TD
  MktCore["MarketDataCore (signals)"] -->|tradeReceived(Trade)| UGR.onTradeReceived
  MktCore -->|liveOrderBookUpdated(QString)| DP.onLiveOrderBookUpdated

  UGR["UnifiedGridRenderer"] -- connect --> DP.dataUpdated
  DP["DataProcessor"] -->|dataUpdated()| UGR.update (throttled -> geometryDirty)
  DP -->|viewportInitialized()| UGR.viewportChanged

  UGR -- connect --> GVS.viewportChanged
  UGR -- connect --> GVS.panVisualOffsetChanged
  UGR -- connect --> GVS.autoScrollEnabledChanged

  GVS["GridViewState"] -->|viewportChanged()| UGR.onViewportChanged
  GVS -->|panVisualOffsetChanged()| UGR.panVisualOffsetChanged
  GVS -->|autoScrollEnabledChanged()| UGR.autoScrollEnabledChanged

  UGR -- wheelEvent/pan --> GVS.handleZoom/handlePan*
  GVS -- emits --> GVS.viewportChanged

  UGR.setTimeframe --> UGR.timeframeChanged
  UGR.setPriceResolution --> UGR.priceResolutionChanged
  UGR.setRenderMode --> UGR.renderModeChanged
  UGR.setShowVolumeProfile --> UGR.showVolumeProfileChanged
  UGR.enableAutoScroll --> UGR.autoScrollEnabledChanged

  %% Resampling / LOD
  UGR.updateVisibleCells --> DP.updateVisibleCells
  DP.suggestTimeframe --> LTSE.suggestTimeframe
  LTSE["LiquidityTimeSeriesEngine"] -->|suggestTimeframe()| DP

  %% Render invalidation
  UGR.geometryChanged --> UGR.updatePaintNode
  DP.dataUpdated --> UGR.updatePaintNode
```

Duplicate signal sources that may flip LOD without user input:
- DataProcessor::dataUpdated multiple emits (on trade, on order book, on clear, on resolution/timeframe change) can cascade suggestTimeframe via updateVisibleCells if wired; verify throttling in UGR (250ms) prevents LOD thrash.
- UnifiedGridRenderer::timeframeChanged is emitted both from setTimeframe() at 298-309 and during auto-suggestion paths (139-143, 304-308); ensure only one source drives actual LTSE/DataProcessor resampling.


