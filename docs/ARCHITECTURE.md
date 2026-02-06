# Sentinel Architecture

## Overview

Sentinel is a GPU-accelerated trading terminal built on a mandatory client-server architecture. The server runs as a headless daemon for 24/7 data ingestion; the client is a lightweight Qt6/QML visualizer.

## Core Principles

- **Client-Server Split**: Ingestion and visualization are decoupled. Server handles data, client handles rendering.
- **GPU-Resident Rendering**: Heatmaps and labels are rendered directly on GPU via streamed intensity textures and an MSDF atlas.
- **Hot Path Efficiency**: Pre-aggregated buffers minimize overhead between server and GPU; transport is JSON + base64 today.
- **Deterministic Threading**: Network I/O (Boost.Beast), aggregation, and rendering run on dedicated threads.

## Directory Layout

```
apps/
  sentinel_gui/       # Visualization client (remote-only)
  sentinel-server/    # Headless data daemon

libs/
  core/
    marketdata/       # Exchange transports + MarketDataCoreEngine (see MARKETDATA.md)
    protocol/         # Client-server WebSocket protocol
    servermodel/      # Server state, aggregation, persistence
    model/            # Shared DTOs
  gui/
    UnifiedGridRenderer   # GPU pipeline orchestration
    datasources/          # RemoteGridDataSource
    render/               # GPU nodes + MSDF atlas (HeatmapIntensityNode, MsdfGlyphNode)
    qml/                  # UI components
```

Only `QtCore` is permitted in `libs/core`. All rendering logic lives in `libs/gui`.

## Data Pipeline

### Server

```
Exchange -> MarketDataCoreEngine -> ServerDataModel -> Persistence + SentinelStreamServer -> WebSocket
```

- **MarketDataCoreEngine**: Maintains exchange connections, parses feeds, updates order books. See `docs/MARKETDATA.md` for full details.
- **ServerDataModel**: Central data hub for all symbols. Coordinates persistence and streaming.
- **TickBinaryLogger**: Append-only binary logging with hourly rotation.
- **TimeframeAggregator**: Timer-driven aggregation (100ms, 1s, etc.) into GPU-ready slices.
- **SentinelStreamServer**: Broadcasts pre-aggregated heatmap columns to clients via WebSocket.

### Client

```
WebSocket -> SentinelStreamClient -> RemoteGridDataSource -> DataProcessor -> UnifiedGridRenderer -> GPU
```

- **SentinelStreamClient**: Boost.Beast WebSocket client with reconnection handling.
- **RemoteGridDataSource**: Manages local buffers for received slices. Emits `heatmapSliceReceived`.
- **DataProcessor**: Validates incoming slices, emits to renderer. No local aggregation in remote mode.
- **UnifiedGridRenderer**: Manages viewport state, ring-buffer uploads, drives `updatePaintNode()`.
- **HeatmapIntensityNode**: Single-quad QSG material. Samples intensity + palette on GPU.
- **MsdfGlyphNode**: QSG node for MSDF glyph quads rendered from atlas textures.

## Rendering Pipeline

### Server Side

```
LiveOrderBook -> HeatmapTwapStreamer (TWAP, dense u8 column) -> SentinelStreamServer (heatmap_slice)
```

The server produces dense `u8` columns: bids 0-127, asks 128-255.

### Client Side

```
RemoteGridDataSource -> DataProcessor -> UnifiedGridRenderer -> HeatmapIntensityNode + MsdfGlyphNode -> GPU
                                                             -> CandlestickOverlayItem -> GPU
```

The client receives pre-aggregated columns and uploads directly to GPU textures. No CPU-per-cell rendering; labels come from the MSDF atlas. Candlesticks are rendered as a GPU-batched overlay aligned to the same coordinate plane.

### Coordinate System: `TimeAxisMapping`

All chart layers (heatmap, candles, labels, future TPO/footprint) share a single coordinate mapping: `TimeAxisMapping` (`libs/gui/render/TimeAxisMapping.hpp`).

**Produced once per frame** in `UnifiedGridRenderer::updatePaintNode()` after overlap math. Consumed by all renderers in the same frame.

**Core invariant:** 1 heatmap slice = 1 candlestick bar. Same bucket width (`appendMs`), same epoch-aligned boundaries, same screen mapping.

```
TimeAxisMapping fields:
  viewStart/EndMs, viewMin/MaxPrice    — pan-adjusted viewport
  dataStart/EndMs, actualDataStart/End — ring buffer logical range
  dataMin/MaxPrice                     — data price bounds
  appendMs, tickSize, gridWidth/Height — grid parameters
  drawRect, srcRect, cellW, cellH      — overlap-computed screen geometry
  timeOffset                           — heatmap shader ONLY (ring buffer offset)
```

**Key helpers:**
- `timeToScreenX(timeMs)` — world time → screen X via srcRect/drawRect interpolation. Does NOT use `timeOffset`.
- `priceToScreenY(price)` — price → screen Y via srcRect/drawRect interpolation.
- `bucketStartMsForTime(timeMs)` — epoch-aligned bucket start.
- `visibleDataStartMs() / visibleDataEndMs()` — clamped visible data range.

**Invariant:** `timeOffset` is strictly heatmap-shader-only. Candles and labels MUST NOT incorporate it into screen mapping. They use `timeToScreenX()` / `priceToScreenY()`.

### Key Data Structures

```
heatmap_slice message:
  - time_start / time_end / timeframe_ms
  - min_price / max_price / tick_size / mid_price / last_trade
  - format=u8 / encoding=base64 / column
  - liquidity_format=u16 / liquidity_encoding=base64 / liquidity_column / liquidity_scale (optional)
  - reset (bool)
```

## Protocol

**JSON (v0)**: Used for subscription handshake, snapshots, server_config, and heatmap_slice streaming (base64 payloads).

## Threading Model

- **Server I/O Thread**: Boost.Asio io_context handles all network operations.
- **Server Aggregation**: Timer-driven on dedicated thread.
- **Client I/O Thread**: SentinelStreamClient runs on Boost.Asio strand.
- **Client GUI Thread**: Qt event loop, receives data via queued signals.
- **Client Render Thread**: QSG rendering, never touches QObject graph.

Cross-thread communication uses `Qt::QueuedConnection` exclusively.

## Performance

- Server streams 8192x8192 (67M cells) depth grids.
- Client maintains 60 FPS regardless of visible cell count.
- CPU cost in client is constant (GPU does the work).

## Runtime Notes

- Client requires a server connection (no local-only mode).
- Server is authoritative for heatmap config and timeframes; client consumes `server_config` on connect.
- GUI-only screenshot API listens on `127.0.0.1` and captures the full main window as PNG via `GET /screenshot` (see `SENTINEL_GUI_API_PORT` and `SENTINEL_GUI_SCREENSHOT_DIR`).

## Related Documentation

- `docs/MARKETDATA.md`: Complete MarketDataCoreEngine pipeline, threading, error handling.
