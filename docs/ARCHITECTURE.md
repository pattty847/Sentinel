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
- **`roller`:** Offline RAWL2 v1/v2 reading, shared journal feed parsing, daily product grids and deterministic BookRecorder driving/checkpoints. The batch converter owns no exchange connection; the live server path is unchanged in slice A. See [ROLLER.md](ROLLER.md).
- **`heatmap`:** GUI-independent sparse heatmap model (`SparseColumns`, `TimeComposer`, `binCell`/`binColumn` CPU reference, `ChunkCodec`, `HeatmapResolution`); see "Sparse heatmap core model" below.
- **`network` / `protocol`:** Owns the client-server websocket communication (`SentinelStreamClient`, `SentinelStreamServer`). See `docs/SENTINEL_STREAM_CLIENT.md` for the stream client’s role in prepping render objects.
- **`trading`:** Owns simulated order execution, local order storage, position tracking, and the shared replay/paper-trading backtest core.

### 2. GUI (`libs/gui`)
**Responsibility:** GPU-accelerated rendering, declarative UI, and data sourcing for the visualizer.
**Ownership & Boundaries:**
- The GUI layer exclusively owns rendering logic, QSG node generation, and visual widget behavior. It heavily employs Qt6, QML, and QSG.
- **`datasources`:** Acts as the ingress point from the Core network layer. `RemoteGridDataSource` receives heatmap slices and buffers them before dispatching to the renderer.
- **`render`:** The performance-critical hot-path. Owns the generation of QSG structures (e.g., `HeatmapIntensityNode`, `MsdfGlyphNode`). Must avoid manipulating `QObject` trees on the render thread to ensure low-lag performance. Coordinated entirely by the `UnifiedGridRenderer`. See `docs/UI_ARCHITECTURE.md` for a deep-dive into the GUI structure.
- **`lab`:** Isolated QRhi compute/render path for measuring client-side binning over recording entries. It is not wired into `UnifiedGridRenderer` or the server stream.
- **`qml` & `widgets`:** Owns the declarative scenes (e.g., `CandleChartView`) and the dockable window management (e.g., `ChartDock`, `OrderBookDock`).

### 3. Application Bootstraps (`apps/`)
**Responsibility:** Executable entry points.
**Ownership & Boundaries:**
- **`sentinel-server`:** Minimal footprint CLI bootstrap that instantiates the Core data daemon.
- **`sentinel_gui`:** Minimal footprint UI bootstrap that instantiates the Qt `QApplication` and connects to the server daemon.
- **`sentinel-roll` / `hmc2_diff`:** Thin QtCore bootstraps for the core roller and decoded recording comparison; output policy and conversion logic live in `libs/core/roller`.
- **`sentinel-backtest`:** Minimal CLI bootstrap that replays historical trade files through the shared trading simulation core.
- **`sentinel-lab`:** Benchmark and inspection harness for the production heatmap GPU path (S5c): `LocalChunkTransport` -> `ChunkFetcher` -> `HeatmapSourceController` on a heatmap-data thread (`lab/LabData`) -> `HeatmapTileNode` in a plain `QQuickItem` (`lab/LabItem`). Headless `--screenshot`, `--window-screenshot`, `--tick-sweep`, `--tick-change-frames`, `--s5-bench` (vs the B1 hybrid) and the binner compute `--bench`; tick controls (Auto/Manual, `--hysteresis`, `--min-row-px`, `--tick`, `--zoom-rows-px`, `--no-crossfade`), `--end-utc` (a pinned closed range), `--band-edges` (E4) and `--charts`.

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

### Sparse heatmap core model (integration slice S1)

`libs/core/heatmap` owns the GUI-independent `SparseColumns`, `TimeComposer`,
CPU `binCell`/`binColumn` reference, and `HeatmapResolution` policy. Each time
column retains native constituents with config/tick identity, original size
scale, packed row/side codes, observation duration, and per-side coverage runs.
Raw minute entries occupy eight bytes; entry-duration sidecars are omitted when
every entry uses the constituent's observed duration. Composed constituents are
explicitly marked and validation requires their exact numerator sidecar.
Composition creates exactly the selected UTC-epoch timeframe (including 16m
and 90m); only levels that divide it participate. Sealed hour scan ranges take
precedence over minutes, including gaps. `RecordingLoader` is an HMC2 adapter
for tests/lab use; hour multiples load deep hours plus the newest open hour's
minute tail, while odd timeframes use minutes. `startMs/endMs` is only a bounding
extent: explicit `scannedRanges` proves complete output buckets. `bucketState()`
distinguishes `NotLoaded`, recorder `Gap`, and `Present`; unscanned chunks and
incomplete edge buckets never become known gaps. Partial output aggregates are
omitted, so controllers retain raw input chunks until a full bucket is loaded.

Composition accumulates native rows in bounded dense scratch vectors, with a
sorted sparse merge for wide row spans, and groups time buckets in a sorted
vector. Coverage is merged by endpoint sweeps. The CPU reference normalizes each native
row/side by its own covered duration, then weights physical grids by observed
duration; config/size-scale changes on the same tick share the denominator.
Composed entries retain decoded duration-weighted numerators in memory alongside
their rounded 15-bit codes, avoiding a second log quantization before final
price binning. Future GPU/wire work must preserve this precision contract to
keep zero-code-step page parity. Missing columns remain sparse, covered zero
cells stay valid, incomplete coverage stays invalid, and incompatible grids
make only their output column unknown. `binCell` remains the independent oracle;
`binColumn` sweeps each native constituent once and bins the whole price range
for labels/walls.

### Heatmap tick and zoom contract (integration slice T)

Product contract: [heatmap interaction spec](research/2026-09-heatmap-interaction-spec.md).
`HeatmapResolution.hpp` holds the pure policy in integer price units: the preset
ladder `{1, 2, 2.5, 5} x 10^k`, Auto (`autoTickUnits`: smallest preset that is a
multiple of the common tick of the data in view and at least `minRowPx` tall;
finer only at `minRowPx * (1 + h)`, coarser only below `minRowPx * (1 - h)`;
stateful, a fixed point at any zoom), Manual preset offering (any preset some
loaded grid can build), `ManualTickMemory` per (symbol, timeframe), and the
clamps (time zoom-out one column per physical pixel; Manual price zoom-out one
row per physical pixel). There is no auto-timeframe: a column is exactly the
selected timeframe. The chart (today the lab's `LabItem`) picks the tick in
`updatePaintNode` from the `SpanSet`'s `ResolutionSummary` (`autoTickUnits` over
the rows in view) and hands it to `HeatmapTileNode` in the same frame; Manual
draws the locked preset and rows no source builds veil (it never coarsens).
Re-bins happen only on the spec rule 6 triggers; a pan inside the binned rows is
a mapping change. A 150 ms crossfade (spec rule 8, owner choice after E2; 0 =
hard switch) fades the previous picture out over the new one. The binner owns
its readback results and registers a QRhi cleanup callback: whichever of the
binner and its QRhi goes first, in-flight readbacks complete before their result
is freed and nothing calls into a destroyed QRhi (FM-099). The veil is a neutral
grey hatch, distinct from the background, data and the blue loading hatch. `idealTick` and
`layerFor` remain for the legacy page path and the near/deep migration only.

### GPU heatmap price binning (integration slice S4)

`libs/gui/render/heatmap` (target `sentinel_heatmap_gpu`) bins price on the GPU.
Time is never binned there: a worker composes `SparseColumns` at exactly the
selected timeframe (`TimeComposer`), and `buildGpuSource` turns it into an
immutable `HeatmapGpuSource`. That source holds per-bucket slots (column,
`NotLoaded` or `Gap`), native-tick groups pooled exactly as `binColumn` pools
them, merged full-coverage runs, a 16-row entry index, and per-row values
(numerator / pooled coverage * group weight, in double) as 8-byte float-float
entries. `HeatmapGpuBinner` pages a source into fresh buffers within a per-frame
byte budget, into two grow-only buffer sets (active + spare, at most one
pending source), while the previous source keeps drawing; entries are split into
≤ 64 MiB pages for D3D11, both sets together are capped (512 MiB default), and a
failed or refused pending source never disturbs the active one; it is reported
once and not retried every frame (allocation failures back off 2 s doubling to
60 s). That active/spare pair now serves only the precision self-test, the parity
tests and the compute benchmark; the chart path uses the binner's resident pool
(below). One compute invocation per
output cell sums its bin's rows per side in float-float and encodes the 15-bit
code through a threshold table that is exact against `recording::encodeSize`.
Codes, side and validity therefore equal `binColumn`. Metal compiles with fast
math, so the fast kernel launders float-float intermediates through an XOR with a
runtime zero; without it, two-sum error terms fold away (FM-094). Because that
relies on observed compiler behaviour, a runtime precision self-test runs once
per backend/device once a source is active (its CPU fixture is built on a worker); until it passes (and forever if it fails) the binner uses a
GLSL `precise` kernel variant. The re-bin key is source, grid, output size scale
and kernel variant.
Each cell carries one of four states: data, veil (scanned but unproven, or an
incompatible grid), loading (not scanned, or outside the row clip) and no data
(outside the advertised availability). The grid is anchored to absolute UTC
buckets and price bins with a guard margin, so a pan inside it only changes the
draw mapping. `HeatmapTileNode` records compute passes in
`QSGRenderNode::prepare()` and draws in `render()` inside the normal scene graph;
`tests/render/test_qsg_compute_spike.cpp` guards that mechanism. Display ticks
must be multiples of `commonTick()` (LCM of the native ticks). Not yet wired
into `UnifiedGridRenderer` (S6). Results and limits:
[GPU heatmap integration plan](research/2026-09-gpu-heatmap-integration-plan.md), S4.

### Heatmap span planning and source controller (slice S5b)

`HeatmapSpanPlanner` (core, pure) plans epoch-aligned tile spans for several
chunk sources: per span and source id, the chunk keys (`tiles::chunksFor`) and
the source's availability clipped to the span; ranks visible > fallback >
prefetch (by tile distance, at least 2 tiles or one view width per side) >
recent-tf. Source ids are data (`kChunkSources`); nothing branches on them.
Its `ResolutionSummary` is tick-free: per column and source, the native ticks,
their LCM and the full-coverage price bands. Tick selection reuses the slice-T
hysteresis (`autoTickUnitsIf`): Auto takes only presets that build every row in
view that some source covers (rows no source covers veil at every tick and
never block a tick), so the fine band reaches $1 when the rows in view lie
inside it; Manual offers every preset some loaded source builds, and
`veiledRanges` names the columns and price ranges a locked tick veils.

`HeatmapSourceController` (`libs/gui/render/heatmap`, QtCore only; it lives in
the GUI library because it builds `gpu::GpuSource` images) runs per chart on
the heatmap-data thread with the `ChunkFetcher`. It wants chunks by rank
priority, peeks the store on `chunkStored`/`chunkRevised`, and builds each
(span, source) on the process-wide `SpanSourceCache` (2-thread pool, bounded
queue, byte-bounded LRU plus a weak registry of published builds), keyed by
source, tf, span, clipped availability and chunk generations, so charts share
one build. It publishes an immutable, tick-free `SpanSet` (spans by rank,
sources coarsest common tick first, stale flags, merged `ResolutionSummary`)
through a mutex-guarded latest pointer and a queued signal. A tf switch keeps
the previous tf's built visible spans as fallback until the new view is built,
then as recent-tf (no chunk demand, no rebuild; switching back republishes
them). Admission is strictly by rank, after every surviving slot has taken its
new rank: visible and fallback always enter; prefetch needs GPU room, CPU tier
room and node credit of bytes + 10%; eviction removes only content ranked below
what it admits. The node reports through `HeatmapCapacity` whenever its
resident bytes change (free bytes, uploaded span sources, GPU loss); credit is
free bytes minus the controller's outstanding (not yet uploaded) reservations,
and an uploaded source's CPU image is released (rebuilt from chunks after a
loss). The span-source tier pins claimed images; above it the cache drops the
lowest-rank prefetch/recent-tf slots of all charts. A process-wide CPU ceiling
(1 GiB default) bounds wanted decoded chunks plus span images: each chart
commits its share to a ledger and, above the ceiling, gives up recent-tf,
prefetch, fallback, then the visible spans farthest from its view centre (they
draw as loading and are listed in `SpanSet::refused`); its nearest visible span
always stays. A built source keeps only its open chunks wanted. Results for an
older serial, or not matching the source's current desired key, are dropped.
The chunk store never evicts a key some chart wants.

### Shared heatmap data service (S6a)

`HeatmapDataService` in `libs/gui/render/heatmap` owns the process heatmap-data
thread, `ChunkStore`, `ChunkFetcher`, `SpanSourceCache`, controller creation and
destruction, and the 250 ms stats timer. Its transport factory runs on the data
thread; an optional start callback runs only after the consumers are attached.
The factory may parent ancillary clients/timers to the supplied context. At
shutdown controllers die before caches, the fetcher before its transport, and
the transport before context-owned clients; queued calls are drained while the
service is still alive. `LabData` configures local HMC2 or its own reconnecting
server client and delegates shared lifecycle/stats to this service.

The main window creates a service around `RemoteGridDataSource::streamClient()`
before connecting that client, in both renderer modes. It also installs GUI
hello/config callbacks before connecting. The adapter learns connection state
only from signals: attaching after connect misses both that state and the
initial subscription availability push. No main-chart controller exists in
S6a. In S6b a controller may receive its view/timeframe before availability;
its existing availability callback replans that pending view. The legacy
`DataProcessor::setHeatmapEnabled` is a separate mute switch that preserves
recording capability/mode; disabling recording configuration would instead
select the old slice path. S6a leaves the mute switch enabled.

`HeatmapChartSettings` is a core value type with no GUI types; validation,
QSettings storage, layout snapshots and the GUI-thread synthetic input adapter
belong to GUI. The new API settings acknowledge persistence in S6a, with an
explicit `activeRenderer: legacy`; renderer application and metrics are S6b.

### Heatmap tile node (slice S5c)

`HeatmapTileNode` (render thread) draws a `SpanSet`. Each span source build is
uploaded once into the binner's resident pool (per-frame byte budget; the binner
drops its CPU reference when the upload completes) and reported uploaded, so the
controller releases the image. Per visible span and tick the node bins the rows
around the view (one view height each side) into its own buffers: the coarsest
source first, then each finer resident source as a fill pass
(`heatmap_bin.comp` `dims.w` bit 1) that replaces only cells left veiled, and
only with valid ones (owner decision 1; `tiles::fillVeiled` is the CPU oracle).
A finer source is resident while the rows around the view overlap its bands.
Transitions: the last picture holds until every visible span of a new
(timeframe, tick) is ready, a tick change then crossfades (each change its own
fading layer), and a span whose content changed draws its previous bin until the
new one is ready. The node keeps everything it draws (current, held, fading,
fallback) resident with its sources and retires it itself; it enforces the
per-chart GPU cap (320 MiB) by evicting sources the snapshot dropped, then
recent-tf, then prefetch far to near (never visible, fallback or drawn), and
reports `HeatmapCapacity::report(free, uploaded, lost, missing)` whenever
resident bytes change (`missing`: keys it evicted after reporting them uploaded,
or released images it never held; the controller rebuilds or forgets them).
Visible spans the CPU ceiling refused draw the loading hatch. The chunk store has
no blocking loader any more: bodies arrive only through `put()`.

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
