# TODO - Next Session

## Stream + Heatmap Pipeline
- Add QRhi (non-OpenGL) upload path for heatmap slices in `HeatmapIntensityNode` so Vulkan/Metal/D3D backends work.
- Make `RemoteGridDataSource` initialize LiveOrderBook from server-authoritative min/max/tick (protocol fields or inference), instead of hardcoded values.

## Timeframe Updates
- Add periodic and/or threshold-based heatmap slice refresh for long timeframes (e.g., update every 1s or on significant row changes) so 1h/1d slices stay live.

## Misc
- Revisit stream protocol schema to include heatmap/grid metadata and order book range info for client initialization.
- GridViewState + CoordinateSystem reviewed; no changes needed.
