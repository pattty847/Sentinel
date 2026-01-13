---
name: Sentinel Client/Server Architecture Plan
overview: ""
todos:
  - id: 99a49d89-b215-4012-a32b-4d388f77d165
    content: Create sentinel_server app target with CMake and basic main.cpp
    status: pending
  - id: eaf985e5-328c-456e-becf-95e9c37735cf
    content: Implement WebSocket server using Boost.Beast for client connections
    status: pending
  - id: e534193e-e44d-4b06-98bc-09444ead05e1
    content: Define binary protocol messages (Subscribe, Snapshot, TickDelta, etc.)
    status: pending
  - id: 1682cbbf-f76f-4472-8e9e-b08debc387aa
    content: Move MarketDataCore ownership from GUI to server process
    status: pending
  - id: 71a31e57-6411-43df-9887-b7b01806adc6
    content: Create ServerConnection class in GUI to consume from server WebSocket
    status: pending
  - id: a8469ada-b461-4c03-a20c-df89b9743b28
    content: Add SQLite for metadata/segment index with schema
    status: pending
  - id: b9d90470-1d1f-4971-8867-3f2a808dd461
    content: Implement binary segment writer for 100ms tick persistence
    status: pending
  - id: e1f81d6f-cc0f-4f60-a0f9-9ad431841683
    content: Implement segment reader for warm start and snapshot queries
    status: pending
  - id: af68ab9e-f408-4466-90e0-3b6389ae5034
    content: Server-side snapshot request handler for historical data
    status: pending
  - id: 93597e01-d4da-438e-9b44-92283c6398b8
    content: Background rollup jobs for multi-timeframe aggregation
    status: pending
  - id: 6c15e44a-cb44-4a86-9974-eb1651beff3e
    content: Extract IExchangeAdapter interface, refactor Coinbase implementation
    status: pending
---

# Sentinel Client/Server Architecture Plan

## A. High-Level Architecture

### Process Layout

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                           SENTINEL SERVER (headless)                         │
│  ┌─────────────┐   ┌──────────────┐   ┌─────────────┐   ┌───────────────┐  │
│  │  Exchange   │──▶│  Ingestion   │──▶│ Aggregation │──▶│   Storage     │  │
│  │  Adapters   │   │  Pipeline    │   │   Engine    │   │  (SQLite +    │  │
│  │ (Coinbase)  │   │              │   │  (LTSE)     │   │  Binary Segs) │  │
│  └─────────────┘   └──────────────┘   └─────────────┘   └───────────────┘  │
│                                              │                    │         │
│                                              ▼                    ▼         │
│                                     ┌─────────────────────────────────┐     │
│                                     │      Client Streamer            │     │
│                                     │  (WebSocket + Binary Protocol)  │     │
│                                     └─────────────────────────────────┘     │
└─────────────────────────────────────────────────────────────────────────────┘
                                              │
                          ┌───────────────────┼───────────────────┐
                          ▼                   ▼                   ▼
                    ┌──────────┐        ┌──────────┐        ┌──────────┐
                    │ Client 1 │        │ Client 2 │        │ Client N │
                    │  (Qt)    │        │  (Qt)    │        │  (future)│
                    └──────────┘        └──────────┘        └──────────┘
```

### Responsibility Boundaries

| Component | Server | Client |

|-----------|--------|--------|

| Exchange WebSocket | ✓ | |

| Authentication (JWT) | ✓ | |

| Order book maintenance | ✓ | |

| Trade/book normalization | ✓ | |

| Multi-TF aggregation | ✓ | |

| Persistence to disk | ✓ | |

| Historical replay | ✓ | |

| Streaming to clients | ✓ | |

| Subscribe to symbols | | ✓ (request) |

| Request snapshots | | ✓ |

| Render pipeline | | ✓ |

| User interaction | | ✓ |

---

## B. Database and Storage Strategy

### Recommendation: SQLite + Custom Binary Segments (Hybrid)

**Why this choice for Sentinel:**

- Runs perfectly on Raspberry Pi 5 (ARM64, low memory)
- Zero ops burden (no daemon, no config)
- Binary segments give you raw speed for the hot path
- SQLite handles metadata, indexes, and complex queries
- Clear upgrade path to QuestDB/TimescaleDB when you hit scale

### Storage Layout

```
data/
├── sentinel.db              # SQLite: metadata, symbol config, session info
├── market/
│   ├── BTC-USD/
│   │   ├── ticks/           # Raw 100ms snapshots (binary segments)
│   │   │   ├── 2024-12-07_00.bin
│   │   │   ├── 2024-12-07_01.bin
│   │   │   └── ...
│   │   ├── agg_1s/          # Pre-rolled 1s aggregates
│   │   ├── agg_5s/          # Pre-rolled 5s aggregates
│   │   └── agg_1m/          # Pre-rolled 1min aggregates
│   └── ETH-USD/
│       └── ...
└── index/
    └── segment_index.db     # SQLite: segment timestamps, byte offsets
```

### Binary Segment Format (100ms ticks)

```cpp
// Header (32 bytes)
struct SegmentHeader {
    uint32_t magic = 0x53454E54;  // "SENT"
    uint16_t version = 1;
    uint16_t flags;
    int64_t  start_timestamp_ms;
    int64_t  end_timestamp_ms;
    uint32_t tick_count;
    uint32_t reserved;
};

// Each tick record (variable size, ~200-500 bytes typical)
struct TickRecord {
    int64_t  timestamp_ms;
    uint16_t bid_count;
    uint16_t ask_count;
    // Followed by: bid_count × (price:f64, size:f64)
    // Followed by: ask_count × (price:f64, size:f64)
};
```

### Tiered Retention Policy

| Resolution | Retention | Storage Est. (BTC-USD) |

|------------|-----------|------------------------|

| 100ms (base) | 3 days | ~2-5 GB |

| 1s | 14 days | ~500 MB |

| 5s | 30 days | ~100 MB |

| 1min | 1 year | ~50 MB |

Background rollup jobs downsample and compress older data automatically.

### SQLite Schema (metadata)

```sql
-- Symbol configuration
CREATE TABLE symbols (
    id INTEGER PRIMARY KEY,
    symbol TEXT UNIQUE NOT NULL,
    exchange TEXT NOT NULL,
    tick_size REAL NOT NULL,
    min_price REAL,
    max_price REAL,
    created_at INTEGER
);

-- Segment index for fast time-range lookups
CREATE TABLE segments (
    id INTEGER PRIMARY KEY,
    symbol_id INTEGER REFERENCES symbols(id),
    resolution_ms INTEGER NOT NULL,
    start_time_ms INTEGER NOT NULL,
    end_time_ms INTEGER NOT NULL,
    file_path TEXT NOT NULL,
    byte_offset INTEGER,
    byte_length INTEGER,
    tick_count INTEGER,
    UNIQUE(symbol_id, resolution_ms, start_time_ms)
);
CREATE INDEX idx_segments_time ON segments(symbol_id, resolution_ms, start_time_ms);

-- Trade log (optional, for CVD/delta tracking)
CREATE TABLE trades (
    id INTEGER PRIMARY KEY,
    symbol_id INTEGER REFERENCES symbols(id),
    timestamp_ms INTEGER NOT NULL,
    trade_id TEXT,
    price REAL NOT NULL,
    size REAL NOT NULL,
    side INTEGER  -- 0=buy, 1=sell
);
CREATE INDEX idx_trades_time ON trades(symbol_id, timestamp_ms);
```

---

## C. In-Memory Data Structures

### Server Memory Model

```cpp
class ServerDataModel {
    // Hot ring buffers per symbol (last N minutes at base resolution)
    struct SymbolHotData {
        std::string symbol;
        LiveOrderBook currentBook;           // Existing O(1) dense book
        RingBuffer<TickSnapshot, 18000> ticks; // 30min × 100ms = 18000 ticks
        
        // Pre-aggregated ring buffers for each timeframe
        std::map<int64_t, RingBuffer<AggregatedSlice, 1000>> aggBuffers;
        // 1s: 1000 slices = ~16 min
        // 5s: 1000 slices = ~83 min
        // etc.
    };
    
    std::unordered_map<std::string, SymbolHotData> m_symbols;
};
```

### Tick Snapshot (100ms grain)

```cpp
struct TickSnapshot {
    int64_t timestamp_ms;
    
    // Sparse representation for memory efficiency
    std::vector<std::pair<int32_t, float>> bidLevels;  // (tick_index, quantity)
    std::vector<std::pair<int32_t, float>> askLevels;
    
    // Trade summary for this 100ms window
    float buyVolume = 0;
    float sellVolume = 0;
    uint16_t tradeCount = 0;
};
```

### Aggregation Flow

```
Raw book update (per-tick)
        │
        ▼
┌─────────────────────┐
│   LiveOrderBook     │  (existing, O(1) updates)
│   applyUpdates()    │
└─────────────────────┘
        │
        ▼ (every 100ms timer)
┌─────────────────────┐
│  Snapshot Capture   │  LiveOrderBook.captureDenseNonZero()
└─────────────────────┘
        │
        ▼
┌─────────────────────┐
│   TickSnapshot      │  → RingBuffer (hot)
│                     │  → Binary segment (cold)
└─────────────────────┘
        │
        ▼ (on boundary: 1s, 5s, etc.)
┌─────────────────────┐
│   Rollup Worker     │  Aggregate N ticks → 1 AggregatedSlice
└─────────────────────┘
        │
        ▼
┌─────────────────────┐
│  AggregatedSlice    │  → Timeframe ring buffer
│  (existing LTSE     │  → Higher-res segment file
│   LiquidityTimeSlice)│
└─────────────────────┘
```

---

## D. Streaming Protocol: Server → Client

### Transport: WebSocket (Boost.Beast)

**Why WebSocket over gRPC:**

- Already have Boost.Beast in the codebase
- Simpler deployment (single port, HTTP upgrade)
- Easy debugging with browser tools
- Bidirectional streaming native

### Encoding: Binary with JSON fallback

**Primary: Packed binary structs** (for production)

- 10-20x smaller than JSON
- Zero parsing overhead
- Use pragma pack or FlatBuffers later

**Fallback: JSON** (for debugging)

- Enabled via `?format=json` query param

### Message Types

```cpp
enum class MsgType : uint8_t {
    // Client → Server
    Subscribe       = 0x01,
    Unsubscribe     = 0x02,
    SnapshotRequest = 0x03,
    Ping            = 0x04,
    
    // Server → Client
    SubscribeAck    = 0x10,
    Snapshot        = 0x11,
    TickDelta       = 0x12,
    BookDelta       = 0x13,
    TradeBatch      = 0x14,
    Pong            = 0x15,
    Error           = 0x1F,
};

// Binary message header (8 bytes)
struct MessageHeader {
    uint32_t length;      // Total message length including header
    MsgType  type;
    uint8_t  flags;       // Compression, etc.
    uint16_t sequence;    // For ordering/dedup
};
```

### Key Messages

**Subscribe Request:**

```cpp
struct SubscribeMsg {
    MessageHeader header;
    char symbol[16];           // "BTC-USD\0"
    int64_t timeframe_ms;      // 0 = auto (server picks best for viewport)
    int64_t viewport_start_ms; // For initial snapshot
    int64_t viewport_end_ms;
    uint8_t feeds;             // Bitmask: 0x01=book, 0x02=trades, 0x04=agg
};
```

**Snapshot Response:**

```cpp
struct SnapshotMsg {
    MessageHeader header;
    char symbol[16];
    int64_t timeframe_ms;
    int64_t start_time_ms;
    int64_t end_time_ms;
    uint32_t slice_count;
    // Followed by: slice_count × AggregatedSlice data
};
```

**Live Delta (incremental update):**

```cpp
struct TickDeltaMsg {
    MessageHeader header;
    char symbol[16];
    int64_t timestamp_ms;
    uint16_t bid_delta_count;
    uint16_t ask_delta_count;
    // Followed by: deltas as (tick_index:i32, qty:f32)
};
```

### Subscription Model

1. Client connects → Server sends `ServerInfo` (supported symbols, TFs)
2. Client sends `Subscribe` with viewport params
3. Server responds with `Snapshot` (historical backfill)
4. Server streams `TickDelta` / `BookDelta` in real-time
5. Client can request new `Snapshot` on zoom/pan (with `viewport_start/end`)
6. Server automatically adjusts TF based on viewport width

---

## E. Fault Tolerance and Gap-Free Behavior

### Exchange Reconnection (Server)

```cpp
class ExchangeReconnector {
    void onDisconnect() {
        m_connected = false;
        // Mark data as potentially stale
        m_lastGoodTimestamp = now();
        
        // Exponential backoff: 1s → 2s → 4s → ... → 60s max
        scheduleReconnect(m_backoff);
        m_backoff = std::min(m_backoff * 2, 60s);
    }
    
    void onReconnect() {
        m_connected = true;
        m_backoff = 1s;
        
        // Request full snapshot to heal any gaps
        requestFullBookSnapshot();
        
        // Mark gap in data (clients can show "gap" indicator)
        recordDataGap(m_lastGoodTimestamp, now());
    }
};
```

### Server Startup (Warm Start)

```cpp
void Server::warmStart() {
    // 1. Load segment index from SQLite
    loadSegmentIndex();
    
    // 2. Reload last N minutes of ticks into hot ring buffers
    for (auto& symbol : m_symbols) {
        auto ticks = loadTicksFromSegments(symbol, now() - 30min, now());
        symbol.hotBuffer.bulkLoad(ticks);
    }
    
    // 3. Rebuild aggregation state from hot data
    rebuildAggregationState();
    
    // 4. Connect to exchange and resume streaming
    connectToExchange();
}
```

### Client Reconnection

```cpp
void Client::onServerDisconnect() {
    // Show "reconnecting" indicator
    m_ui.showReconnecting();
    
    // Exponential backoff retry
    scheduleReconnect();
}

void Client::onServerReconnect() {
    // Request snapshot for current viewport (heals any gaps)
    sendSnapshotRequest(m_viewport.startTime, m_viewport.endTime);
    
    // Resume live stream
    resubscribe();
}
```

### Gap Tracking

```sql
-- Track known data gaps for UI indication
CREATE TABLE data_gaps (
    id INTEGER PRIMARY KEY,
    symbol_id INTEGER REFERENCES symbols(id),
    start_time_ms INTEGER NOT NULL,
    end_time_ms INTEGER NOT NULL,
    reason TEXT  -- "exchange_disconnect", "server_restart", etc.
);
```

---

## F. Exchange Adapter Abstraction

### Interface

```cpp
// [libs/core/marketdata/adapters/IExchangeAdapter.hpp]
class IExchangeAdapter {
public:
    virtual ~IExchangeAdapter() = default;
    
    // Lifecycle
    virtual void connect() = 0;
    virtual void disconnect() = 0;
    virtual bool isConnected() const = 0;
    
    // Subscriptions
    virtual void subscribe(const std::vector<std::string>& symbols) = 0;
    virtual void unsubscribe(const std::vector<std::string>& symbols) = 0;
    
    // Callbacks (set by server)
    std::function<void(const NormalizedTrade&)> onTrade;
    std::function<void(const std::string& symbol, 
                       std::span<const BookLevelUpdate>)> onBookUpdate;
    std::function<void(const std::string& symbol,
                       const NormalizedBookSnapshot&)> onBookSnapshot;
    std::function<void(bool connected)> onConnectionStatus;
    std::function<void(const std::string& error)> onError;
    
    // Symbol normalization
    virtual std::string normalizeSymbol(const std::string& exchangeSymbol) = 0;
    virtual double getTickSize(const std::string& symbol) = 0;
};
```

### Normalized Data Types

```cpp
// Exchange-agnostic trade
struct NormalizedTrade {
    int64_t timestamp_ms;
    std::string symbol;       // Canonical: "BTC-USD"
    std::string exchange_id;  // Original trade ID from exchange
    double price;
    double size;
    AggressorSide side;
};

// Exchange-agnostic book update
struct NormalizedBookSnapshot {
    int64_t timestamp_ms;
    std::string symbol;
    std::vector<std::pair<double, double>> bids;  // (price, size)
    std::vector<std::pair<double, double>> asks;
};
```

### Coinbase Adapter (existing code refactored)

```cpp
// [libs/core/marketdata/adapters/CoinbaseAdapter.cpp]
class CoinbaseAdapter : public IExchangeAdapter {
    // Reuse existing: BeastWsTransport, Authenticator, MessageDispatcher
    // Just wire callbacks to normalized interface
    
    void handleTradeEvent(const TradeEvent& e) {
        NormalizedTrade t;
        t.timestamp_ms = e.timestamp;
        t.symbol = e.product_id;  // Coinbase already uses canonical format
        t.price = e.price;
        t.size = e.size;
        t.side = e.side;
        
        if (onTrade) onTrade(t);
    }
};
```

---

## G. Migration Plan (Incremental Steps)

### Phase 1: Extract Server Core (Week 1)

1. **Create `apps/sentinel_server/` target**

   - New CMakeLists.txt
   - Minimal `server_main.cpp` that boots headless

2. **Move `MarketDataCore` initialization to server**

   - Server owns `Authenticator`, `DataCache`, `MarketDataCore`
   - Remove these from `MainWindowGPU`

3. **Add basic WebSocket server** (Boost.Beast)

   - Listen on port 9000
   - Accept connections, echo test

4. **Wire up simple JSON streaming**

   - Server: on trade → broadcast JSON to all clients
   - Client: connect to `ws://localhost:9000`, receive trades

**Milestone:** Client renders live trades from server (JSON, no persistence)

### Phase 2: Binary Protocol + Book Streaming (Week 2)

5. **Define binary message format**

   - Header struct, message types
   - Pack/unpack helpers

6. **Stream order book deltas**

   - Server: capture `LiveOrderBook` deltas, broadcast
   - Client: maintain local `LiveOrderBook` from deltas

7. **Add subscription management**

   - Client sends `Subscribe` with symbol
   - Server tracks per-client subscriptions

**Milestone:** Client renders live heatmap from server (binary, no persistence)

### Phase 3: Persistence Layer (Week 3)

8. **Add SQLite for metadata**

   - Symbol config, segment index tables
   - Simple wrapper class

9. **Implement binary segment writer**

   - 100ms tick snapshots to hourly files
   - Segment header + tick records

10. **Add segment reader for warm start**

    - Load recent segments on server boot
    - Populate hot ring buffers

**Milestone:** Server survives restart, client sees continuous data

### Phase 4: Snapshot + Backfill (Week 4)

11. **Implement `SnapshotRequest` handler**

    - Client requests time range
    - Server queries segments + hot buffer
    - Returns aggregated snapshot

12. **Client viewport-driven requests**

    - On pan/zoom, request snapshot for new viewport
    - Merge with live stream

13. **Add timeframe auto-selection**

    - Server suggests TF based on viewport width
    - Client can override

**Milestone:** Full historical browsing works

### Phase 5: Aggregation Pipeline (Week 5)

14. **Background rollup jobs**

    - 100ms → 1s, 1s → 5s, 5s → 1min
    - Run on timer, write to agg segment files

15. **Multi-TF ring buffers on server**

    - Keep hot data for each TF
    - Serve correct TF based on client request

16. **Retention cleanup job**

    - Delete old 100ms segments (>3 days)
    - Keep higher TFs longer

**Milestone:** DLOD works, storage bounded

### Phase 6: Production Hardening (Week 6+)

17. **Add heartbeat/watchdog**

    - Client sends ping, server sends pong
    - Detect stale connections

18. **Metrics and logging**

    - Connection count, message rate, latency histograms
    - Expose via simple HTTP endpoint

19. **Multi-symbol support**

    - Test with BTC-USD + ETH-USD
    - Per-symbol subscription routing

20. **Exchange adapter abstraction**

    - Extract `IExchangeAdapter` interface
    - Refactor Coinbase code to implement it

**Milestone:** Stable 24/7 server, ready for additional exchanges

---

## H. Key Files to Create/Modify

### New Files

| Path | Purpose |

|------|---------|

| `apps/sentinel_server/CMakeLists.txt` | Server build target |

| `apps/sentinel_server/server_main.cpp` | Server entry point |

| `libs/core/server/ServerCore.hpp` | Server orchestrator |

| `libs/core/server/ClientStreamer.hpp` | WebSocket server for clients |

| `libs/core/server/SegmentWriter.hpp` | Binary segment persistence |

| `libs/core/server/SegmentReader.hpp` | Segment loading |

| `libs/core/server/AggregationWorker.hpp` | Background rollup jobs |

| `libs/core/protocol/Protocol.hpp` | Message definitions |

| `libs/core/protocol/PacketCodec.hpp` | Binary pack/unpack |

| `libs/core/marketdata/adapters/IExchangeAdapter.hpp` | Adapter interface |

| `libs/core/marketdata/adapters/CoinbaseAdapter.cpp` | Refactored Coinbase |

| `libs/gui/client/ServerConnection.hpp` | Client-side WS connection |

| `libs/gui/client/RemoteDataProvider.hpp` | Replaces direct MarketDataCore |

### Modified Files

| Path | Changes |

|------|---------|

| `CMakeLists.txt` | Add server target |

| `vcpkg.json` | Add sqlite3 dependency |

| `libs/gui/MainWindowGpu.cpp` | Remove MarketDataCore, use RemoteDataProvider |

| `libs/core/LiquidityTimeSeriesEngine.h` | May need minor refactoring for server use |

---

## I. Performance Considerations

### Hot Path (Server)

- **Book updates:** O(1) via existing `LiveOrderBook`
- **Tick capture:** 100ms timer, ~10 ticks/sec/symbol
- **Client broadcast:** Zero-copy buffer sharing for multi-client
- **Segment writes:** Async, buffered, hourly flush

### Memory Budget (Raspberry Pi 5, 8GB)

| Component | Estimate |

|-----------|----------|

| LiveOrderBook per symbol | ~2 MB |

| Hot tick buffer (30min × 100ms) | ~50 MB |

| Aggregation buffers (all TFs) | ~20 MB |

| WebSocket buffers (10 clients) | ~10 MB |

| SQLite cache | ~50 MB |

| **Total per symbol** | ~130 MB |

With 4 symbols: ~520 MB, leaving plenty of headroom.

### Latency Budget

| Stage | Target |

|-------|--------|

| Exchange WS → Server | <5 ms |

| Server processing | <1 ms |

| Server → Client WS | <5 ms |

| Client processing | <5 ms |

| **Total tick-to-screen** | <16 ms (60 FPS) |