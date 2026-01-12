# Sentinel Client-Server Architecture

This document details the complete refactor of Sentinel from a monolithic GUI application into a distributed client-server architecture. This transformation enables 24/7 data collection, eliminates GUI performance bottlenecks, and provides scalable real-time market data streaming.

## Architecture Overview

### Transformation Summary

**Before (Monolithic)**:
```
Exchange → MarketDataCore → DataCache → Qt Signals → DataProcessor → Renderer
```

**After (Distributed)**:
```
Server Process:
Exchange → MarketDataCore → ServerDataModel → SentinelStreamServer → WebSocket

Client Process:
WebSocket → SentinelStreamClient → RemoteGridDataSource → DataProcessor → Renderer
```

### Core Benefits

- **Continuous Data Collection**: Server runs 24/7 without GUI dependencies
- **Performance Isolation**: Heavy data processing separated from UI rendering
- **Crash Resilience**: GUI crashes don't interrupt data collection
- **Scalability**: Multiple clients can connect to single data source
- **Storage Integration**: Persistent historical data with efficient retrieval

---

# CLIENT

## Overview

The client component (`sentinel-gui`) is a lightweight Qt6/QML application focused purely on visualization and user interaction. It connects to the server via WebSocket to receive pre-processed market data streams.

## Core Architecture

### Data Flow
```
SentinelStreamClient → RemoteGridDataSource → DataProcessor → LiquidityTimeSeriesEngine → UnifiedGridRenderer
```

### Key Components

#### 1. Data Source Abstraction (`libs/gui/datasources/`)

**IGridDataSource.hpp**
- Abstract interface defining data access contract
- Provides symbol subscription and data retrieval methods
- Enables seamless switching between local and remote data sources

```cpp
class IGridDataSource {
public:
    virtual GridDataSnapshot currentSnapshot(const QString& symbol, Timeframe tf) = 0;
    virtual void subscribe(const QString& symbol, Timeframe tf) = 0;
signals:
    void slicesUpdated(const QString& symbol, Timeframe tf);
};
```

**LocalGridDataSource.hpp/.cpp**
- Wraps existing DataCache for monolithic mode compatibility
- Preserves existing behavior during migration
- Acts as fallback when server is unavailable

**RemoteGridDataSource.hpp/.cpp**
- Implements remote data access via SentinelStreamClient
- Manages local caching of received data slices
- Handles subscription state and reconnection logic

#### 2. Network Communication (`libs/core/protocol/`)

**SentinelStreamClient.hpp/.cpp**
- WebSocket client implementation using Boost.Beast
- Manages connection lifecycle (connect, authenticate, subscribe)
- Handles message parsing and protocol state machine
- Provides automatic reconnection with exponential backoff

Key responsibilities:
- Connection management to server
- JSON/Binary protocol handling
- Message queue management
- Error handling and recovery

#### 3. Integration Layer

**DataProcessor Updates**
- Modified to accept IGridDataSource interface instead of direct DataCache access
- Maintains existing API surface for seamless integration
- Supports both local and remote data sources

**LiquidityTimeSeriesEngine Enhancement**
- New method: `addAggregatedSlice(const LiquidityTimeSlice&)`
- Bypasses internal aggregation when receiving pre-processed data
- Maintains compatibility with existing rendering pipeline

### Configuration

**Runtime Mode Selection**
- Environment variable: `SENTINEL_REMOTE=1` enables remote mode
- Default behavior maintains monolithic operation
- Configuration stored in application settings

### Threading Model

- **Main Thread**: Qt GUI, event handling, rendering coordination
- **Network Thread**: WebSocket I/O, message processing
- **Data Thread**: DataProcessor operations, cache management

Communication between threads uses Qt's queued connections to ensure thread safety.

---

# SERVER

## Overview

The server component (`sentinel-server`) is a headless daemon designed for continuous operation. It ingests market data, performs aggregation, manages persistent storage, and streams processed data to connected clients.

## Core Architecture

### Data Flow
```
Exchange → MarketDataCore → ServerDataModel → Storage + SentinelStreamServer
```

### Key Components

#### 1. Server Data Model (`libs/core/servermodel/`)

**ServerDataModel.hpp/.cpp**
- Central data management hub for all market symbols
- Maintains in-memory state and coordinates persistence
- Thread-safe access to live order books and historical data

**SymbolHotData.hpp**
```cpp
struct SymbolHotData {
    std::string symbol;
    LiveOrderBook liveBook;                              // Current market state
    RingBuffer<TickSnapshot, N_TICKS> rawTicks;          // Short-term tick history
    std::map<int64_t, RingBuffer<AggregatedSlice, N_SLICES>> aggByResolution; // 100ms, 1s, 5s, 1m
};
```

Key responsibilities:
- Symbol lifecycle management
- Live order book maintenance
- Raw tick buffering
- Aggregated slice caching
- Storage coordination

#### 2. Persistence Layer

**TickBinaryLogger.hpp/.cpp**
- Append-only binary logging of raw market data
- Segment-based storage with atomic commits
- Configurable rotation policies (time/size based)

**TimeframeAggregator.hpp/.cpp**
- Timer-driven aggregation pipeline
- Converts raw ticks to multiple timeframes (100ms → 1s → 5s → 1m)
- Manages aggregation windows and rollup logic

**StorageLayout.hpp**
- Defines binary format structures
- File organization and naming conventions
- Metadata schemas for SQLite integration

#### 3. Storage Format

**Directory Structure**:
```
data/
  sentinel.db                    # SQLite metadata
  market/
    BTC-USD/
      ticks/
        2025-12-08_00.bin       # Hourly raw tick segments
        2025-12-08_01.bin
      agg_100ms/                # Pre-aggregated timeframes
      agg_1s/
      agg_5s/
      agg_1m/
```

**Binary Formats**:
```cpp
struct SegmentHeader {
    uint32_t magic = 0x53454E54;     // "SENT"
    uint16_t version = 1;
    int64_t start_timestamp_ms;
    int64_t end_timestamp_ms;
    uint32_t tick_count;
};

struct TickSnapshotRecord {
    int64_t timestamp_ms;
    uint16_t bid_count;
    uint16_t ask_count;
    // Followed by bid_count + ask_count TickLevel entries
};

struct AggregatedSlice {
    int64_t start_ts_ms;
    int64_t end_ts_ms;
    float total_bid_volume;
    float total_ask_volume;
    // Optional: compressed price-level volume distribution
};
```

**SQLite Schema**:
```sql
CREATE TABLE symbols (
    id INTEGER PRIMARY KEY,
    symbol TEXT UNIQUE NOT NULL,
    exchange TEXT NOT NULL,
    tick_size REAL NOT NULL,
    min_price REAL,
    max_price REAL
);

CREATE TABLE segments (
    id INTEGER PRIMARY KEY,
    symbol_id INTEGER REFERENCES symbols(id),
    resolution_ms INTEGER NOT NULL,      -- 0=raw, 100, 1000, 5000, etc.
    start_time_ms INTEGER NOT NULL,
    end_time_ms INTEGER NOT NULL,
    file_path TEXT NOT NULL,
    byte_offset INTEGER,
    byte_length INTEGER,
    record_count INTEGER
);
```

#### 4. Network Server (`libs/core/protocol/`)

**SentinelStreamServer.hpp/.cpp**
- Multi-client WebSocket server using Boost.Beast
- Session management with per-client state
- Protocol version negotiation
- Rate limiting and connection management

**SentinelSession**
- Individual client connection handler
- Subscription management per client
- Message routing and formatting
- Error handling and graceful disconnection

#### 5. Application Entry Point (`apps/sentinel-server/`)

**main.cpp**
- Command-line argument parsing
- Service configuration
- Signal handling for graceful shutdown

**SentinelServerApp.hpp/.cpp**
- Application lifecycle management
- Component initialization and wiring
- Market data integration
- Server startup and shutdown sequences

### Startup and Warmup Process

1. **Initialization Phase**:
   - Load SQLite metadata and symbol configurations
   - Initialize ServerDataModel with symbol registry
   - Start storage subsystems (logger, aggregator)

2. **Warmup Phase**:
   - Load recent segments from disk into RAM ring buffers
   - Reconstruct latest LiveOrderBook states
   - Prepare aggregated slice caches

3. **Operational Phase**:
   - Connect to exchange feeds via MarketDataCore
   - Start WebSocket server for client connections
   - Begin continuous data collection and aggregation

### Threading Model

- **Main Thread**: Application coordination, signal handling
- **Market Data Thread**: Exchange connection, data dispatch
- **Aggregation Thread**: Timeframe processing, storage operations
- **Network Thread Pool**: WebSocket I/O, client session management

---

# CLIENT-SERVER

## Communication Protocol

### Connection Lifecycle

1. **Connection Establishment**
   - Client initiates WebSocket connection to server
   - TCP handshake followed by WebSocket upgrade
   - Authentication and capability negotiation

2. **Subscription Handshake**
   - Client sends subscription requests for symbols/timeframes
   - Server validates and acknowledges subscriptions
   - Server sends initial data snapshot

3. **Live Streaming**
   - Server pushes incremental updates as new data arrives
   - Client processes and integrates updates into local cache
   - Periodic heartbeat/ping messages maintain connection

4. **Disconnection and Recovery**
   - Client handles graceful and ungraceful disconnections
   - Automatic reconnection with exponential backoff
   - Re-subscription and catch-up data requests

### Message Protocol

**JSON Protocol (v0 - Current)**

Client → Server:
```json
{
  "type": "subscribe",
  "symbol": "BTC-USD",
  "timeframe_ms": 1000,
  "lookback_ms": 1800000
}
```

Server → Client:
```json
{
  "type": "snapshot",
  "symbol": "BTC-USD",
  "timeframe_ms": 1000,
  "start_ts_ms": 1733650000000,
  "slices": [...]
}

{
  "type": "slice_batch",
  "symbol": "BTC-USD",
  "timeframe_ms": 1000,
  "slices": [...]
}
```

**Binary Protocol (v1 - Planned)**
- Compact message framing with headers
- Efficient serialization of slice data
- Sequence numbering for gap detection
- Compression support for high-frequency data

### Data Consistency

**Synchronization Mechanisms**:
- Sequence numbers prevent message reordering
- Client-side buffering handles network latency
- Server maintains session state for reliable delivery

**Error Handling**:
- Connection timeouts trigger automatic reconnection
- Invalid messages logged and ignored
- Data gaps detected via sequence number validation

### Performance Characteristics

**Latency Optimization**:
- Direct WebSocket streaming bypasses Qt signal overhead
- Pre-aggregated data reduces client-side processing
- Efficient binary encoding minimizes network bandwidth

**Throughput Scaling**:
- Server maintains single data source for multiple clients
- Client-side caching reduces redundant requests
- Configurable batch sizes optimize network utilization

## Migration Strategy

### Phase Implementation Status

**Phase 0: Abstractions (✓ Completed)**
- Implemented IGridDataSource interface
- Created LocalGridDataSource wrapper
- Refactored DataProcessor integration

**Phase 1: Server Skeleton (✓ Completed)**
- Built sentinel-server application
- Implemented ServerDataModel
- Created SentinelStreamServer with Boost.Beast

**Phase 2: Client Remote Mode (✓ Completed)**
- Implemented SentinelStreamClient
- Created RemoteGridDataSource
- Added runtime toggle (SENTINEL_REMOTE=1)
- Established subscription handshake

**Phase 3: Persistence (Next)**
- Implement TickBinaryLogger
- Create TimeframeAggregator
- Add SQLite metadata management
- Enable server warmup from disk

**Phase 4: Optimization (Future)**
- Binary protocol implementation
- Performance tuning and stress testing
- Production deployment preparation

### Deployment Considerations

**Development Mode**:
- Both processes run locally
- Easy debugging and iteration
- Monolithic fallback available

**Production Mode**:
- Server deployed on dedicated infrastructure
- Multiple client connections supported
- Monitoring and alerting integration

**Hybrid Operation**:
- Gradual client migration
- A/B testing between modes
- Performance comparison and validation