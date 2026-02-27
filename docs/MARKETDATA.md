# Market Data Architecture

## Overview

The market data module features a high-performance, thread-safe pipeline for processing real-time WebSocket data feeds. The design emphasizes a clean separation of concerns, with a pure C++ core engine and a distinct adapter for Qt GUI integration.

- **`MarketDataCoreEngine`**: The heart of the pipeline. A pure C++, non-GUI component that manages the WebSocket connection, authentication, message parsing, and data dispatching. It operates on a dedicated worker thread and uses `std::function` callbacks to emit data, making it reusable in any context (GUI, server, CLI).

- **`MarketDataCoreQt`**: A thin Qt adapter that wraps `MarketDataCoreEngine`. It consumes the engine's callbacks on the worker thread and uses `Qt::QueuedConnection` to safely emit Qt signals to the GUI thread.

---

## High-Level Architecture

The architecture clearly separates the I/O worker thread from the GUI thread.

```
┌─────────────────────────────────────────────────────────────────────────┐
│                          Application Layer                            │
│                                                                         │
│  ┌──────────────────────────┐         ┌───────────────────────────────┐ │
│  │   MarketDataCoreQt       │         │      MarketDataCoreEngine     │ │
│  │    (GUI Thread)          │◀───┐    │       (Worker Thread)         │ │
│  │                          │    │    │                               │ │
│  │ Signals:                 │    │    │ Callbacks:                    │ │
│  │  • tradeReceived()       │    │    │  • onTrade()                  │ │
│  │  • bookUpdates()         │    │    │  • onLiveOrderBook...()       │ │
│  │  • connectionStatus()    │    │    │  • onConnectionStatus()       │ │
│  │                          │    │    │  • onError()                  │ │
│  └──────────────────────────┘    │    └───────────────────────────────┘ │
│                                  │                                      │
│                Cross-Thread Communication (Qt::QueuedConnection)        │
│                                                                         │
└─────────────────────────────────────────────────────────────────────────┘
```

---

## Layered Component Architecture

The data pipeline consists of three primary layers within the `MarketDataCoreEngine`.

```
┌────────────────────────────────────────────────────────────────┐
│                     DATA FLOW PIPELINE                         │
└────────────────────────────────────────────────────────────────┘

┌─────────────┐      ┌──────────────┐      ┌─────────────┐
│  Exchange   │─────▶│   Transport  │─────▶│   Dispatch  │
│  WebSocket  │      │    Layer     │      │   Layer     │
└─────────────┘      └──────────────┘      └─────────────┘
                            │                      │
                            │                      │
                            ▼                      ▼
                     ┌──────────────┐      ┌─────────────┐
                     │     Auth     │      │  Callbacks  │
                     │   Manager    │      │  (std::function)
                     └──────────────┘      └─────────────┘
```
The engine is now stateless regarding historical data. Consumers of the engine are responsible for caching and state management.

---

## Component Details

### 1. Transport Layer (`ws/`)

#### **WsTransport** (Abstract Interface)
- **Purpose**: A pure virtual interface for WebSocket communication, decoupled from any specific implementation.
- **Responsibilities**: Connection lifecycle, message transport, and status/error reporting via callbacks.

#### **BeastWsTransport** (Concrete Implementation)
- **Technology**: Boost.Beast WebSocket over SSL, running on a Boost.Asio `io_context`.
- **Threading**: All operations are executed on a Boost.Asio `strand` to ensure serial access to the WebSocket stream, guaranteeing thread safety without explicit locks.
- **Features**: Asynchronous operations, automatic reconnection with exponential backoff, and a keep-alive ping timer.

### 2. Authentication Layer (`auth/`)

#### **Authenticator**
- **Purpose**: Creates signed JSON Web Tokens (JWTs) for the Coinbase Advanced Trade API using the ES256 algorithm.
- **Thread Safety**: `createJwt()` is stateless and thread-safe.
- **Error Handling**: JWT creation is wrapped in `try/catch` blocks to prevent exceptions from crashing the I/O thread, a critical resilience improvement.

### 3. Dispatch Layer (`dispatch/`)

#### **MessageDispatcher**
- **Purpose**: Parses incoming JSON messages from the WebSocket and normalizes them into strongly-typed event structs (`TradeEvent`, `BookSnapshotEvent`, etc.).
- **Implementation**: A static `parse` method provides a stateless, functional approach to message processing.

**Parsing Flow**:
```
JSON Message ─────┬─→ channel="market_trades" ─→ TradeEvent[]
                  │
                  ├─→ channel="l2_data" ───────┬─→ type="snapshot" ─→ BookSnapshotEvent
                  │                             └─→ type="update" ───→ BookUpdateEvent
                  │
                  └─→ (other channels/types)
```

### 4. Callback Emitters
- **Purpose**: The `MarketDataCoreEngine` uses `std::function` members to provide data to its owner.
- **Callbacks**:
  ```cpp
  using TradeCb = std::function<void(const Trade&)>;
  using OrderBookLevelUpdatesCb = std::function<void(...)>;
  using OrderBookInitializedCb = std::function<void(...)>;
  using ConnectionStatusCb = std::function<void(bool)>;
  using ErrorCb = std::function<void(const std::string&)>;
  ```
- **Integration**: The client (e.g., `MarketDataCoreQt`) provides implementations for these callbacks to receive data.

### 5. Data Models (`model/`)

The `model/` directory contains the core data structures used throughout the pipeline.

#### **Trade**
- Represents a single market trade, containing `product_id`, `price`, `size`, `side`, and `timestamp`.

#### **OrderBook-related Structs**
- `OrderBookLevel`: A simple `price`/`size` pair.
- `BookLevelUpdate`: Represents an incremental update to the order book (`isBid`, `price`, `quantity`).
- `OrderBook`: A sparse representation used for initial snapshots.

#### **LiveOrderBook**
The `LiveOrderBook` implements a **dense, fixed-range order book** optimized for high-performance visualization and GPU rendering.
- **Key Feature**: It maps a continuous price range to a `std::vector`, allowing for O(1) updates of price levels.
- **Management**: This class is a data structure provided to consumers. The `MarketDataCoreEngine` **does not** manage `LiveOrderBook` instances itself; it only provides the raw snapshot and update data required to maintain one. The consumer is responsible for instantiating and updating it.

---

## Threading Model

### Worker Thread (Boost.Asio)
The `MarketDataCoreEngine` spawns a dedicated `m_ioThread` that runs `m_ioc.run()`. This thread handles all network I/O, message parsing, and callback invocation. A `while (m_running)` loop with `try/catch` ensures the thread and `io_context` are resilient to exceptions thrown by handlers.

### GUI Thread (Qt)
The `MarketDataCoreQt` object lives on the GUI thread. It receives data from the engine's callbacks (which execute on the worker thread) and safely transfers it to the GUI thread by emitting signals via `Qt::QueuedConnection`.

### Cross-Thread Communication
```
 Worker Thread (MarketDataCoreEngine)      GUI Thread (MarketDataCoreQt)
      │                                          │
 1. Parse trade, invoke callback               │
    m_onTrade(trade)                             │
      │                                          │
      └───────invokes lambda set by adapter──────┼──→ 2. Lambda is invoked on worker thread.
               (captures QPointer<self>)         │      It calls QMetaObject::invokeMethod()
                                                 │      with Qt::QueuedConnection.
                                                 │
                                                 ▼
                                           3. Qt Event Loop processes the event
                                              and emits tradeReceived(trade)
                                              safely on the GUI thread.
                                                 │
                                                 ▼
                                           4. UI components update.
```
This model ensures a clean separation, preventing the core engine from having any dependency on Qt while providing a safe, idiomatic integration pattern for the GUI.

---

## Message Processing Flow

### Trade Message Flow
```
1. WebSocket receives JSON.
        │
        ▼
2. BeastWsTransport::onRead() passes payload to engine.
        │
        ▼
3. MarketDataCoreEngine::dispatch() calls MessageDispatcher::parse().
        │
        ▼
4. A Trade is created from the parsed data.
        │
        ▼
5. m_onTrade(trade) callback is invoked.
        │
        ▼
6. MarketDataCoreQt's handler queues a signal emission to the GUI thread.
```

### Order Book Flow
```
1. WebSocket receives `l2_data` message.
        │
        ▼
2. MessageDispatcher identifies `BookSnapshotEvent` or `BookUpdateEvent`.
        │
        ▼
3. MarketDataCoreEngine calls the appropriate handler:
   │
   ├─→ [Snapshot] handleOrderBookSnapshot()
   │   └─→ Invokes m_onLiveOrderBookInitialized() with bid/ask vectors.
   │
   └─→ [Update] handleOrderBookUpdate()
       └─→ Invokes m_onLiveOrderBookLevelUpdates() with incremental changes.
        │
        ▼
4. MarketDataCoreQt receives the data and emits signals for the GUI.
```

---

## File Organization

```
libs/core/marketdata/
├── MarketDataCoreEngine.hpp   # Main orchestrator (pure C++)
├── MarketDataCoreEngine.cpp
│
├── ws/                        # WebSocket transport layer
│   ├── WsTransport.hpp        # Abstract transport interface
│   ├── BeastWsTransport.hpp   # Boost.Beast implementation
│   ├── BeastWsTransport.cpp
│   └── SubscriptionManager.hpp
│
├── auth/                      # Authentication layer
│   ├── Authenticator.hpp
│   └── Authenticator.cpp
│
├── dispatch/                  # Message parsing layer
│   ├── MessageDispatcher.hpp  # JSON → Event parser
│   └── Channels.hpp
│
└── model/                     # Data models
    ├── TradeData.h            # Trade, OrderBook, LiveOrderBook, etc.
    └── LiveOrderBook.cpp

libs/gui/marketdata/
├── MarketDataCoreQt.hpp       # Qt adapter for the engine
└── MarketDataCoreQt.cpp
```

---

## Key Design Decisions

### 1. **Why was the Cache Layer removed?**
The original `MarketDataCore` included a `DataCache`. This was removed to improve separation of concerns and reusability. By emitting data via callbacks, `MarketDataCoreEngine` is now a stateless processor. Consumers can implement any caching strategy they need (or none at all). This allows the same engine to be used in the GUI (which needs caches for visualization) and the `sentinel-server` (which might have different caching or forwarding logic).

### 2. **Why a separate `MarketDataCoreQt` adapter?**
To keep the core C++ library free of Qt dependencies. `MarketDataCoreEngine` can be used in any C++ project, while `MarketDataCoreQt` provides a clean, idiomatic bridge to the Qt world, handling cross-thread communication safely.

### 3. **Why strand instead of mutex in transport?**
A `strand` serializes asynchronous operations within the `io_context`, preventing concurrent access to the WebSocket without blocking the I/O thread. Using a `mutex` would introduce blocking calls and defeat the purpose of an asynchronous networking model.

---

## Conclusion

The refactored market data architecture achieves:
- **Decoupling**: The core engine is fully independent of the GUI and caching logic.
- **Reusability**: `MarketDataCoreEngine` can be used for servers, CLIs, or other applications.
- **Low Latency**: Sub-millisecond message processing.
- **Thread Safety**: Clean boundaries and safe cross-thread communication patterns.
- **Maintainability**: Clear separation of concerns makes the system easier to understand and extend.
- **Reliability**: Resilient I/O thread and robust error handling.



## Trading stream additions (paper mode)

The stream protocol now includes a minimal paper-trading vertical slice:

- `trade_command` (client -> server): `PLACE_ORDER`, `CANCEL_ORDER`, `CANCEL_ALL`, `FLATTEN`.
- `order_update` (server -> client): authoritative order lifecycle updates.
- `position_update` (server -> client): authoritative per-symbol position and unrealized PnL.

Server-side fills are generated by the paper execution adapter using the latest trade price plus optional `trading.slippage_bps` from `server_config.yaml`.
