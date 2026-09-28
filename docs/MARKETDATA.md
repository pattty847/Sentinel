# Market Data Architecture

## Overview

The market data stack is a thread-safe pipeline for real-time WebSocket feeds. The core is pure C++; Qt is used only in a thin GUI adapter.

- **MarketDataCoreEngine** — Pure C++ orchestrator: WebSocket connection, authentication, message parsing, and dispatch. Runs on a dedicated worker thread and uses `std::function` callbacks so it can be used from GUI, server, or CLI.
- **MarketDataCoreQt** — Thin Qt adapter around the engine. Receives callbacks on the worker thread and re-emits Qt signals on the GUI thread via `Qt::QueuedConnection`.

## High-level architecture

I/O runs on the worker thread; GUI runs on the Qt thread.

```
┌─────────────────────────────────────────────────────────────────────────┐
│  MarketDataCoreQt (GUI thread)     MarketDataCoreEngine (worker thread)  │
│  Signals: tradeReceived(),         Callbacks: onTrade(),                 │
│  bookUpdates(), connectionStatus()  onLiveOrderBook...(), onError()      │
│         ◀────────── Qt::QueuedConnection ──────────                      │
└─────────────────────────────────────────────────────────────────────────┘
```

The engine is stateless with respect to history; consumers are responsible for caching and state.

## Pipeline layers

Three layers inside `MarketDataCoreEngine`:

```
Exchange WebSocket → Transport → Auth (optional) → Dispatch → Callbacks (std::function)
```

### 1. Transport (`ws/`)

- **WsTransport** — Abstract interface: connection lifecycle, send/receive, status/error callbacks.
- **BeastWsTransport** — Boost.Beast over SSL on a Boost.Asio `io_context`. All operations run on a single strand (serialized, no mutex). Supports async I/O, reconnection with backoff, and keep-alive ping.

### 2. Authentication (`auth/`)

- **Authenticator** — Loads CDP API keys from `key.json` (optional) and builds signed JWTs (ES256) for authenticated channels. **Public channels** (level2, market_trades, heartbeats, candles) do not require auth; subscribe messages are sent without a `jwt` field when no key is present. Only channels such as `user` and `futures_balance_summary` need auth. Use `hasCredentials()` before `createJwt()` when keys are optional. Stateless and thread-safe.

### 3. Dispatch (`dispatch/`)

- **MessageDispatcher** — Parses JSON from the WebSocket into typed events (`Event` variant containing `TradeEvent`, `BookSnapshotEvent`, `BookUpdateEvent`, etc.). Stateless `parse` entry point. It handles DTO transformations such as fast string-to-double parsing (`Cpp20Utils::fastStringToDouble`) and ISO8601 timestamp conversion.

**Flow:** `channel=market_trades` → array of `TradeEvent`; `channel=l2_data` + `type=snapshot` → `BookSnapshotEvent`; `type=update` → `BookUpdateEvent`.

### 4. Callbacks

The engine exposes `std::function` callbacks (e.g. `TradeCb`, `OrderBookLevelUpdatesCb`, `OrderBookInitializedCb`, `ConnectionStatusCb`, `ErrorCb`). The owner (e.g. `MarketDataCoreQt`) supplies implementations and forwards to the GUI thread via `QMetaObject::invokeMethod(..., Qt::QueuedConnection)`.

### 5. Data models (`model/`)

- **Trade** — `product_id`, `price`, `size`, `side` (`AggressorSide` enum), `timestamp`.
- **BookLevelUpdate**, **BookDelta** — Incremental update structures.
- **LiveOrderBook** — Dense, fixed-range order book for visualization and GPU. Maps a continuous price range to pre-allocated `std::vector<double>` arrays for `O(1)` updates using `price_to_index`. It uses `std::mutex` to ensure thread-safety on aggregations and structural mutations. The engine does **not** own `LiveOrderBook` instances; it only delivers snapshot and update data. The consumer creates and updates the book.

## Threading & Pure C++ Boundary

- **Worker thread (Boost.Asio)** — `MarketDataCoreEngine` runs `m_ioc.run()` on a dedicated thread. All network I/O, parsing, and `std::function` callback invocation happen there. A `while (m_running)` loop with try/catch keeps the thread and `io_context` resilient to handler exceptions.
- **GUI thread Boundary (Qt)** — `MarketDataCoreQt` acts as the pure C++ boundary buffer. It lives on the GUI thread and registers C++ DTOs with Qt's MetaObject system (`qRegisterMetaType`). When a callback fires on the worker thread, the adapter uses `QMetaObject::invokeMethod` with `Qt::QueuedConnection` to safely copy DTO payloads (e.g., `std::move(updatesCopy)`) and emit signals on the GUI thread, protecting the core from Qt object leaks and keeping UI updates safe.

## Message flow

**Trades:** WebSocket → BeastWsTransport::onRead() → engine → MessageDispatcher::parse() → Trade → `m_onTrade(trade)` → adapter queues signal → GUI thread emits `tradeReceived(trade)`.

**Order book:** WebSocket → parse → `BookSnapshotEvent` or `BookUpdateEvent` → `handleOrderBookSnapshot()` or `handleOrderBookUpdate()` → `m_onLiveOrderBookInitialized()` or `m_onLiveOrderBookLevelUpdates()` → adapter queues signals → GUI updates.

## Heatmap timeframes

In `server_config`, top-level `timeframes_ms` is the configured candidate list, kept for compatibility. `heatmap.served_timeframes_ms` lists the active 1m anchor and configured integer multiples of 1m that the server rolls up from it (5m, 15m, 1h, 4h, 1d in the shipped config). The client uses this list to disable unavailable chart timeframes. If the field is absent on an older server, availability is unknown and all toolbar timeframes stay enabled. The 1s timeframe remains available for candles but is not served as a heatmap.

Heatmap rollup buckets align to UTC epoch multiples of their timeframe; 1d starts at 00:00 UTC. A bucket is present if it contains at least one recorded 1m column. Missing minutes contribute no value, and a bucket with no recorded columns is absent. Each recorded 1m TWAP contributes one minute of weight. The server maps each column into the latest observed price band for the output bucket, averages signed bid/ask intensity (bid positive, ask negative), and averages liquidity in physical units before quantizing. Live rolled slices refresh when a 1m column finalizes; the forming 1m slice continues at the sample cadence. `oldest_available_ms` for a rolled timeframe is the containing aligned bucket of the oldest 1m stored column.

Candle timeframe identifiers are millisecond durations internally and `timeframe_sec` on the wire. The 1m candle is the anchor for every configured coarser candle (5m, 15m, 1h, 4h, 1d). Rollups use the first open, last close, extrema of highs/lows, and sums of volume and trade count. Live 1m empty bars carried by the server are included in coarser bars. Historical REST backfill pages 1m candles and applies the same UTC rollup boundaries; REST does not provide trade count, so backfilled counts remain zero.

## Heatmap history transport

Clients request `heatmap_history_request` with `symbol`, `timeframe_ms`, `end_time`, and `count`. The server limits a page to 1,024 columns so a 2,048-row u16 intensity-and-liquidity response stays below the per-session write budget. Persisted reads and JSON/base64 encoding run on a two-worker pool with a global eight-job admission cap; saturation returns a protocol error and does not block the single network I/O thread. The response echoes the requested boundary as `request_end_time`, reports the storage floor as `oldest_available_ms`, and carries chronological self-describing columns.

`DataProcessor` keeps a bounded, time-keyed column cache on its worker thread; live slices and history pages both land in it and nothing is discarded because of what is on screen. The GPU ring is a window of `grid_width` consecutive buckets onto that cache: each bucket owns a fixed ring slot, so following live or panning slides the window by rewriting only the slots of buckets that enter it. While the window reaches the newest live bucket it keeps sliding with live data; when the view pans back, the window stays put, live columns go to the cache, and returning to the live edge restores them without a refetch. The worker requests pages for buckets near the view that are neither cached nor known missing; a short page marks everything older than its end as known, and `oldest_available_ms` marks the storage floor. Columns from other price bands are resampled into the window's band (merged rows keep the stronger magnitude), and server recenters rebuild the window instead of clearing it or moving the view. A per-column coverage mask shades missing buckets, live gaps included, while recorded all-zero columns stay unshaded. The chart shows an in-flight history state and reports the storage floor when a manual view reaches it. A history page's `grid_width` is its page size and never resizes the window.

## Volume profile stream

`volume_profile_slice` carries `schema_version` for the volume profile family. Session boundaries are Unix milliseconds in `session_start_ms` and `session_end_ms`. The client also accepts legacy `session_start` and `session_end` fields when the corresponding `*_ms` field is absent; the suffixed fields take precedence.

## File layout

```
libs/core/marketdata/
├── MarketDataCoreEngine.hpp / .cpp
├── ws/           WsTransport, BeastWsTransport, SubscriptionManager
├── auth/         Authenticator
├── dispatch/     MessageDispatcher, Channels
└── model/        TradeData.h, LiveOrderBook.cpp

libs/gui/marketdata/
├── MarketDataCoreQt.hpp
└── MarketDataCoreQt.cpp
```

## Design decisions

- **No cache in the engine** — Cache was removed so the engine stays a stateless processor. Consumers (GUI, server) implement their own caching. Same engine can drive both.
- **Separate Qt adapter** — Keeps `libs/core` free of Qt. The engine is reusable in any C++ context; the adapter handles thread crossing and signals.
- **Strand instead of mutex in transport** — The strand serializes async operations on the `io_context` without blocking the I/O thread; a mutex would introduce blocking and complicate the async model.

---

## Transport security (TLS / WSS)

The internal stream server (`SentinelStreamServer`) defaults to `ws://127.0.0.1:8080`. Without TLS, all traffic (market data, server config, and eventually trade commands) is plaintext. TLS upgrades to `wss://` so the stream is encrypted and the client can verify the server.

**Encrypted:** `server_config`, `heatmap_slice`, l2 snapshots/updates, `market_trades`, candle and footprint/TPO history, `trade_command`, `order_update`, `position_update`, and control messages (e.g. `subscribe`).

**Handshake:** TCP connect → TLS 1.3 ClientHello/ServerHello (server uses self-signed EC cert with SANs for localhost/127.0.0.1) → TLS Finished → HTTP WebSocket upgrade → JSON subscribe/server_config/heatmap_slice.

**Certificates:** Generate with `certs/gen-certs.ps1` (Windows) or `certs/gen-certs.sh` (Linux/macOS). Outputs `certs/sentinel-server.crt` and `certs/sentinel-server.key` (gitignored). Configure in `server_config.yaml` under `tls.cert_file` / `tls.key_file` and in `client_config.yaml` under `server.ca_file`. If `ca_file` is missing or invalid, the client falls back to `verify_none` with a log warning (acceptable for local dev only).

**Future:** Client authentication (e.g. HMAC-SHA256 of server nonce) can gate `trade_command` after the WSS handshake; the existing `Authenticator` in `libs/core/marketdata/auth/` can be used for signing.

---

## Trading stream (paper mode)

The stream protocol includes a paper-trading vertical:

- **Client → Server:** `trade_command` — `PLACE_ORDER`, `CANCEL_ORDER`, `CANCEL_ALL`, `FLATTEN`.
- **Server → Client:** `order_update` (order lifecycle), `position_update` (position and unrealized PnL).

The server runs paper execution (no real broker); fills use last trade price plus optional `trading.slippage_bps` from `server_config.yaml`. For setup and usage, see **`docs/PAPER_TRADING_QUICKSTART.md`**.

---

## Related documentation

- **`docs/ARCHITECTURE.md`** — System overview, client–server pipeline, rendering.
- **`docs/PAPER_TRADING_QUICKSTART.md`** — Paper trading configuration and hotkeys.
- **`docs/CONFIG.md`** — Server and client YAML options.

## Recording v2 core (HMC2, not yet wired)

`recording::BookRecorder` and `Hmc2Store` live in `libs/core/servermodel` and do not depend on GUI Qt. The recorder takes full, unclipped absolute L2 batches from one producer, and performs integration, compression and disk I/O on its own worker. `onInvalid` ends observation; only an accepted snapshot resumes it. An invalid interval preserves the minute's earlier numerator and peaks. Zero-observation minutes are omitted. The constructor overload accepting a local-clock function and `drainForTest()` permit deterministic replay without sleeps.

Each side has an ordered, pooled price map (O(log levels) updates, O(1) best bid/ask access). Each layer has a pooled hash map keyed by `(integer row, side)`, with lazily advanced integrals and a reused touched-row buffer. One message is applied in full before its final row sizes update peaks. Malformed prices/sizes invalidate the whole batch's book continuity. Prices use checked nearest rounding in integer price units; an inclusion window tests the row's lower price edge, so bounds are `ceil(minMid * lowFrac * priceScale / rowTickUnits)` through `floor(maxMid * highMult * priceScale / rowTickUnits)`, inclusive on both sides. Empty entries inside minute bounds mean observed zero, not a missing book.

Enqueue arrival time samples the supplied local clock; idle ticks use the last actual `local - envelope` offset. Envelope time is clamped to a nondecreasing integration clock. A crossed minute is sealed before applying the new batch, then held until `clock >= bucketEnd + latenessMs`. Backward messages cannot revise sealed integrals, even within the lateness hold; older-than-committed messages also set the current minute's late flag. Lateness is therefore a commit delay, not an event-reordering buffer. Shutdown drains accepted messages but does not advance time: the open minute and any sealed records still in the lateness hold are dropped. Crash has the same uncommitted-tail loss. Restart begins observation at the new snapshot, never the last stored timestamp. Upstream must deliver disconnect/gap/suspend invalidations in order before timer advancement; silence alone is not evidence of an invalid book.

The queue bounds queued level count and has 4,096 message slots, including control events. A dropped data batch becomes an ordered invalidation for its symbol. If the control slots themselves fill, an out-of-band barrier invalidates all symbols after the queued prefix; incoming messages, including snapshots, are dropped until that barrier is consumed. Continuity then requires a newly accepted snapshot. All drops are counted. Pooled containers and buffers reuse their high-water storage; new symbols, larger books/batches, minute sealing and hourly work can still allocate.

### HMC2 byte layout

All scalar fields are little-endian, with no native padding. IEEE-754 `f64` is used for floating fields. Symbols/layers are 1-255 ASCII letters, digits, dot, dash or underscore, excluding `.` and `..`.

Path: `<root>/<symbol>/<layer>-<tfMs>/<YYYY-MM-DD>.hmc2`, selected using the **bucket start UTC day**. A configuration mismatch against the latest generation creates `<YYYY-MM-DD>.g<N>.hmc2`, starting at N=1. Returning to an old configuration also creates a new generation. Readers visit generations numerically and return the last valid record per bucket, sorted chronologically. Queries use `[startMs, endMs)` bucket starts.

File header, schema version 2:

| Field | Encoding |
| --- | --- |
| Magic | u32 `0x32434d48` (bytes `HMC2`) |
| Schema | u16 `2` |
| Header length | u32, includes the 14-byte fixed prefix; maximum 65,536 |
| Header CRC | u32 IEEE CRC-32 over the **whole header with this field zeroed** |
| Symbol, layer | Each u16 byte length followed by bytes |
| Timeframe | i64 ms; 60,000 or 3,600,000 |
| Price scale, row tick | f64 price units per quote currency unit; i64 price units |
| Size floor, codes/octave | f64, f64 |
| Config hash | u64 |

The recorder uses FNV-1a-64 over the serialized header body (config hash zeroed), then f64 lowFrac and highMult. Header fields are compared as well as the hash. Hour records retain the originating minute policy hash, allowing rebuilds to exclude incompatible generations.

Record framing: u32 magic `0x32524348` (bytes `HCR2`), u32 compressed length, u32 raw length, u32 IEEE CRC-32 of **compressed bytes**, followed by one zstd frame (compression level 3). Raw length is bounded to 16 MiB; compressed input is bounded to `ZSTD_compressBound(16 MiB)` before allocation. Payload:

- i64 bucketStartMs; u32 observedMs; u32 flags.
- i64 bidRowLo, bidRowHi, askRowLo, askRowHi (inclusive; lo > hi is empty).
- f64 midOpen, midClose, midMin, midMax; u32 entryCount.
- Sorted unique `(row, side)` entries: LEB128 zigzag i64 row delta (first relative to 0), u16 TWAP magnitude with ask in bit 15, u16 peak magnitude. Repeated row with bid then ask uses delta 0. Peak has no side bit.

Flags: bit 0 partial (`observedMs < timeframe`), bit 1 resynced (snapshot occurred), bit 2 late events, bit 3 underflow (a positive stored TWAP/peak was below the size floor). Positive underflows encode as magnitude 1. Readers return side separately from both magnitudes. Extreme representable sizes saturate the fixed size code.

One root `.lock` is held for the writer's lifetime. Every append flushes the stream and synchronizes the file (macOS F_FULLFSYNC with fsync fallback). New directories and their parents, and directories receiving a new file, are synchronized. Windows attempts a native directory metadata flush and surfaces a failure if the filesystem denies it; this path needs platform validation. A writer that fails mid-append is poisoned for that day/config until reopen. Disk failures are logged and counted; failed minute writes never enter the live hourly accumulator.

Locked writer reopen repairs only an incomplete terminal frame at a known record boundary. Complete bad-CRC frames, bad magic and interior corruption are retained and logged. Recovery scans forward in chunks and accepts a resync candidate only after framing, CRC, zstd and payload validation; a length extending past EOF does not cause truncation if a later valid record exists. Read-only queries never truncate. Bad headers are skipped, and a writer creates a fresh generation; I/O errors surface to the caller.

Hourly layers rebuild the current hour from deduplicated, committed minute records of the matching policy on the first snapshot. For each row/side, the mean is `sum(decodedMinuteTwap * minuteObservedMs) / sum(minuteObservedMs where minute bounds cover row)`; a covered absent row contributes zero, and an uncovered row contributes neither numerator nor denominator. Peaks use decoded maxima. The hour is persisted after its own lateness watermark, including when the book is invalid at hour end.

**Schema limitation before serving:** an hourly record has one bounds interval per side and one observedMs. Its bounds are the envelope of contributing minute bounds. They cannot express disjoint coverage holes or the different denominator of each row. Stored nonzero hourly quantities use the correct coverage-weighted math, but a future reader must consult minute coverage before interpreting every absent row inside the hourly envelope as observed zero, or extend the schema with coverage runs. Further rollups must likewise use minute coverage, not blindly weight hourly values by the hour's global observedMs. This is a limitation of Revision 2's hourly representation, not permission to fabricate coverage.
