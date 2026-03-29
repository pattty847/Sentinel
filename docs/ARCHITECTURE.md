# Sentinel Architecture

## Overview

Sentinel is a GPU-accelerated trading terminal with a mandatory client–server design. The server runs as a headless daemon for data ingestion and aggregation; the client is a Qt6/QML visualizer that connects remotely.

**Core principles**

- **Client–server split** — Ingestion and visualization are decoupled. Server owns data and config; client owns rendering and UI.
- **GPU-resident rendering** — Heatmaps and labels are rendered on GPU via streamed intensity textures and an MSDF atlas.
- **Hot-path efficiency** — Pre-aggregated buffers and JSON + base64 transport keep overhead low between server and GPU.
- **Deterministic threading** — Network I/O (Boost.Beast), aggregation, and rendering run on dedicated threads. Cross-thread communication uses `Qt::QueuedConnection` only.

## Major Subsystems and Ownership Boundaries

Sentinel is rigidly divided into three main operational theaters: **Core**, **GUI**, and **App Bootstraps**.

### 1. Core (`libs/core`)
**Responsibility:** Business logic, networking, state management, and data aggregation.
**Ownership & Boundaries:**
- The Core must remain purely C++ (with minimal QtCore usage if functionally necessary). It cannot have any Qt GUI dependencies.
- **`marketdata` / `coinbase`:** Owns the exchange connections and feed parsing (`MarketDataCoreEngine`).
- **`servermodel`:** Owns the central state of the server. It aggregates high-frequency market data into GPU-ready heatmap slices and TWAP streams via the `TimeframeAggregator` and `HeatmapTwapStreamer`.
- **`network` / `protocol`:** Owns the client-server websocket communication (`SentinelStreamClient`, `SentinelStreamServer`). See `docs/SENTINEL_STREAM_CLIENT.md` for the stream client’s role in prepping render objects.
- **`trading`:** Owns simulated order execution, local order storage, position tracking, and the shared replay/paper-trading backtest core.

### 2. GUI (`libs/gui`)
**Responsibility:** GPU-accelerated rendering, declarative UI, and data sourcing for the visualizer.
**Ownership & Boundaries:**
- The GUI layer exclusively owns rendering logic, QSG node generation, and visual widget behavior. It heavily employs Qt6, QML, and QSG.
- **`datasources`:** Acts as the ingress point from the Core network layer. `RemoteGridDataSource` receives heatmap slices and buffers them before dispatching to the renderer.
- **`render`:** The performance-critical hot-path. Owns the generation of QSG structures (e.g., `HeatmapIntensityNode`, `MsdfGlyphNode`). Must avoid manipulating `QObject` trees on the render thread to ensure low-lag performance. Coordinated entirely by the `UnifiedGridRenderer`. See `docs/UI_ARCHITECTURE.md` for a deep-dive into the GUI structure.
- **`qml` & `widgets`:** Owns the declarative scenes (e.g., `CandleChartView`) and the dockable window management (e.g., `ChartDock`, `OrderBookDock`).

### 3. Application Bootstraps (`apps/`)
**Responsibility:** Executable entry points.
**Ownership & Boundaries:**
- **`sentinel-server`:** Minimal footprint CLI bootstrap that instantiates the Core data daemon.
- **`sentinel_gui`:** Minimal footprint UI bootstrap that instantiates the Qt `QApplication` and connects to the server daemon.
- **`sentinel-backtest`:** Minimal CLI bootstrap that replays historical trade files through the shared trading simulation core.

## Data pipeline

### Server

```
Exchange → MarketDataCoreEngine → ServerDataModel → Persistence + SentinelStreamServer → WebSocket
```

- **MarketDataCoreEngine** — Exchange connections, feed parsing, order book updates. See `docs/MARKETDATA.md`.
- **ServerDataModel** — Central hub for all symbols; coordinates persistence and streaming.
- **TickBinaryLogger** — Append-only binary logging with hourly rotation.
- **TimeframeAggregator** — Timer-driven aggregation (e.g. 100 ms, 1 s) into GPU-ready slices.
- **SentinelStreamServer** — Broadcasts pre-aggregated heatmap columns and related streams to clients.

### Client

```
WebSocket → SentinelStreamClient → RemoteGridDataSource → DataProcessor → UnifiedGridRenderer → GPU
```

- **SentinelStreamClient** — Boost.Beast WebSocket client; parses, validates, and emits typed slice DTOs. See `docs/SENTINEL_STREAM_CLIENT.md`.
- **RemoteGridDataSource** — Local buffers for received slices; emits `heatmapSliceReceived`.
- **DataProcessor** — Validates slices and forwards to renderer; no local aggregation in remote mode.
- **UnifiedGridRenderer** — Viewport state, ring-buffer uploads, `updatePaintNode()`; drives heatmap, footprint, TPO, candles, labels.
- **HeatmapIntensityNode** — Single-quad QSG material; samples intensity and palette on GPU.
- **MsdfGlyphNode** — QSG node for MSDF glyph quads from atlas textures.

## Rendering pipeline

**Server:** LiveOrderBook → HeatmapTwapStreamer (TWAP, dense u8 column) → SentinelStreamServer (`heatmap_slice`).  
**Client:** RemoteGridDataSource → DataProcessor → UnifiedGridRenderer → HeatmapIntensityNode, MsdfGlyphNode, CandlestickOverlayItem, and other overlays → GPU.

The server produces dense u8 columns (bids 0–127, asks 128–255). The client uploads them to GPU textures; no per-cell CPU rendering. Labels use the MSDF atlas; candlesticks are a GPU-batched overlay on the same coordinate plane.

### Coordinate system: TimeAxisMapping

All chart layers (heatmap, candles, labels, TPO, footprint) share one mapping: **TimeAxisMapping** (`libs/gui/render/TimeAxisMapping.hpp`). It is produced once per frame in `UnifiedGridRenderer::updatePaintNode()` and consumed by all renderers in that frame.

**Invariant:** 1 heatmap slice = 1 candlestick bar (same `appendMs`, epoch-aligned boundaries, same screen mapping).

- **Fields:** `viewStart/EndMs`, `viewMin/MaxPrice` (viewport); `dataStart/EndMs`, `actualDataStart/End`, `dataMin/MaxPrice` (ring and data bounds); `appendMs`, `tickSize`, `gridWidth/Height`; `drawRect`, `srcRect`, `cellW`, `cellH` (screen geometry); `timeOffset` (heatmap shader only).
- **Helpers:** `timeToScreenX(timeMs)`, `priceToScreenY(price)` (no `timeOffset`); `screenXToTime`, `screenYToPrice`; `bucketStartMsForTime`; `visibleDataStartMs` / `visibleDataEndMs`.
- **Rule:** `timeOffset` is heatmap-shader-only. Candles and labels use `timeToScreenX` / `priceToScreenY` only.

For renderer contracts and forbidden patterns, see **`docs/COORDINATE_SYSTEMS.md`**.

### Protocol data: heatmap_slice

```
time_start, time_end, timeframe_ms
min_price, max_price, tick_size, mid_price, last_trade
format=u8, encoding=base64, column
liquidity_format=u16, liquidity_encoding=base64, liquidity_column, liquidity_scale (optional)
reset (bool)
```

## Protocol

**JSON (v0)** — Subscription handshake, snapshots, `server_config`, `heatmap_slice` and related stream messages (base64 payloads where applicable).

## Threading

| Context | Role |
|--------|------|
| Server I/O | Boost.Asio `io_context`; all network operations |
| Server aggregation | Dedicated thread; timer-driven |
| Client I/O | SentinelStreamClient on Boost.Asio strand |
| Client GUI | Qt event loop; receives data via queued signals |
| Client render | QSG; must not touch QObject graph |

Cross-thread communication uses `Qt::QueuedConnection` exclusively.

## Performance and runtime

- Server can stream large depth grids (e.g. 8192×8192). Client aims for 60+ FPS; CPU cost is largely constant (GPU does the work).
- Client requires a server connection (no local-only mode). Server is authoritative for heatmap config and timeframes; client consumes `server_config` on connect.
- **Screenshot API** — GUI listens on `127.0.0.1`; `GET /screenshot` captures the main window as PNG. Configure via `gui.api_port` and `gui.screenshot_dir` in client config.

## Related documentation

| Document | Description |
|----------|-------------|
| `docs/SENTINEL_STREAM_CLIENT.md` | SentinelStreamClient: WebSocket client, message handling, validation, render-object prep |
| `docs/MARKETDATA.md` | MarketDataCoreEngine pipeline, transport, auth, threading, TLS, trading stream |
| `docs/COORDINATE_SYSTEMS.md` | Coordinate spaces and renderer contracts |
| `docs/CONFIG.md` | Server and client config files and options |
| `docs/PAPER_TRADING_QUICKSTART.md` | Paper trading setup and usage |
| `docs/FEATURES.md` | Feature overview and notable changes |
| `docs/TRADING_SIMULATION_BLUEPRINT.md` | Long-term plan for shared live paper trading, replay, and future book-aware execution |
