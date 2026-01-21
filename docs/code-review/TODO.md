# TODO - Next Session

## Stream + Heatmap Pipeline
- Add QRhi (non-OpenGL) upload path for heatmap slices in `HeatmapIntensityNode` so Vulkan/Metal/D3D backends work.
- Client liquidity threshold only affects new columns. Add a raw intensity ring (pre-threshold) and reapply threshold across history, or request a server resend on threshold changes.

## Timeframe Updates
- Add periodic and/or threshold-based heatmap slice refresh for long timeframes (e.g., update every 1s or on significant row changes) so 1h/1d slices stay live.

## Misc
- Revisit stream protocol schema to include heatmap/grid metadata and order book range info for client initialization.
- GridViewState + CoordinateSystem reviewed; no changes needed.
- Decide heatmap tick size strategy (band % + fixed height vs manual presets) and liquidity threshold policy (absolute vs adaptive/EMA); redesign control panel to match final choices.
