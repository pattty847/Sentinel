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
- The Core may use QtCore (QObject/signals, QTimer, QByteArray, QString). It cannot have any Qt GUI, Quick, QML or scene-graph dependencies.
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
- **SentinelStreamServer** — Broadcasts pre-aggregated heatmap columns and related streams to clients. Each client session is owned until explicit teardown, serializes model events on its Asio executor, and enforces bounded event/write backlogs. Upstream symbol subscriptions are reference-counted across clients.

### Client

```
WebSocket → SentinelStreamClient → RemoteGridDataSource → DataProcessor → UnifiedGridRenderer → GPU
```

- **SentinelStreamClient** — Boost.Beast WebSocket client; parses, validates, and emits typed slice DTOs. See `docs/SENTINEL_STREAM_CLIENT.md`.
- **RemoteGridDataSource** — Local buffers for received slices; emits `heatmapSliceReceived`.
- **DataProcessor** — Validates live slices and prepares bounded historical heatmap pages off the GUI thread, including missing-time columns and price-band resampling.
- **History workers** — SentinelStreamServer uses two bounded workers for persisted heatmap reads and response encoding; at most eight jobs may be queued or running, and completions re-enter the owning session through its Asio executor.
- **UnifiedGridRenderer** — Viewport state, bounded ring-buffer uploads, history request identity, and `updatePaintNode()`; drives heatmap, footprint, TPO, candles, labels.
- **HeatmapIntensityNode** — Single-quad QSG material; samples intensity and palette on GPU.
- **MsdfGlyphNode** — QSG node for MSDF glyph quads from atlas textures.

## Rendering pipeline

**Server:** LiveOrderBook → HeatmapTwapStreamer (TWAP, dense u8 column) → SentinelStreamServer (`heatmap_slice`).  
**Client:** RemoteGridDataSource → DataProcessor → UnifiedGridRenderer → HeatmapIntensityNode, MsdfGlyphNode, CandlestickOverlayItem, and other overlays → GPU.

The server produces dense live columns and self-describing u16 persisted columns. The client uploads live data incrementally and replaces historical GPU pages in bounded batches; no per-cell QML rendering is used. Labels use the MSDF atlas; candlesticks are a GPU-batched overlay on the same coordinate plane.

### Recording heatmap re-band publication

After the first recording picture, `heatmap_window::ColumnWindow` stages each new
display band on the DataProcessor worker without publishing intermediate slot
writes. The GUI keeps its existing ring, price/time mapping, coverage and texture.
The worker continues to cache live arrivals and fetch history. A replacement is
ready when every bucket intersecting the current visible time range and bounded
window is loaded or proven missing by a scanned interval/storage floor. Partially
visible buckets count; future time and off-screen prefetch do not. Row validity
and intensity are not time-page readiness signals: valid zero values and proven
gaps must both be allowed to replace the old picture.

The ready generation publishes one full immutable window update, replacing data
and mapping together through the existing snapshot/upload path. A newer re-band
discards only the staged projection and restarts readiness; generation and request
checks reject stale replies. Viewport changes re-evaluate readiness, and a reset
cancels the hold. Initial loading and legacy publication keep their existing
behavior. If a page fails, the old picture remains until fetching succeeds; it
retains its original world coordinates, so newly exposed prices/times outside
that picture remain uncovered. The walls API reads the worker's staged projection,
which can differ from the retained picture during this interval.

This first step uses an instant swap. Holding publication adds no GPU memory or
per-frame rendering work, and causes one existing full upload at readiness instead
of blanking uploads at re-band time. Readiness costs at most the window width in
map lookups per worker update while waiting (no allocations). A future two-layer
GPU fade would additionally retain an R16 texture (`2 * width * rows` bytes:
32 MiB at 8192 x 2048), its mapping and coverage, and draw a second blended quad
during the fade. That option needs separate mapping/label lifecycle handling and
live GPU measurement; it is not implemented here.

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

## Trade overlays and recording heatmaps

`servermodel/TradeOverlayPublisher` builds footprint deltas, session-relative TPO
letters and volume profiles from a bounded immutable snapshot of the shared trade
tape. The snapshot budget applies only to the requested time window. For TPO
history before the retained tape, the worker fetches bounded REST minute-candle
pages and fills their high/low ranges on the same independent grid. TPO history
and live publication respect session close.
`SentinelStreamServer::Session` owns timer cadence, request selection and
bounded admission to the history worker pool. Workers read only atomic stop flags;
replies return by executor post, with subscription/generation checks before write.
REST uses asynchronous socket operations behind its synchronous worker API, with
one 10-second deadline per call. Blocking system DNS lookups are isolated in at
most four self-owned resolver threads process-wide, so a timed-out caller does
not join a stuck OS resolver. Lookups are shared per host/port; successful
endpoints have a five-minute fresh TTL and stay usable while refresh is pending
or capacity is exhausted. Uncached callers wait within their request deadline
for capacity. No resolver thread retains server/client state.
Overlay pagination checks shutdown and selection cancellation between pages.
Candle-history REST and screener work also use the joined, bounded history pool;
no detached task retains a session or server executor. The screener uses blocking
QProcess APIs with a 90-second process deadline (uv startup, the upstream
30-second HTTP timeout, and serialization margin), cancellation checks every
50 ms, and an 8 MiB output cap. On POSIX, the launched process starts its own
process group; cancellation sends SIGTERM to the group, allows 250 ms of grace,
then sends SIGKILL to the group and reaps the direct child. Shutdown joins admitted work before destroying the REST
client or executor.
The legacy heatmap streamer remains only for its heatmap path pending removal.

The GUI carries `TradeOverlayGrid` with each immutable footprint/TPO upload.
`TradeOverlayMapping` projects that grid against the frame's common world
viewport and full surface. Texture dimensions do not imply shared price/time
coordinates. Footprint inserts neutral missing time slots and resets on changes
to its own grid; TPO clears its old band when its own price metadata changes.
Volume profile also uses the full surface and its own price metadata. Recording
re-bands do not reset these overlays or stretch them to the heatmap overlap rect.
