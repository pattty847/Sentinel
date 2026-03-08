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

- **MessageDispatcher** — Parses JSON from the WebSocket into typed events (`TradeEvent`, `BookSnapshotEvent`, `BookUpdateEvent`, etc.). Stateless `parse` entry point.

**Flow:** `channel=market_trades` → `TradeEvent[]`; `channel=l2_data` + `type=snapshot` → `BookSnapshotEvent`; `type=update` → `BookUpdateEvent`.

### 4. Callbacks

The engine exposes `std::function` callbacks (e.g. `TradeCb`, `OrderBookLevelUpdatesCb`, `OrderBookInitializedCb`, `ConnectionStatusCb`, `ErrorCb`). The owner (e.g. `MarketDataCoreQt`) supplies implementations and forwards to the GUI thread via `QMetaObject::invokeMethod(..., Qt::QueuedConnection)`.

### 5. Data models (`model/`)

- **Trade** — `product_id`, `price`, `size`, `side`, `timestamp`.
- **OrderBookLevel**, **BookLevelUpdate**, **OrderBook** — Snapshot and incremental update structures.
- **LiveOrderBook** — Dense, fixed-range order book for visualization and GPU. Maps a continuous price range to a vector for O(1) updates. The engine does **not** own `LiveOrderBook` instances; it only delivers snapshot and update data. The consumer creates and updates the book.

## Threading

- **Worker thread (Boost.Asio)** — `MarketDataCoreEngine` runs `m_ioc.run()` on a dedicated thread. All network I/O, parsing, and callback invocation happen there. A `while (m_running)` loop with try/catch keeps the thread and `io_context` resilient to handler exceptions.
- **GUI thread (Qt)** — `MarketDataCoreQt` lives on the GUI thread. When a callback fires on the worker thread, the adapter uses `Qt::QueuedConnection` to emit the corresponding signal on the GUI thread so UI updates stay safe.

## Message flow

**Trades:** WebSocket → BeastWsTransport::onRead() → engine → MessageDispatcher::parse() → Trade → `m_onTrade(trade)` → adapter queues signal → GUI thread emits `tradeReceived(trade)`.

**Order book:** WebSocket → parse → `BookSnapshotEvent` or `BookUpdateEvent` → `handleOrderBookSnapshot()` or `handleOrderBookUpdate()` → `m_onLiveOrderBookInitialized()` or `m_onLiveOrderBookLevelUpdates()` → adapter queues signals → GUI updates.

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
