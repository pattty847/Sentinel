# Monolith Cleanup Report (GPU-Only Heatmap)

## Outcome
- Legacy CPU/QSG render path fully removed.
- GPU-only heatmap path is now the only supported render route.
- LOC reduction: ~17k -> ~13k (approx. 5k lines removed).
- Build runs; server + client streaming verified.

## What Was Removed
### Strategy/Scene Graph Stack
- Strategy interface + implementations (Heatmap/TradeFlow/TradeBubble/Candle).
- GridSceneNode (layered strategy scene graph root).
- IDataAccessor bridge for strategies.
- RenderDiagnostics and unused render config/types.
- CellInstance grid types and CPU cell cache plumbing.

### GUI / QML Legacy Toggles
- Render mode selection for strategy-based paths.
- Layer toggles (heatmap/bubbles/flow) and bubble sliders.
- RenderModeSelector QML control.

### DataProcessor Legacy Ingestion
- LiquidityTimeSeriesEngine-based ingestion and cell snapshot pipeline.
- updateVisibleCells and associated cache/viewport-version logic.
- Order book sampling and dense ingestion flows.

## What Remains (GPU Heatmap Path)
- HeatmapIntensityNode + shader path.
- UnifiedGridRenderer GPU branch only.
- DataProcessor heatmap slice ingestion and column forwarding.
- GridViewState + CoordinateSystem.
- Axis models and QML chart scaffolding.
- RemoteGridDataSource + heatmapSliceReceived pipeline.
- LiquidityTimeSeriesEngine in core (left intact for now).

## Key Files Affected
### Deleted
- libs/gui/render/IRenderStrategy.*
- libs/gui/render/strategies/*
- libs/gui/render/GridSceneNode.*
- libs/gui/render/IDataAccessor.hpp
- libs/gui/render/GridTypes.hpp
- libs/gui/render/RenderTypes.hpp
- libs/gui/render/RenderConfig.hpp
- libs/gui/render/RenderDiagnostics.*
- libs/gui/qml/controls/RenderModeSelector.qml

### Modified
- libs/gui/UnifiedGridRenderer.*
- libs/gui/render/DataProcessor.*
- libs/gui/datasources/IGridDataSource.hpp
- libs/gui/datasources/RemoteGridDataSource.*
- libs/gui/MainWindowGpu.cpp
- libs/gui/qml/DepthChartView.qml
- libs/gui/CMakeLists.txt

## Verification
- Build succeeded after cleanup.
- Server + client running; heatmap columns streaming.
- No QML property errors.

## Notes
- This change completes the transition away from the legacy monolith renderer.
- GPU-only heatmap is now the singular render path.
