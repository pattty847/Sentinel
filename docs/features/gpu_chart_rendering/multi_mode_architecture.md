# Multi-Mode Chart Architecture

This document outlines the new chart mode system and candle batching pipeline.
The GPU rendering layer now receives batched candle updates from `GPUDataAdapter`
every 16ms. `ChartModeController` exposes a simple API for switching between
chart modes which higher level components can use to toggle visibility of the
trade scatter, candle view and order book heatmap.
