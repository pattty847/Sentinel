# SentinelStreamClient

The `SentinelStreamClient` (`libs/core/protocol/SentinelStreamClient.cpp`) is the client-side WebSocket component that connects to the Sentinel server, receives pre-aggregated market data and chart slices, and emits typed signals for downstream consumers. It is the **primary ingress** for all remote data that feeds the render pipeline.

## Role in the Architecture

```
WebSocket → SentinelStreamClient → RemoteGridDataSource → DataProcessor → UnifiedGridRenderer → GPU
```

- **Server** streams chart data as JSON/base64 messages and binary envelopes. Footprint cells use the flow stream and flow history; there is no footprint wire family.
- **SentinelStreamClient** parses, validates, decodes, and emits typed DTOs via Qt signals.
- **RemoteGridDataSource** owns the client instance, connects signals with `Qt::QueuedConnection`, and buffers slices before forwarding to the render pipeline.

The client is a key aspect of the server–client split: it owns the network I/O and protocol parsing, but does not perform aggregation or rendering. All emitted data is already in render-ready form (dense u8 columns, etc.).

## Connection Lifecycle

1. **Construction** — Host, port, optional CA file. If no CA file, TLS peer verification is disabled (dev mode).
2. **connectToServer()** — Starts `io_context`, resolves host:port, connects TCP, performs SSL handshake, then WebSocket handshake.
3. **onHandshake** — Sets `m_isConnected`, emits `connected()`, starts `doRead()`, and flushes any queued writes.
4. **disconnectFromServer()** — Stops `io_context`, joins the I/O thread.

All network operations run on a dedicated Boost.Asio strand in a single background thread. Cross-thread delivery to the GUI uses `Qt::QueuedConnection` on all signals.

## Threading

| Context | Role |
|---------|------|
| Client I/O | Boost.Asio `io_context` on a dedicated thread; all reads/writes on `m_strand` |
| GUI | Qt event loop; receives data via queued signals from the client |

Writes (subscribe, history requests, trade commands) are posted to `m_strand` and serialized through `m_writeQueue`. Reads are continuous `async_read` → `handleMessage` → `doRead` loop.

## Outgoing Messages (Client → Server)

| Method | Message type | Purpose |
|--------|--------------|---------|
| `subscribe(symbol)` | `subscribe` | Subscribe to symbol feed |
| `unsubscribe(symbol)` | `unsubscribe` | Unsubscribe |
| `requestHeatmapChunks(...)` | `heatmap_chunk_request` | Recorded heatmap chunks (binary replies) |
| `subscribeHeatmapLive(...)` / `unsubscribeHeatmapLive(...)` | `heatmap_live_subscribe` / `heatmap_live_unsubscribe` | The live raw tail |
| `requestTpoHistory(...)` | `tpo_history_request` | Historical TPO slices |
| `requestCandleHistory(...)` | `candle_history_request` | OHLCV candle history |
| `requestScreenerData(...)` | `screener_request` | Screener rows (crypto/stock) |
| `sendTradeCommand(...)` | `trade_command` | Order placement / modification |
| `sendAlgoCommand(...)` | `algo_command` | Algo start/stop/params |

All outgoing messages are JSON; payloads are enqueued and written sequentially.

## Incoming Message Types and Render Objects

The client parses JSON by `type` and dispatches to handler methods. Each handler validates schema, decodes base64 where needed, and emits a typed signal.

### Chart / Render Slices

| Message type | Handler | Emitted signal | Downstream use |
|--------------|---------|----------------|----------------|
| `server_config` | `handleServerConfigMessage` | `serverConfigReceived` | Config, timeframes, symbols |
| `snapshot` | `handleSnapshotMessage` | `snapshotReceived` | Order book init |
| `l2update` | `handleL2UpdateMessage` | `l2UpdateReceived` | Order book deltas |
| `trade` | `handleTradeMessage` | `tradeReceived` | Trade feed |
| `flow_bar` / `flow_history_chunk` | `handleBinaryMessage` (worker decode) | `flowBarsReceived` | FlowSeriesBuffer → FlowFeed → footprint cells; live FLB1/base64 and history FHE1/FLB1 |
| `heatmap_availability` | (inline) | `heatmapAvailabilityReceived` | Recorded heatmap sources and levels |
| `heatmap_slice`, `heatmap_history_chunk`, `heatmap_recording_live` | none | none | Retired legacy heatmap (S8a): dropped silently, no per-frame warning, until the server stops sending them (S8b) |
| `tpo_slice` | `handleTpoSliceMessage` | `tpoSliceReceived` | TPO letters → GPU |
| `tpo_history_chunk` | `handleTpoHistoryChunkMessage` | `tpoSliceReceived` (per column) | Historical TPO |
| `volume_profile_slice` | `handleVolumeProfileSliceMessage` | `volumeProfileSliceReceived` | Volume profile bins |
| `candle_history_chunk` | `handleCandleHistoryChunkMessage` | `candleHistoryReceived` | OHLCV history |
| `candle_bar_update` / `candle_bar_closed` | `handleCandleBarMessage` | `candleBarUpdateReceived` / `candleBarClosedReceived` | Live candles |
| `screener_update` | `handleScreenerUpdateMessage` | `screenerUpdateReceived` | Screener table |

### Trading / System

| Message type | Handler | Emitted signal |
|--------------|---------|----------------|
| `order_update` | `handleOrderUpdateMessage` | `orderUpdated` |
| `position_update` | `handlePositionUpdateMessage` | `positionUpdated` |
| `risk_order_update` | `handleRiskOrderUpdateMessage` | `riskOrderUpdated` |
| `algo_order_event` | `handleAlgoOrderEventMessage` | `algoOrderEventReceived` |
| `pnl_snapshot` | `handlePnlSnapshotMessage` | `pnlSnapshotReceived` |
| `coinbase_latency` | `handleCoinbaseLatencyMessage` | `coinbaseLatencyReceived` |

## Validation and Drop Logic

The client enforces protocol guardrails before emitting. Invalid messages are dropped with throttled warnings.

1. **Schema version** — Each message family (heatmap, candle, TPO, volume_profile, server_config) has a supported schema version. Mismatches or missing `schema_version` cause a drop.
2. **Grid height** — Must be ≤ `SentinelProtocol::kMaxGridHeight` (65536).
3. **Payload size** — Base64-decoded payloads must be ≤ `SentinelProtocol::kMaxPayloadBytes` (256 KiB). Estimate is checked before decode; actual size is checked after.
4. **Base64 decode** — Uses `QByteArray::fromBase64(..., AbortOnBase64DecodingErrors)`. Empty or invalid decode causes a drop.
5. **Shape validation** — TPO/volume_profile slices validate expected byte counts.

Drop reasons are logged with throttling (`kSchemaLogThrottleMs` = 2 s per reason) to avoid log spam.

## Slice DTOs and GPU Readiness

Slices emitted by the client are already in formats suitable for GPU upload:

- **HeatmapSlice** — `column` (u8 intensity), `liquidityColumn` (u16), `liquidityScale`; grid metadata (min/max price, tick size, time range).
- **TpoSlice** — `letters` (ASCII TPO per level).
- **VolumeProfileSlice** — `volumeBinsF32` (float per bin); POC, VAH, VAL.

No per-cell CPU rendering occurs in the client. The render pipeline uploads these buffers to textures and samples them on GPU.

## Dependencies

- **Boost.Beast** — WebSocket over TLS (`websocket::stream<ssl_stream<tcp_stream>>`).
- **nlohmann/json** — JSON parsing.
- **Qt** — `QObject`, signals, `QByteArray`, `qRegisterMetaType` for cross-thread signals.
- **ProtocolValidation** — `extractSchemaVersion`, `isGridHeightValid`, `isPayloadSizeValid`, `estimateBase64DecodedBytes`.
- **SentinelStreamClientParseHelpers** — `parseServerConfig`, `parseCandleBar`, `parseOrderBookLevels`, `parseL2Updates`.

## Related Documentation

- `docs/ARCHITECTURE.md` — Overall architecture and data pipeline.
- `docs/MARKETDATA.md` — Server-side market data and protocol.
- `docs/COORDINATE_SYSTEMS.md` — Renderer contracts and coordinate mapping.
