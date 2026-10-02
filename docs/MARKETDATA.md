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
- **BeastWsTransport** — Boost.Beast over SSL on a Boost.Asio `io_context`. All operations run on a single strand (serialized, no mutex). Supports async I/O and keep-alive ping.
- **MarketDataCoreEngine recovery** — Every transport-down notification, including a failed initial handshake, schedules one retry. Backoff starts at 1 s, doubles to a 30 s cap, and resets on transport-up. Duplicate down notifications share the pending retry. The watchdog keeps rearming through outages; a connected stream with no inbound message for 20 s requests one close, waits for transport-down, then retries with at least 5 s backoff. A healthy connection keeps its existing subscriptions and message handling.
- **Subscription frame scope** — Subscribe frames name exactly the added products; unsubscribe frames name exactly the removed products, never the remaining desired set. Desired-state mutations run on the engine's I/O strand. Reconnect replays all desired products. Heartbeats are connection-scoped (no `product_ids`) and are never unsubscribed while any product remains desired; removing the last product sends its L2/trade unsubscriptions plus the heartbeat unsubscribe.
- **Per-product L2 silence** — `ReconnectPolicy::level2Stale` defaults to 30 s and is checked by the 2 s watchdog using steady time, independently for each desired product. Silence invalidates only that product via `onLiveOrderBookInvalidated` and sends only its L2 unsubscribe/subscribe pair; other products, trades and heartbeats stay subscribed. A missing recovery snapshot doubles that product's retry interval up to `level2RetryMaximum` (10 min), with one error per attempt. Failure counts and retry intervals survive reconnect/replay; only a valid snapshot followed by updates resets them. Shared reconnect escalation is limited to two attempts per failing product. A product that has never supplied an accepted snapshot cannot reconnect a healthy peer; a previously accepted product (`everAccepted`, retained across reconnects) can escalate even while peers remain healthy. Such a reconnect records the resnapshot cooldown before closing; actual transport loss still invalidates every book. After the limit, the product stays invalid and retries only its own L2 subscription at the backed-off rate. Duplicate subscribe alone is not assumed to supply a snapshot; [Coinbase documents product/channel unsubscribe](https://docs.cdp.coinbase.com/coinbase-app/advanced-trade-apis/websocket/websocket-overview).
- **Quiet products** — Matching recovery snapshot fingerprints, with no intervening updates, double that product's silence threshold up to `level2QuietMaximum` (5 min). Updates reset the threshold to 30 s. This uses bounded snapshot evidence rather than a second order book: updates invalidate the comparison baseline, and the next snapshot establishes a new baseline. Fingerprints affect polling frequency only, never book validity. Timestamp/flag updates allocate nothing per L2 event. All policy fields are constructor-only, not YAML settings. Raw-capture replay honors the product on invalidation/resync markers; empty or absent product means every book.
- **Subscription acknowledgement reconciliation** — Parse Advanced Trade `events[].subscriptions.level2` (and channel-list forms) separately from other channel products; heartbeats never appear as products. Warn for desired products missing from an explicit cumulative L2 list, except products currently awaiting a recovery snapshot (unsubscribe acknowledgement is expected during recovery). A trades-only ack makes no L2 claim. Acks are diagnostic, not proof of book freshness. Provider errors within 5 s of a scoped recovery unsubscribe additionally warn with the product, frame type and elapsed time; this is temporal correlation, not proof that the frame caused the error, and does not change error handling.

- **BeastWsTransport deadlines** — One connect attempt (DNS resolve, TCP connect, TLS handshake, WS handshake) must finish within `server.mdc.connect_timeout_ms` (default 20 s); the timeout is reported as `onError("connect timed out in <phase> ...")` plus one `onStatus(false)`, so it enters the backoff above. A close (including the stale-heartbeat close to a dead peer) reports down within `server.mdc.close_timeout_ms` (default 3 s) instead of Beast's 30 s close timeout. Each attempt has an id; `connect()`, `close()` and any terminal outcome retire it, so a late resolve/connect/handshake callback from a timed-out attempt is ignored. No deadline runs while connected.

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

Candle history follows the visible time range with one additional screen of older prefetch. Requests use a leading/trailing 100 ms throttle: send immediately when idle, then keep the earliest trailing deadline during sustained movement. Each page requests at most 350 bars; coarser pages target about 350 anchor minutes (at least one output bucket). The client serializes candle-history requests across selection changes, matches replies by symbol/timeframe/start/end, and rejects previous selection generations before merging. History merges on the GUI-owned candle buffer, never in QSG callbacks, without advancing the live sequence. A bucket owned by the live stream always wins over history, even when a REST snapshot was labelled closed after crossing a boundary during its fetch; live updates can also take ownership of a previously closed history-only bucket. Reconnect clears ownership and the live sequence counter for every cached series; selecting a cached symbol/timeframe again does the same for that series. These cached bars become eligible for a history refresh until new live updates establish ownership. Reconnect and cached selection re-entry queue one newest-first refresh pass covering the visible range plus prefetch, even when the oldest cached bar already covers that range or the cache is full. The pass uses the normal page caps, throttle and single-flight guard; after its pages finish, older backfill resumes. Live candle deliveries carry a local client generation captured before their queued handoff. The data source rejects mismatched generations, symbols and timeframes so a queued pre-reentry bar cannot reclaim ownership after an A-B-A switch. This tag does not change the wire protocol.

An empty or overlap-only successful page advances the scan cursor to the previous window; it does not prove that older history is exhausted. Three consecutive empty windows pause that symbol/timeframe for 60 seconds. While the viewport still needs older data, a timer resumes from the saved scan cursor, including for a stationary viewport; it never restarts at the loaded edge after a pause. Empty 1s scans have a seven-day total time lookback from their first empty window, preserved across pauses; REST timeframes have no additional empty-lookback cap and remain paged and paused. Finding older bars starts a new empty-scan budget. Returning to loaded coverage, filling local capacity, or exhausting the 1s lookback cancels the retry timer. Transport errors preserve the cursor and retry the same window after 2 seconds. Backfill stops at the existing local retention capacity (60,000 bars at 1s, 20,000 through 1m, 10,000 coarser), so pages cannot loop after immediate cache eviction.

The server's 1s history contract is an inclusive `end_time_sec` with `limit` counting retained bars (capped at 350), drawn from its 10,000-bar memory window. For sparse series, returned bars may predate the nominal `start_time_sec = max(0, end_time_sec - limit)` metadata; that field is not a lower filter bound. The client pages backward with `end_time_sec = oldest_loaded_second - 1` to exclude overlap. Coarser history uses REST time windows.

## Heatmap history transport

Clients request `heatmap_history_request` with `symbol`, `timeframe_ms`, `end_time`, and `count`. The server limits a page to 1,024 columns so a 2,048-row u16 intensity-and-liquidity response stays below the per-session write budget. Persisted reads and JSON/base64 encoding run on a two-worker pool with a global eight-job admission cap; saturation returns a protocol error and does not block the single network I/O thread. The response echoes the requested boundary as `request_end_time`, reports the storage floor as `oldest_available_ms`, and carries chronological self-describing columns.

Recording history is opt-in: omit `source` or send `"legacy"` for the existing response. With `source: "recording"`, also send `price_min`, `price_max`, and `rows` (1–16384); `display_tick` is optional and selects an exact server display tick. Optional `request_id` (string) and `band_generation` (unsigned integer) correlate rebands; the reply echoes both, including protocol errors. The server caps `count` at 1024 and also limits each recording reply to at most 2,000,000 requested cells. `end_time=0` selects latest; otherwise it is an inclusive output-bucket boundary. The builder runs on the same bounded history worker pool, with one lazily created `Hmc2Reader` per worker thread and per-request record, entry, and time budgets.

A recording `heatmap_history_chunk` keeps `schema_version`, `encoding: "base64"`, `format: "u16"`, `columns`, and the usual symbol/timeframe/request boundary. It adds `source: "recording"`, `value_encoding: "absolute_log_size"`, `size_floor`, `codes_per_octave`, `layer`, `band_lo`, `band_tick`, `band_rows`, `request_id`, `band_generation`, and `status`. `band_lo` is the lower edge of the bottom row; row arrays descend from the top row. The server pads any short builder band at the top to exactly `rows`, leaving padding unknown unless coverage was proven. Each column has `column` (little-endian u16 absolute log codes), `liquidity_column` (little-endian u16 dominant-side quantity), `liquidity_scale`, `validity` (base64 bytes; bit *i*, least significant bit first, applies to array row *i*), `observed_ms`, and `flags`. Both arrays contain exactly `band_rows` cells. The existing `time_start`, `time_end`, `min_price`, `max_price`, and `tick_size` remain self-describing.

Page fields `scanned_start` and `scanned_end` bound the proven half-open scanned interval, `next_end` is the next inclusive paging boundary, and `exhausted` alone says the storage floor was reached. `oldest_available_ms` and `latest_available_ms` are output bucket starts (zero means unavailable). `status` is `complete`, `budget`, `cancelled`, `invalid_request`, `incompatible_grid`, or `io_error`; `budget` is a normal partial page and never implies exhaustion. The client must not infer missing older history from a short page. `server_config.recording` advertises `available`, `layers`, `timeframes_ms`, `size_floor`, and `codes_per_octave`; `available` is false when the recorder did not start.

`DataProcessor` keeps a bounded, time-keyed column cache on its worker thread; live slices and history pages both land in it and nothing is discarded because of what is on screen. The GPU ring is a window of `grid_width` consecutive buckets onto that cache: each bucket owns a fixed ring slot, so following live or panning slides the window by rewriting only the slots of buckets that enter it. While the window reaches the newest live bucket it keeps sliding with live data; when the view pans back, the window stays put, live columns go to the cache, and returning to the live edge restores them without a refetch. The worker requests pages for buckets near the view that are neither cached nor known missing; for legacy pages only, a short page marks everything older than its end as known, and `oldest_available_ms` marks the storage floor. Columns from other price bands are resampled into the window's band (merged rows keep the stronger magnitude), and server recenters rebuild the window instead of clearing it or moving the view. A per-column coverage mask shades missing buckets, live gaps included, while recorded all-zero columns stay unshaded. The chart shows an in-flight history state and reports the storage floor when a manual view reaches it. A history page's `grid_width` is its page size and never resizes the window.

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

## Recording v2 core (HMC2)

`recording::BookRecorder` and `Hmc2Store` live in `libs/core/servermodel` and do not depend on GUI Qt. The recorder takes full, unclipped absolute L2 batches from one producer, and performs integration, compression and disk I/O on its own worker. `onInvalid` ends observation; only an accepted snapshot resumes it. An invalid interval preserves the minute's earlier numerator and peaks. Zero-observation minutes are omitted. The constructor overload accepting a local-clock function and `drainForTest()` permit deterministic replay without sleeps.

Each side has an ordered, pooled price map (O(log levels) updates, O(1) best bid/ask access). Each layer has a pooled hash map keyed by `(integer row, side)`, with lazily advanced integrals and a reused touched-row buffer. One message is applied in full before its final row sizes update peaks. Malformed prices/sizes invalidate the whole batch's book continuity. Prices use checked nearest rounding in integer price units; an inclusion window tests the row's lower price edge, so bounds are `ceil(minMid * lowFrac * priceScale / rowTickUnits)` through `floor(maxMid * highMult * priceScale / rowTickUnits)`, inclusive on both sides. Empty entries inside minute bounds mean observed zero, not a missing book.

Enqueue arrival time samples the supplied local clock; idle ticks use the last actual `local - envelope` offset. Envelope time is clamped to a nondecreasing integration clock. A crossed minute is sealed before applying the new batch, then held until `clock >= bucketEnd + latenessMs`. Backward messages cannot revise sealed integrals, even within the lateness hold; older-than-committed messages also set the current minute's late flag. Lateness is therefore a commit delay, not an event-reordering buffer. Shutdown drains accepted messages but does not advance time: the open minute and any sealed records still in the lateness hold are dropped. Crash has the same uncommitted-tail loss. Restart begins observation at the new snapshot, never the last stored timestamp. Upstream must deliver disconnect/gap/suspend invalidations in order before timer advancement; silence alone is not evidence of an invalid book.

The queue bounds queued level count and has 4,096 message slots, including control events. A dropped data batch becomes an ordered invalidation for its symbol. If the control slots themselves fill, an out-of-band barrier invalidates all symbols after the queued prefix; incoming messages, including snapshots, are dropped until that barrier is consumed. Continuity then requires a newly accepted snapshot. All drops are counted. Symbols are interned once on the producer, so even long symbol names do not allocate on repeated enqueues. Pooled containers and buffers reuse their high-water storage; new symbols, larger books/batches, minute sealing and hourly work can still allocate.

### HMC2 byte layout

All scalar fields are little-endian, with no native padding. IEEE-754 `f64` is used for floating fields. Symbols/layers are 1-255 ASCII letters, digits, dot, dash or underscore, excluding `.` and `..`.

Path: `<root>/<symbol>/<layer>-<tfMs>/<YYYY-MM-DD>.hmc2`, selected using the **bucket start UTC day**. A configuration or file-schema mismatch against the latest generation creates `<YYYY-MM-DD>.g<N>.hmc2`, starting at N=1. Returning to an old configuration also creates a new generation. Readers visit generations numerically and return the last valid record per bucket, sorted chronologically. Queries use `[startMs, endMs)` bucket starts. Supported UTC years are 2000 through 2200 inclusive (`[2000-01-01, 2201-01-01)`). Appends and path construction reject times outside this range before calendar conversion; range queries clamp their bounds. The recorder also rejects event/local clock values outside it, preventing microsecond timestamps from advancing the minute loop.

File header: new minute writes use schema 3 (temporal deltas); new hour writes use schema 4 (absolute with coverage). Readers also accept schemas 1/2 as absolute records and schema 3 hours as approximate coverage:

| Field | Encoding |
| --- | --- |
| Magic | u32 `0x32434d48` (bytes `HMC2`) |
| Schema | u16 `3` (minutes), `4` (hours) |
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
- Schema 1/2: sorted unique `(row, side)` entries: LEB128 zigzag i64 row delta (first relative to 0), u16 TWAP magnitude with ask in bit 15, u16 peak magnitude. Every record is absolute.
- Schema 3: after entryCount, u8 kind (`0` keyframe, `1` delta), then i64 predecessor bucketStartMs (`0` for keyframes). EntryCount counts changes for a delta, including removals.
- Schema 3 entries: LEB128 zigzag i64 row delta (first relative to 0), u8 side (`0` bid, `1` ask), LEB128 zigzag `(twapCode - previousTwapCode)`, LEB128 zigzag `(peakCode - twapCode)`. Previous TWAP is zero for keyframes/new keys. Repeated row with bid then ask uses row delta 0. Codes remain 15-bit magnitudes; side is separate.
- Schema 4 is restricted to tf=3,600,000 and kind=0. It uses the schema 3 entry layout followed by u32 `coveredMs` on **each entry**. After all entries: u32 coverage-run count, then each run as i64 lo, i64 hi (inclusive native rows), u8 side, u32 coveredMs. Runs are sorted by `(side, lo)`, disjoint within a side, and include covered zero rows. Each duration is positive and at most record observedMs; every entry must lie in a same-side run with the same duration. Coverage outside all runs is zero. Raw/frame length caps still apply. Schema 1-3 hour entries receive record observedMs as their in-memory coveredMs and flag bit 4 (approximate coverage).
- Keyframes list every key. Deltas list only changed keys; omitted keys retain both codes. A removal reconstructs TWAP and peak as zero. A record containing a zero-TWAP entry uses a keyframe: this preserves legitimate instantaneous peaks with zero integrated TWAP without confusing them with delta removals. Empty books have no entries. Bounds, mids, observation time, and observation flags are absolute metadata on every record; changing bounds does not implicitly remove keys.

A keyframe starts every UTC 15-minute interval containing a record, and is also required on writer reopen/eviction, after rollback/write failure, and on a gap, duplicate, or backward timestamp. Hourly records are therefore always keyframes. Delta predecessors must be the immediately preceding record in that same file and series, exactly one timeframe earlier and inside the same 15-minute interval. Writer state advances only after a durable append; the cache holds at most 64 file writers and eviction discards the base. Sparse changes with an unrepresentable signed row jump, or replacements exceeding the raw limit, fall back to a keyframe.

Range readers reconstruct from the query start's UTC 15-minute boundary (at most 15 minutes of preceding records), maintaining an independent base per file before deduplicating generations. The format has no seek index: readers still scan framing and compressed payloads to find timestamps, but do not reconstruct older entry states. A missing/corrupt predecessor invalidates the chain: deltas warn and are skipped until a valid keyframe, never applied to another generation or an earlier surviving bucket. Framing, CRC, zstd, varints, code ranges, sorted unique keys and reconstructed state size remain bounded/validated. Readers never repair.

Flags: bit 0 partial (`observedMs < timeframe`), bit 1 resynced (snapshot occurred), bit 2 late events, bit 3 underflow (a positive stored TWAP/peak was below the size floor), bit 4 approximate coverage (legacy hourly records). Positive underflows encode as magnitude 1. Readers return side separately from both magnitudes. Extreme representable sizes saturate the fixed size code.

One root `.lock` is held for the writer's lifetime. Every append flushes the stream and synchronizes the file (macOS F_FULLFSYNC with fsync fallback). Every directory and its parent chain are synchronized on first writer use, including existing directories left by a previous process; directories receiving a new file are synchronized again. New generations use exclusive creation so a filename collision cannot overwrite history. Windows attempts a native directory metadata flush and surfaces a failure if the filesystem denies it; this path needs platform validation. An append records its starting file offset. On failure it attempts to truncate back to that offset and synchronize the rollback, evicts the cached writer, and rethrows the original error. The next append reopens and repairs the file instead of losing the remainder of the day; rollback failures are separately logged. Disk failures are logged and counted; failed minute writes never enter the live hourly accumulator.

Locked writer reopen repairs only an incomplete terminal frame at a known record boundary. Complete bad-CRC frames, bad magic and interior corruption are retained and logged. Recovery scans forward in chunks and accepts a resync candidate only after framing, CRC, zstd and payload validation; a length extending past EOF does not cause truncation if a later valid record exists. A possible torn tail following interior damage is retained, with an explicit warning that its boundary is unverified. Read-only queries never truncate. Bad headers are skipped, and a writer creates a fresh generation without overwriting empty or partial header files. Range reads warn and skip unreadable files or directory entries, preserving readable history from other days; write I/O errors still throw.

Hourly layers rebuild the current hour from deduplicated, committed minute records of the matching policy on the first snapshot. Startup also reconstructs and writes the immediately preceding hour if its matching-policy rollup is missing, closing the crash window between the last minute commit and the hourly write. Existing hourly records are left alone. Recovery/write failures are logged without invalidating unrelated live books, and hourly accumulation failures do not label an already-committed minute as lost. For each row/side, the mean is `sum(decodedMinuteTwap * minuteObservedMs) / sum(minuteObservedMs where minute bounds cover row)`; a covered absent row contributes zero, and an uncovered row contributes neither numerator nor denominator. Peaks use decoded maxima. The hour is persisted after its own lateness watermark, including when the book is invalid at hour end.

Schema 4 removes the earlier hour-coverage limitation: sparse quantities carry their own denominator, and coverage runs preserve the denominator for absent zero rows and holes in moving bounds. Multi-hour means remain exact with respect to stored, quantized TWAPs; the hourly mean still incurs the fixed log codec's normal rounding error.

### Recording history builder (core S1)

`recording::Hmc2Reader` is a worker-owned read session. `visit` reconstructs records in chronological order for `[start,end)`, using the last valid record per bucket across numeric generations and append order. It caches frame metadata for at most 64 files, the active reconstructed record, and a replay window capped at 16 records / 262,144 entries. It never retains the requested range's records. The original tolerant `Hmc2Store::readRange` remains available for recorder recovery.

A shared `ReadControl` bounds source records, entry work, wall time and cancellation across availability and scans. Cold index discovery charges source records without parsing entries; a budget stop retains the completed metadata prefix and resumes on the next request. Appends extend that index from its prior boundary only after validating the old tail frame's framing/CRC and header. Shrink, same-size rewrite with changed mtime, or a damaged anchor rebuilds it. Index and directory-listing caches use LRU eviction (64 entries); directory mtime refreshes the listing, and file size/mtime refreshes series availability. All public reader operations and destruction assert the constructing thread in debug builds; S2 needs one reader per history worker.

Limits are soft admission budgets, clamped to at least 1 source record, 1 entry-work unit, and 10 ms. Reconstruction charges inherited plus changed entries and a conservative coverage-run bound; cache hits still charge. The first selected record and its at-most-15-record delta chain are admitted as one unit even when they exceed the requested record/entry budget. Availability likewise admits its first edge candidate on each side, so discovering the floor cannot repeatedly consume the entire admission budget. A first output column with at most 16 native time buckets defers the deadline through projection; larger rollups must fit their budget. Counters report actual work, including overshoot. Cancellation remains immediate at cooperative checks, including inside these first units. Discovery may still return no columns, but a reused reader advances its cached cursor. One-shot root calls discard that progress, so history workers must reuse their reader. Default `ReadLimits` is unlimited.

I/O policy: discovery warns and skips permission-denied/corrupt files, continuing to other days/generations like `readRange`. A discovery that skipped a file is not cached as definitive availability, so recovery is retried. Other I/O failures while discovering a required range or availability boundary return IoError; they cannot establish an empty series. Once a candidate is selected, open/read failure, disappearance, shrink or bucket-identity change returns `IoError`; it does not advance the scanned interval or cache a gap. CRC/zstd/payload corruption is distinct: warn, skip the bad candidate, and try an earlier valid generation. Directory enumeration failures remain errors, never proven empty history.

`recording::buildPage(reader-or-root, BuildRequest, StopToken)` returns ascending columns and a common `(lo,tick,rows)` band, layer and size-code scale. Requests accept 1m multiples below an hour, integer-hour multiples thereafter, at most 1024 output buckets, up to 16384 rows, and an optional exact display tick. Automatic ticks use the shared `recording::ladderTickUnits` ladder (`{1,2,2.5,5} x 10^k`, restricted to native-tick multiples) until `ceil(hi/tick)-floor(lo/tick) <= rows`; the returned row count is that actual count. Explicit ticks must fit the band and be integral native-tick multiples. Cells are half-open on the absolute price grid; arrays descend from the highest row, with lower-edge prices. Minute history selects near below the recorded deep tick and deep otherwise. Hour history uses deep; near-resolution hour requests are rejected because the recorder does not persist near hours.

Rollups preserve numerator and denominator per native row and side before summing at the display tick. Covered absent entries contribute zero; uncovered rows contribute neither numerator nor denominator. Both sides remain separate until dominant-side projection, with bids winning ties. Columns carry absolute log cells, linear u16 dominant-side quantities with `quantityScale`, observedMs, flags, and an LSB-first validity mask in the same descending order. A row is valid only when **every native row on both sides** was covered for all observed time on its source grid. Partial observation sets `partial` without turning observed zeros into unknown rows. Legacy hours propagate `approximate coverage`. Grid changes across recording generations are evaluated per output bucket. Compatible grids retain their own native-row denominators; their display-row side quantities are combined by observed duration before choosing the dominant side, and validity requires full coverage in every grid. If any constituent native tick does not divide the display tick (for example old $10 rows at a $25 display tick), that output column carries zero cells/quantities and an all-unknown validity mask, retaining its observed duration and flags; the page still completes. A $50 display tick represents both old $10 and new $5 rows. No native row is split to invent finer detail.

For tf >= 1h, persisted hours are combined with minutes from the latest unpersisted hour only. Minutes belonging to a persisted hour never participate. Result availability uses output bucket starts. `endMs=0` selects latest; nonzero end is inclusive and aligned down to output timeframe. The builder walks output buckets backward, then returns columns ascending. Only completed output buckets enter `[scannedStartMs,scannedEndMs)`; unfinished rollups are discarded on budget/cancellation. Gaps inside that interval have been scanned. `nextEnd` is the preceding output bucket; `exhausted` means the availability floor was reached, never merely that a page was short. An I/O failure returns a status without claiming the unread interval. Root overloads are one-shot; serving workers should reuse a reader.

### Sparse recording chunks (S2 codec, served by S3)

`recording::buildChunk` reads one native level of a chunk source into `heatmap::SparseColumns`: minute chunks span one UTC hour, and hour chunks (source `hmc2.deep` only) span one UTC day. The chunk start is aligned to that span. The output extent is the full span, but `scannedRanges` proves only the reader's completed scan. For an open chunk, pass `BookRecorder::watermarks(symbol,layer)`; the builder selects the native level's exclusive cutoff and scans only complete buckets before it. The minute cutoff has already applied recorder lateness. The separate hour cutoff advances after the hour rollup is written, so an unwritten hour remains `NotLoaded`. `chunkState` seals when the selected cutoff reaches the chunk end; a sealed state reports `committedThroughMs` equal to the chunk end and revision 0, so sealed bytes and content hashes do not depend on how far the recorder has moved on. Sealed frames require one complete scanned range. The shared, mutex-protected encoded LRU stores sealed chunks only, is bounded by encoded bytes, and returns immutable shared buffers.

`heatmap::ChunkCodec` v2 encodes a chunk as little-endian `SHC1`, `u16` wire version, `u8` kind, `u8` sealed state, length-prefixed symbol and source id, signed `i64` level/start/end, grid identity (`u64` config hash, `i64` row tick units, `f64` price scale), `SizeScale` (two `f64`), signed `i64` committed-through time, `u64` revision, `u64` FNV-1a content hash of the header prefix and uncompressed payload, `u32` column and entry counts, `u32` raw and compressed lengths, then a zstd frame. Request identity is outside cached SHC1 bytes: `SHE1`, `u16` envelope version, `u64` request id, then SHC1 bytes. The header grid and scale are zero when the chunk contains multiple native identities; each native constituent always carries its own identity and scale in the payload. The payload starts with aligned scanned ranges, then column/native records (bucket, observed duration, flags, coverage runs, entry counts), then entries in column/native order. Entries use row-delta unsigned varints with the side in bit 0, `u16` original size codes, and optional covered-duration varints. v2 transports native columns only; composed numerators have no portable encoding yet. v2 replaced v1's `u8` near/deep layer code with the source id string; v1 frames are refused (no compatibility shim). Raw payloads and whole response envelopes are limited to 16 MiB. The decoder checks the zstd frame content size before allocating, then rejects unknown versions and malformed lengths, counts, hashes, or sparse invariants. Both encoder and decoder validate source/level and timestamp bounds/alignment before deriving end times, including `not_modified` control frames. The JSON recording page protocol (`heatmap_history_request` with `source: "recording"`) stays active until S8 deletes it.

**Chunk sources.** The key is `(symbol, source, levelMs, startMs)`. `source` is a neutral id; the wire never names near/deep. Today's ids are `hmc2.near` (1m level) and `hmc2.deep` (1m and 1h levels). They are migration-only: they map to the HMC2 recording layers while HMC2 is the source (`heatmap::kChunkSources`), and nothing may branch product or renderer behaviour on them. Raw-derived levels will add new ids. `SparseColumns::layer` still holds the HMC2 layer name in memory only.

**Control frames.** Kind `not_modified` (3): `SHC1`, version, kind, `u8` sealed, symbol, source, `i64` level and start, `i64` committed-through, `u64` revision, `u64` content hash of the chunk the requester already holds; no payload. Kind `error` (4): `SHC1`, version, kind, symbol, source, `i64` level and start (echoed as sent, not validated), `u16` code (1 `invalid_request`, 2 `unavailable`, 3 `busy`, 4 `build_failed`), length-prefixed message (at most 255 bytes). Both are exact-length. Kind `live_column` (2) carries the native raw tail described below.

### Heatmap chunk wire (S3)

Requests and availability are JSON text frames; chunk replies are binary WebSocket frames (`SHE1` envelope around `SHC1`). Existing JSON messages are unchanged. The server sets the frame type per queued write (`PendingWrite::binary`); the client branches on `got_binary()`. A binary frame sent by a client is logged and dropped by the server.

- **Request (client -> server):** `{"type":"heatmap_chunk_request","req":<u64>,"symbol":..,"source":"hmc2.deep","level_ms":60000,"starts":[<i64>...],"have_hash":["<16 hex>"|""...]}`. 1..64 starts; `have_hash` is absent or parallel to `starts` (hashes travel as 16 lowercase hex digits because JSON numbers are not reliably 64-bit). Each start gets exactly one binary reply carrying `req` in the `SHE1` envelope: `chunk`, `not_modified` (when `have_hash` equals the current content hash), or `error`. A malformed request gets one `invalid_request` error with the echoed `req` (0 when unreadable) and start 0.
- **Sealed and open:** sealed chunks never change and are served from the shared byte-bounded LRU (256 MiB) as exact bytes. Open chunks are rebuilt per request and carry `committedThroughMs` plus a revision: `1 + complete level buckets before the cutoff`. Content is a function of that cutoff, so a later revision is never older data. The client keeps the newest state per key (sealed beats open; open `(revision, committedThroughMs)` never goes back) and reports an older reply as a local `superseded` error, because two server workers can finish the same key out of order.
- **Cutoffs:** the recorder's per-level watermarks when it records the series in this process. Otherwise, with t = now - 5 min (a freshly started series publishes its first watermark within about a minute), minutes before t's UTC minute and hours before the UTC hour preceding t's are committed.
- **Availability (server -> client):** `{"type":"heatmap_availability","symbol":..,"chunk_wire_version":2,"sources":{"hmc2.deep":{"migration_only":true,"oldest_ms":..,"latest_ms":..,"grids":[{"config_hash":"<hex>","row_tick_units":..,"price_scale":..,"size_floor":..,"codes_per_octave":..}],"levels":[{"level_ms":60000,"chunk_span_ms":3600000,"oldest_ms":..,"latest_ms":..,"committed_through_ms":..}]}}}`. `oldest_ms`/`latest_ms` are persisted bucket starts; `grids` holds the newest 1m record's native grid. Sent on every `subscribe` (when recording is available) and pushed again when a per-level cutoff changes (checked once a second, built on a history worker). The client refuses a message whose `chunk_wire_version` differs from its own. `server_config.recording.chunk_wire_version` announces the version too.
- **Budgets:** per session at most 4 chunk jobs building (half of the server-wide eight-job history pool, so one session cannot starve candle/TPO history) and 32 MiB of chunk replies queued; a start beyond either gets an explicit `busy` error, never a silent drop. A full server pool is also `busy`. Chunk replies are accounted separately from the 16 MiB slow-client write limit; immediate refusals use the ordinary limit. One admitted reply can overshoot the byte budget by at most one frame (16 MiB).
- **Client (`SentinelStreamClient`):** `requestHeatmapChunks(symbol, source, levelMs, starts, haveHash)` returns the `req`. Binary frames are decoded on a dedicated one-thread pool (FIFO, off the network thread), with at most 256 queued/running frames and a 64 MiB decode backlog (both limits include work from invalidated connections until it retires); overflow is refused as `client_overloaded`. Frames shorter than the 14-byte envelope header are refused inline as `malformed`, without posting decode work. Admission refusals are coalesced to one notification per reason until a decode completes (or a new connection starts), so rejection cannot itself enqueue an unbounded error backlog. Signals: `heatmapChunkReceived(req, shared const ChunkFrame)` for `chunk` and `not_modified`, `heatmapChunkFailed(error)` for server errors and the local codes `malformed` (the decoder rejects hostile input before allocating), `superseded` and `client_overloaded`, and `heatmapAvailabilityReceived`. Disconnect invalidates outstanding binary decodes immediately. The connection epoch is checked after decoding under the same lock that clears ordering state and publishes success/error signals, so even a decode already running cannot publish or repopulate ordering state across disconnect/reconnect. Consumers connect these signals with `Qt::QueuedConnection`. No client-side request budget yet: the S5 controller paces requests.
- **Teardown:** jobs hold only a weak session reference; a reply for a closed or destroyed session is discarded, and `stop()` joins running builds.

### Raw heatmap live tail (S5L-a)

Servers advertise `server_config.recording.chunk_live: true` only when recording is available and the live service exists. A client sends
`{"type":"heatmap_live_subscribe","sub":<u64>,"symbol":..,"sources":["hmc2.deep","hmc2.near"],"since_ms":<i64>}`
and cancels with `{"type":"heatmap_live_unsubscribe","symbol":..}`. Sources must be distinct supported ids. A valid re-subscribe replaces the subscription for that symbol (including its sources, id, and final markers); malformed requests leave it intact. A session holds at most eight raw-tail symbols. The live service has a separate cap of 128 symbol subscriptions, independent of the 64 legacy recording views. Subscribe refusals use a binary SHC1 `error` with SHE1 request id `sub`, an echoed symbol and empty source/start 0: `invalid_request`, `unavailable`, or `busy`. These subscriptions only observe series recorded by this process; they do not start an upstream recorder.

Each update is a separate SHE1/SHC1 kind `live_column` (2) frame per source. SHE1's request id is `sub`. Its SHC1 header has the same layout as kind 1 **with one additional signed i64 `tailStartMs` immediately after `endMs` and before the grid identity**; the content hash includes this field. The key uses level 60000 and the UTC hour containing the latest known live minute. `endMs` is that minute's exclusive end, not the hour end. The minute is inferred as the later of the committed cutoff and newest provisional start; before the next open publication at rollover, the newest provisional can still be the just-closed minute. The body is native 1m `SparseColumns` over `[tailStartMs,endMs)`, allowed to reach into at most the preceding hour-sized chunk. State is always open with the recorder's exclusive `committedThroughMs` (zero before any proven cutoff) and the nonzero `LiveCache` series revision. Cutoffs and extents are minute aligned and constrained to the supported recording timestamp range before arithmetic.

Within the byte budget described below, the body carries every retained provisional minute (up to 60 pending minutes plus the open minute), flagged `kProvisional`, and held committed finals whose start is at or after that subscriber's final marker. A final is before `committedThroughMs`; a provisional is at or after it. Column flags come directly from the recorder: an observed forming minute has `kPartial` and `observedMs` for time covered so far, while a complete lateness-held provisional can have `observedMs = 60000` without `kPartial`, including the newest column during rollover. A commit can precede the next open publication: its frame can contain finals without an observed open column. Each minute has one native grid constituent and the original size codes, with entries sorted by (row, side). `scannedRanges` is the coalesced union of transported minute records. It never claims that an omitted or evicted final was an empty recorder minute; such holes stay `NotLoaded` until a chunk scans them. Both encoder and decoder reject inconsistent flags, extents, states, scan claims, counts, grids, durations, or overflowing input. Existing SHC1 payload/envelope limits and hash/zstd validation also apply.

**Live rate.** The recorder publishes each layer's open minute at most once per `recording.live_publish_ms` of integration clock (default 500 ms, clamped to [250, 5000]; owner decision 2026-10-01), so live frames carry a new revision at 2 Hz. While a symbol's book is invalid (resnapshot loop, expired one-sided grace, upstream outage) nothing integrates, so the recorder publishes the open minute once and then only when an input changes (validity, observed time, flags, commit cutoff, minute); it never re-sends a byte-identical record. A failure while building an open-minute publication is logged (throttled) and skipped; it never invalidates a book. The live worker turns every ~100 ms and paces each subscription at half that interval (`liveCadenceMs`, 250 ms by default). It sends only a changed revision, so frames still follow publications (2 Hz). In steady state (provisional publications only, with no intervening final deliveries and no byte-budget backpressure) each publication goes out at the first worker turn after it; a refused admission backs the cadence off as described below. A cadence equal to the publish interval would, after turn rounding, fall behind the recorder, skip publications, and let the data age sawtooth through a whole interval. The same cadence applies to legacy `heatmap_recording_live` views.

The raw path uses `LiveService`'s existing worker and `LiveCache::snapshot()` only, with no disk reads and no recorder changes. It attempts changed revisions at the live cadence above. Entries, column storage, and compression scratch are reused; an encoding is shared across subscribers needing the same retained final suffix, and all subscribers with no pending finals share one encoding per (series, revision). Each session has an independent raw **1 MiB aggregate byte budget**, including SHE1 envelopes, separate from the legacy live slot and chunk reply budget. Multiple raw frames can be admitted together, preserving the live rate for both sources and multiple symbols. Queued raw frames retain FIFO order behind the current write, ahead of queued history; bytes remain charged through write completion or discard. Refusal coalesces to the newest snapshot and doubles the cadence from its base up to 5 seconds (0.25 -> 0.5 -> 1 -> 2 -> 4 -> 5 s by default). Only accepted admission advances that source's final marker, so a refused final accompanies the next accepted frame while it remains retained. Successful admission restores the base cadence. After budget pressure, attempts rotate across sources to prevent starvation. Empty builds consume the revision without delivery or congestion backoff. Allocation/build failures are logged and retried with backoff. Replaced/unsubscribed queued frames are discarded; an already in-flight frame can finish and consumers must correlate `sub`.

Oversized tails produce a throttled warning naming symbol, source, and bytes. The builder removes oldest pending minutes until the envelope fits, preserving the newest known minute. If finals also need trimming, it removes their newest suffix so only an included final prefix advances the marker. Omitted minutes are excluded from scan claims and recovered through chunks; trimmed finals are not marked delivered. An indivisible oversized minute is logged and omitted rather than refused forever. The encoding cache holds at most 17 variants per series/revision (16 possible first finals plus the no-finals variant). SHE1 construction and the queue's owning string still make two per-subscriber copies; the byte budget bounds admitted wire bytes, not these transient copies.

On reconnect the client subscribes after `connected()` plus fresh availability, passing its stored open chunk's `committedThroughMs` as `since_ms` (zero is allowed for a cold subscription). Finals with starts at or after that value are resent if still held: the cache retains at most 16 finals, additionally bounded by entry count. Older data is fetched as chunks. Series revision increases within one server process; a server restart can reset it. Revision is not a persistent identity, and a fresh subscription/connection must accept the reset.

The client routes kind 2 to `heatmapLiveReceived(sub, shared const ChunkFrame)` without `acceptChunkOrder`; it uses exactly the S3 FIFO decode pool, frame/byte admission bounds, refusal coalescing, and connection-epoch invalidation. `subscribeHeatmapLive` returns a unique id from the chunk request id namespace, and `unsubscribeHeatmapLive` cancels a symbol. `ChunkTransport`/`SentinelStreamClientTransport` expose `subscribeLive`, `unsubscribeLive`, and `liveReceived`; the adapter maps subscription ids and rejects events from replaced subscriptions, sources, hosts, and disconnected epochs. The recording-only local transport returns id 0 for unsupported live subscriptions. The fake transport can script/hold/release live replies.

**Client handover (S5L-b):** `ChunkFetcher` owns one `LiveEdge` per (symbol, source) on the heatmap-data thread. `wantLive(chart, symbol)` / `releaseLive(chart)` share one live subscription per symbol over its advertised minute sources. A minute comes from the chunk if its start is less than that chunk's stored `committedThroughMs`, otherwise from live, never both. A missing final stays `NotLoaded`, never zero; a provisional minute omitted by a newer frame loses its previous value. Live data stays outside `ChunkStore`; stored cutoffs trim immutable buffered minutes, and only the latest 120 minutes of live records and missing-minute markers are retained. A frame ahead of a wanted, stored open chunk triggers immediate `have_hash` revalidation, including a follow-up if a commit advances while a request is in flight. Live frames preserve chunk retry deadlines and terminal failures. Refused live subscriptions retry with exponential 100 ms..5 s backoff. Charts keep their subscription interest for three seconds after panning away from the live bucket; symbol changes and chart destruction release it immediately. Reconnect waits for connection plus fresh availability, sends the minimum stored open cutoff across sources as `since_ms`, ignores old subscription ids, and accepts a revision reset in the new epoch. The previous picture remains frozen while disconnected.

The per-chart controller publishes `LiveSnapshot` separately through thread-safe `latestLive()` and queued `liveChanged()`. Sources contain composed columns plus their CPU GPU-upload image over `[startMs,openEndMs)`; all sources use the lowest L of the live-edge builds that may still draw. Normally L advances only on the node's upload report. A dropped or explicitly missing drawable span no longer holds L; prefetch and recent-timeframe slots do not hold it either. If no drawn span still covers the previous L, it can advance to the committed edge floor, leaving the abandoned interval loading. Lag behind the newest live bucket floor is capped at `max(tf, min(2 hours, 64 * tf))` (plus the forming bucket itself); the one-bucket minimum preserves 4h/1D rollover until the previous bucket's span uploads. The cap also applies to stalled cutoffs and slower sibling sources. Exceeding that cap moves L to the newest edge floor; a source wholly before L contributes no live image. Upload acknowledgements are retained only for the current live window. `SpanSourceBuild::completeEndMs` exposes E for the node's clip. `TimeComposer::ComposeOptions::forming` and the production live composer include a terminal proven prefix with its observed duration, so a commit before the next open publication does not blank a coarse forming bucket. Known missing provisional minutes and never-received finals before the wire cutoff still keep that bucket unloaded. The build pool caches committed contributions by chunk generation and unchanged pending contributions by immutable column identity; ordinary live updates recompose one bucket. `latestResolution()` merges history and live for Auto without replacing the historical SpanSet pointer, deduplicates by column/source (loaded history wins), and retains history when live resets. Consumers must use the client-local `LiveSnapshot::version` for upload identity (a server revision can restart or stay the same across a local chunk handover), and retain snapshots with held/fading pictures. A live frame composes on arrival after a 15 ms coalescing window, at most once per `liveMinIntervalMs` (default 500 ms, which composes every 2 Hz server frame; a frame after a short gap waits at most the shortfall); a measured composition median over 5 ms backs off to 5 seconds, recovering to the minimum interval after one at or below 5 ms. Cadence deadlines start at job admission. A consistent completed job may publish if its chart serial and live state still match, even when newer input arrived; newer input coalesces until the next deadline. Unrelated history events do not invalidate live work. Ordinary live frames refresh only the live window, and publications update its CPU ledger without reconciling the span plan. The `heatmap.live.compose` probe reports cost, recomposition counts and cadence.

### Process-wide chunk fetching (S5a)

`heatmap::ChunkFetcher` and its `ChunkTransport` live on the heatmap-data event-loop thread; controllers queue `want(chartId, keys, priority)` and `release` calls there. One fetcher and one `ChunkStore` serve every chart. Incoming transport signals and outgoing controller notifications use queued connections. The transport wraps the existing `SentinelStreamClient` API without changing its decode or ordering rules. The local lab transport builds native chunks on its own worker using a read-only `Hmc2Reader`; it never takes the recording writer lock. Requests use the last advertised availability snapshot. Polls publish only changed snapshots (each connection still gets a fresh initial message); scan failures retain the last good snapshot and log a throttled warning, without failing requests. Shutdown requests cooperative cancellation through the reader's `ReadControl` before joining the worker.

The fetcher deduplicates in-flight and current cached keys, selects work by priority, then batches by `(symbol, source, levelMs)` within the wire limit. It admits at most four chunks and 16 MiB of estimated work. A byte-blocked key stops admission of lower-ranked keys, preserving capacity for visible work. Cold chunks estimate 4 MiB each; warm chunks use decoded bytes, capped at one full 16 MiB wire frame (one large chunk can still progress). This admission estimate is not a bound on actual decoded size. The decoded store separately enforces its configurable byte budget (512 MiB by default) for `put`; externally held shared pointers are outside LRU accounting. The legacy blocking lab API retains its existing keep-newest budget exception until S5c. `busy`, local decode overload, and request deadline expiry retry with exponential 100 ms..5 s backoff, reset on disconnect or cache clear. Each request has a 30 s deadline from admission (configurable, using the fetcher's monotonic clock); partial replies do not extend it. Completion/expiry retires adapter bookkeeping through `forget(requestId)`, and late replies are ignored. A request-level sentinel key that matches none of its pending keys applies to all remaining keys in that request. An unreadable request id retries keys not explicitly identified as bad; identified terminal errors fan out normally, and `superseded` retires silently. A rejected store insertion retries with the same backoff for at most five attempts, then emits `chunkFailed` with `store_rejected`. Successful insertion or reconnect resets the attempt count.

A disconnect loses every request but keeps chart interests and cached bodies. The client adapter treats a second `connected()` as a down/up transition because explicit client disconnect does not emit `disconnected()`. Requests resume only after connection plus fresh availability for their symbol. Wanted open chunks revalidate with `have_hash`; sealed chunks remain current. The fetcher retains the hash's shared body until its reply, even if the store evicts that body meanwhile. `NotModified` preserves the generation; changed bodies and first sealing advance it and broadcast a revision to all charts. Availability advances revalidate only wanted open chunks of the affected source/level; an advance during a request schedules a follow-up if that response still predates the new cutoff. A wire-version mismatch or host replacement clears the store and rejects old outstanding replies. The adapter's `setClient` reports host replacement; attach it before connecting/subscribing the client so no connection or availability notification is missed. A current cache hit in `want()` emits nothing, and eviction is not signalled; controllers peek the store and call `want()` again after a miss. The fetcher marks every key some chart wants as wanted in the store, which never evicts it (even a body larger than the whole budget) until every chart releases it, so a delivered body is never refetched while it is wanted.

### Live recording columns (S4)

A recording client registers one view per session after history confirms its exact band. Live projection uses that band without selecting another tick, and shares the history aggregator and grid-change/unknown-column rules:

```json
{"type":"heatmap_recording_view","symbol":"BTC-USD","layer":"deep","timeframe_ms":300000,"band_lo":50000,"band_tick":20,"band_rows":2048,"band_generation":7}
```

Registration replaces the previous view. Required layer is `near` or `deep`; tf is an
integer minute below 1h or integer hour thereafter, 1m through 1d inclusive (hours require
`deep`). Band lo must be nonnegative and tick-aligned, tick positive, rows 1..16384, and
upper bound <= 1e12. The native-row span is limited to 262144 during worker projection.
The server returns `error` with context `heatmap_recording_view`, `symbol`,
`band_generation`, `code`, `message`, and `retry_ms`. Codes include `invalid_request`,
`unavailable`, `capacity` (64 active views), `rate_limited` (one registration per 250 ms),
`incompatible_grid`, and `io_error`. Worker projection errors use the same envelope.
Clients retry the current confirmed view at the advertised delay, with exponential
1..30 s backoff, and drop stale-generation errors. Reconnect and every re-band must
register anew; generations identify the projection and are echoed unchanged.

A client that mutes its legacy band stream (the GPU heatmap renderer, S6b) releases
the view so the server stops projecting it:

```json
{"type":"heatmap_recording_unview","symbol":"BTC-USD"}
```

The server deactivates and drops the connection's view; there is no reply. Servers
that predate the message ignore it (the client drops the columns it receives).
Resuming the legacy stream registers a fresh view through the normal history path.

`heatmap_recording_live` has the heatmap family `schema_version`, `source:"recording"`,
`status:"complete"`, symbol, timeframe, layer, band, generation, size-code scale, and the
same `columns` array/encoding as recording history: little-endian u16 base64 `column` and
`liquidity_column`, `liquidity_scale`, packed LSB-first `validity`, `observed_ms`, and `flags`.
`value_encoding` is `absolute_log_size`. Zero quantity scale is valid for all-zero columns.
There are at most two columns (previous bucket, possibly still provisional, and current forming bucket). The
history envelope fields retained for decoder reuse do not prove scanned history: live
`scanned_start`/`scanned_end` are zero, request id is empty, and exhausted is false.

`flags & 32` means provisional: the column includes an open or finished-but-pending
minute, or the recorder's commit watermark has not yet reached the output bucket end.
Open-minute source values are valid-time TWAP, peaks, and bounds so far; a finished
pending minute is frozen. All provisional source minutes at/after the watermark remain
cached until commit. The watermark is publication-only, caps cold reader warmup, and
is never inferred from the newest open-minute timestamp. Both this bit and watermark
are publication-only metadata, never HMC2 disk fields. Existing partial/coverage/resync flags retain their meanings. A committed
minute replaces its provisional contribution, never adds to it. For tf > 1m, committed
minutes plus all pending/open provisional minutes are aggregated by each native row's covered duration,
then summed into display rows using the history projection math. Unknown time is excluded;
zero-observation source minutes are omitted. Live multi-hour columns aggregate minute codes
directly, so hourly-quantized history may differ by codec rounding at handover.

Delivery normally follows publication (every `recording.live_publish_ms`, 500 ms by default), coalesces under load, and backs off to 5 s
on budget/backpressure. It is latest-state streaming, not an event log. Committed minutes
are recoverable through history. The recorder callback, disk read/projection/encoding
worker, and network executor have separate ownership. Identical views share projection;
final projections are cached once and sent only to viewers whose per-subscription final
marker has not advanced past that bucket. New or congested viewers can still receive the
cached final. Encoding still echoes each subscriber's generation. Transient disk I/O errors
retain the shared projection and its committed prefix and retry with 2/4/5-second backoff;
they do not deactivate the view or trigger client re-registration. Live has one <=1 MiB in-flight slot
independent of the 16 MiB legacy/history budget (17 MiB combined bound), and is scheduled
immediately after the current write. Shutdown joins live delivery before stopping I/O.
Starting the stream server restarts live delivery; old subscriptions remain inactive and
clients register new ones. Recorder publications remain cached while transport is stopped.
Clients reject stale generations, preserve live columns while browsing history, advance
latest time monotonically, and keep matching live data ahead of delayed history responses.
After live moves past a cached provisional bucket, the client requests that single bucket
through history at most three times, with 2/4/8-second backoff (request timeouts or an
in-flight history request can delay the next attempt). A final with sufficient observed
duration replaces the provisional immediately. After exhaustion, the last valid disk value
wins even if shorter; without a disk value, the bucket becomes unknown. Repair then advances
to the next old provisional. Delayed provisionals cannot reopen a settled repair, while an
actual live final can still correct it. Attempt state is bounded by the column cache and
reset with the projection generation. This repairs finals missed through prolonged transport
congestion without an unbounded server replay queue or a permanent oldest-bucket retry loop.

## Independent trade-overlay publication

Footprint, TPO and volume profile no longer run from `heatmap_slice` callbacks.
A session timer dispatches at most one overlay job per second, with one job in
flight per session, up to eight pending history requests and sixteen overlay
symbols. Live symbols rotate; pending history alternates with live work so neither
can monopolize a session. Jobs use the bounded history pool (two workers, eight
jobs globally). Trade snapshotting, sorting, aggregation and wire encoding run
there, never on the network executor or recording live callback. Only immutable
bounded replies return to the session executor. Closing/unsubscribing discards
late replies, and selection generations prevent obsolete grids from publishing.

Each job copies at most 250,000 trades inside its requested half-open time window
and produces at most 8 MiB. Trades outside that window do not consume the budget;
only matching trades are sorted. An oversized window fails explicitly instead of
producing a partial profile. History
pages contain at most 512 columns. Saturation rejects history requests with a
`trade_overlay` error; live refreshes coalesce until the next timer turn. A live
refresh sends the forming bucket and, when a boundary was crossed, the preceding
closed bucket. Long stalls do not enqueue a bucket backlog; older ranges use
history requests. Footprint uses UTC epoch-aligned half-open buckets; TPO letters
and brackets align to its own session open. TPO history ends at session close,
and live TPO never publishes periods outside the session. VP aggregates the current
TPO session, or the UTC day for W1/M1.

The existing `footprint_slice`, `tpo_slice`, `volume_profile_slice`, and history
chunk wire families retain self-describing grid metadata. Overlay history requests
may additionally specify `price_min`, `tick_size`, and `rows`; defaults come from
`trade_overlays`, independently of heatmap settings. History and live publications
share the selected overlay grid. Weekly (`session_type` 5) and monthly (6, the
UTC calendar month) TPO columns use the same rows at a 5x / 10x coarser tick,
centred on that grid and aligned to their own tick (`tpoGridFor`), so a whole
week or month fits; footprint and VP keep the base grid. The GUI maps footprint
time slots, TPO session columns and VP prices through their own metadata and the
common viewport. Clients page W1/M1 TPO history in windows of at most 7 days.
W1 is the crypto week: 7 days from Monday 00:00 UTC. `tpo_history_request`
requires a unique `request_id` string (1-64 characters; a request without one is
rejected). The server echoes it on the `tpo_history_chunk` and on `trade_overlay`
errors for that request. The GUI keeps one page in flight and accepts only the
chunk carrying that page's id; any other chunk is dropped before a slice is
emitted. `tpo_history_cancel` (`symbol`, `request_id`) drops a queued page or
stops a running one between REST calls; a cancelled page gets no reply. The GUI
sends it for pages it abandons (45 s timeout) or supersedes (new selection).
TPO candle fallback uses the coarsest Coinbase granularity that divides the TPO
period and is aligned with the session open (for example `THIRTY_MINUTE` for
30-minute periods, so a 7-day page is one REST call); every candle lies inside
one period, so each period's range is exactly the high/low of its candles.
The live volume profile follows the TPO session when that session is 24 hours or
shorter (NY, London, Asia, Australia, H24); for W1 and M1 it covers the current
UTC day, because the live trade tape is retained for at most 24 hours.

Live overlays and footprint history derive from the retained server trade tape.
TPO history additionally fills price rows from REST minute-candle high/low ranges
before the oldest retained trade, including its partially retained minute. With
no retained tape (for example after restart), candles cover the entire requested
history window. Fetches run on the history worker, in pages of at most 350 minutes;
fetch errors fail the request explicitly. Every REST call has one 10-second total
network deadline covering DNS, connect, TLS and HTTP, including any auth fallback.
Timeouts return a failed result and log a warning. Overlay jobs check cancellation
before and after each page; shutdown, unsubscribe and selection changes prevent
additional page fetches. An in-flight page may use the remaining request deadline. Candle history uses
the same joined history pool, so server shutdown cannot destroy the REST client
while a fetch is running. Successful DNS endpoints are fresh for five minutes
and remain usable while a shared refresh is pending; an uncached request waits
for resolver capacity within its own deadline.
Candle ranges and retained trades map
onto the same independent overlay grid, anchored from a candle close when no
trade is available. This preserves session history without introducing persisted
trade-history storage. Footprint and VP still have no observations outside the
retained tape; an empty profile does not establish complete historical coverage.
