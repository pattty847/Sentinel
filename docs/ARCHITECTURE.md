# Sentinel Architecture

## Overview

Sentinel is a GPU-accelerated trading terminal built on a mandatory client-server architecture. The server runs as a headless daemon for 24/7 data ingestion; the client is a lightweight Qt6/QML visualizer.

## Core Principles

- **Client-Server Split**: Ingestion and visualization are decoupled. Server handles data, client handles rendering.
- **GPU-Resident Rendering**: All high-density visualizations (heatmaps) are rendered directly on GPU via streamed intensity textures.
- **Zero-Copy Hot Paths**: Binary streams and pre-aggregated buffers minimize overhead between server and GPU.
- **Deterministic Threading**: Network I/O (Boost.Beast), aggregation, and rendering run on dedicated threads.

## Directory Layout

```
apps/
  sentinel_gui/       # Visualization client (remote-only)
  sentinel-server/    # Headless data daemon

libs/
  core/
    marketdata/       # Exchange transports (see MARKETDATA_ARCHITECTURE.md)
    protocol/         # Client-server WebSocket protocol
    servermodel/      # Server state, aggregation, persistence
    model/            # Shared DTOs
  gui/
    UnifiedGridRenderer   # GPU pipeline orchestration
    datasources/          # RemoteGridDataSource
    render/               # GPU strategies (HeatmapIntensityNode)
    qml/                  # UI components
```

Only `QtCore` is permitted in `libs/core`. All rendering logic lives in `libs/gui`.

## Data Pipeline

### Server

```
Exchange -> MarketDataCore -> ServerDataModel -> Persistence + SentinelStreamServer -> WebSocket
```

- **MarketDataCore**: Maintains exchange connections, parses feeds, populates cache. See `docs/MARKETDATA_ARCHITECTURE.md` for full details.
- **ServerDataModel**: Central data hub for all symbols. Coordinates persistence and streaming.
- **TickBinaryLogger**: Append-only binary logging with hourly rotation.
- **TimeframeAggregator**: Timer-driven aggregation (100ms, 1s, etc.) into GPU-ready slices.
- **SentinelStreamServer**: Broadcasts pre-aggregated heatmap columns to clients via WebSocket.

### Client

```
WebSocket -> SentinelStreamClient -> RemoteGridDataSource -> DataProcessor -> UnifiedGridRenderer -> GPU
```

- **SentinelStreamClient**: Boost.Beast WebSocket client with reconnection handling.
- **RemoteGridDataSource**: Manages local buffers for received slices. Emits `heatmapColumnReady`.
- **DataProcessor**: Validates incoming slices, emits to renderer. No local aggregation in remote mode.
- **UnifiedGridRenderer**: Manages viewport state, ring-buffer uploads, drives `updatePaintNode()`.
- **HeatmapIntensityNode**: Single-quad QSG material. Samples intensity + palette on GPU.

## Rendering Pipeline

### Server Side

```
LiveOrderBook -> HeatmapTwapStreamer (TWAP, dense u8 column) -> SentinelStreamServer (heatmap_slice)
```

The server produces dense `u8` columns: bids 0-127, asks 128-255.

### Client Side

```
RemoteGridDataSource -> DataProcessor -> UnifiedGridRenderer -> HeatmapIntensityNode -> GPU
```

The client receives pre-aggregated columns and uploads directly to GPU textures. No CPU-per-cell rendering.

### Key Data Structures

```
heatmap_slice message:
  - time_start / time_end
  - tick_size / min_price / max_price
  - column (base64-encoded u8 array)
```

## Protocol

**JSON (v0)**: Used for subscription handshake and snapshots.

**Binary (v1)**: Compact framing for live streaming. Active protocol for heatmap data.

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

## Related Documentation

- `docs/MARKETDATA_ARCHITECTURE.md`: Complete MarketDataCore pipeline, threading, error handling.
