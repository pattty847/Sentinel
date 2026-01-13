---
name: Sentinel Client/Server Implementation Plan
overview: ""
todos:
  - id: aaaddd14-d342-492f-8a34-8e5d37e5ca2b
    content: Create IGridDataSource interface and LocalGridDataSource implementation
    status: pending
  - id: df2fae8d-dc4c-41a4-b5c3-36080fe439f4
    content: Refactor DataProcessor to use IGridDataSource
    status: pending
  - id: 035e1130-e4d4-4b72-b54f-e67c7da98545
    content: Create sentinel-server app skeleton and build targets
    status: pending
  - id: 16bab3dc-01b4-4b1e-b787-5b93cba48c56
    content: Implement ServerDataModel and internal structures
    status: pending
  - id: 7d497ecd-8d85-4b39-a047-d3f2c416a84f
    content: Implement SentinelStreamServer and JSON protocol v0
    status: pending
  - id: 874dcab0-5450-4453-8d1a-2f72ef76c6a1
    content: Implement SentinelStreamClient and RemoteGridDataSource
    status: pending
  - id: c9d57efa-a9c7-4aa0-a45a-d6ef9c341f70
    content: Add runtime toggle in GUI for Local/Remote mode
    status: pending
---

# Sentinel Client/Server Implementation Plan

This plan details the separation of Sentinel into a headless server (`sentinel-server`) and a lightweight GUI client (`sentinel-gui`), introducing a storage layer and WebSocket streaming.

## A. Current → Target Mapping

### 1. Existing Components

| Component | Current Location | Target Role & Location | Changes |

|-----------|------------------|------------------------|---------|

| **MarketDataCore** | `libs/core/marketdata` | **Shared**. Used by Server (ingest) and Client (monolith mode). | No major changes. Server uses it to feed `ServerDataModel`. |

| **DataCache** | `libs/core/marketdata/cache` | **Shared**. Server uses it for live state. Client (monolith) uses it. | Client (remote) bypasses it in favor of `RemoteGridDataSource`. |

| **LiveOrderBook** | `libs/core/marketdata/cache` | **Shared**. Fundamental DTO. | No changes. |

| **LiquidityTimeSeriesEngine** | `libs/core` | **Client (mostly)**. | Adapted to accept pre-aggregated slices from `RemoteGridDataSource` in addition to raw snapshots. |

| **DataProcessor** | `libs/gui/render` | **Client**. | Refactored to use `IGridDataSource` interface instead of direct `DataCache`. |

| **UnifiedGridRenderer** | `libs/gui` | **Client**. | Unchanged. Agnostic to data source. |

| **BeastWsTransport** | `libs/core/marketdata/ws` | **Shared**. | Used by both `MarketDataCore` (exchange connection) and new `SentinelStreamClient`. |

### 2. Architecture Split

-   **`libs/core`**: Remains the home for shared business logic, models (`Trade`, `OrderBook`), transport (`BeastWsTransport`), and protocols.
-   **`apps/sentinel-server`**: New headless application. Owns `ServerDataModel`, Persistence, and `SentinelStreamServer`.
-   **`apps/sentinel-gui`**: Existing GUI app. Refactored to support "Remote" mode via `IGridDataSource`.

---

## B. New Modules & Files

### 1. Core Server Model (`libs/core/servermodel/`)

Manages server-side state, aggregation, and persistence.

-   `ServerDataModel.hpp/.cpp`: Single source of truth for the server. Manages `SymbolHotData`.
-   `SymbolHotData.hpp`: Container for `LiveOrderBook`, raw tick ring buffer, and aggregated slice buffers per symbol.
-   `TickBinaryLogger.hpp/.cpp`: Handles append-only binary logging of raw ticks.
-   `TimeframeAggregator.hpp/.cpp`: Worker that rolls raw ticks -> 100ms -> 1s slices.
-   `StorageLayout.hpp`: Defines struct layouts (`SegmentHeader`, `TickSnapshotRecord`) and directory paths.

### 2. Protocol (`libs/core/protocol/`)

Shared definitions for the client-server streaming protocol.

-   `SentinelStreamProtocol.hpp`: Message types (JSON/Binary), Enums (Subscribe, Snapshot, Error), and serialization helpers.
-   `SentinelStreamServer.hpp/.cpp`: WebSocket server implementation (using Boost.Beast). Handles client sessions.
-   `SentinelStreamClient.hpp/.cpp`: WebSocket client implementation. Handles reconnection, subscription, and message parsing.

### 3. GUI Data Sources (`libs/gui/datasources/`)

Abstraction layer for the GUI to consume data.

-   `IGridDataSource.hpp`: Abstract interface.
    ```cpp
    virtual GridDataSnapshot currentSnapshot(const QString& symbol, Timeframe tf) = 0;
    virtual void subscribe(const QString& symbol, Timeframe tf) = 0;
    signals: void slicesUpdated(...);
    ```

-   `RemoteGridDataSource.hpp/.cpp`: Implements `IGridDataSource` using `SentinelStreamClient`.
-   `LocalGridDataSource.hpp/.cpp`: Implements `IGridDataSource` using `DataCache` (preserves monolith functionality).

### 4. Server App (`apps/sentinel-server/`)

-   `main.cpp`: Entry point. Parses config/args.
-   `SentinelServerApp.hpp/.cpp`: Application class. Wires `MarketDataCore` -> `ServerDataModel` -> `SentinelStreamServer`.

---

## C. Storage & Aggregation Implementation

### 1. Storage Format

-   **Metadata**: SQLite (`data/sentinel.db`) stores `symbols` and `segments` tables.
-   **Raw Ticks**: Binary files in `data/market/<symbol>/ticks/`. Rotated hourly or by size.
-   **Aggregates**: Binary files in `data/market/<symbol>/agg_<res>/`. Stored as arrays of `AggregatedSlice`.

### 2. Aggregation Pipeline

1.  `MarketDataCore` receives trade/book update.
2.  `ServerDataModel::onBookUpdate` updates in-memory `LiveOrderBook`.
3.  `TickBinaryLogger` appends raw update to current `.tmp` segment.
4.  `TimeframeAggregator` (timer-driven):

    -   Snapshots `LiveOrderBook` every 100ms.
    -   Updates RAM ring buffer for 100ms slices.
    -   Triggers rollups (100ms -> 1s) when enough data accumulates.
    -   Persists completed slices to disk.

### 3. Warmup

-   On startup, `ServerDataModel` reads the SQLite index to find the latest segments.
-   Loads the most recent slices into RAM ring buffers to support immediate client snapshots.
-   Reconstructs `LiveOrderBook` state from the last checkpoint if available (or waits for fresh exchange snapshot).

---

## D. Streaming Path

### 1. Protocol (v0 JSON)

-   **Request**: `{ "type": "subscribe", "symbol": "BTC-USD", "timeframe": 1000 }`
-   **Response (Snapshot)**: `{ "type": "snapshot", "slices": [ ...serialized slices... ] }`
-   **Response (Update)**: `{ "type": "slice_batch", "slices": [ ...new slices... ] }`

### 2. Client Integration

1.  **`RemoteGridDataSource`** connects to `ws://localhost:PORT`.
2.  On `subscribe("BTC-USD")`, sends JSON request.
3.  On message `snapshot` or `slice_batch`:

    -   Parses JSON to `LiquidityTimeSlice` objects.
    -   Emits `slicesUpdated`.

4.  **`DataProcessor`**:

    -   Accepts `IGridDataSource` in constructor.
    -   Subscribes to `slicesUpdated` signal.
    -   Pushes received slices into `LiquidityTimeSeriesEngine`.
    -   **Note**: `LiquidityTimeSeriesEngine` needs a new method `addAggregatedSlice(const LiquidityTimeSlice&)` to bypass internal aggregation logic when running in remote mode.

---

## E. Phased Migration Plan

### Phase 0: Abstractions

-   **Goal**: Decouple GUI from DataCache.
-   **Steps**:

    1.  Create `IGridDataSource` interface.
    2.  Implement `LocalGridDataSource` (wraps existing `DataCache` access).
    3.  Refactor `DataProcessor` to use `IGridDataSource`.
    4.  Verify Monolith mode works exactly as before.

### Phase 1: Server Skeleton

-   **Goal**: Running server process with basic streaming.
-   **Steps**:

    1.  Create `apps/sentinel-server`.
    2.  Implement `ServerDataModel` (RAM only initially).
    3.  Implement `SentinelStreamServer` (WebSocket echo/simple push).
    4.  Wire `MarketDataCore` to feed `ServerDataModel`.

### Phase 2: Remote Client

-   **Goal**: GUI connects to Server.
-   **Steps**:

    1.  Implement `SentinelStreamClient` and `RemoteGridDataSource`.
    2.  Add config toggle in `sentinel-gui` (Local vs Remote).
    3.  Implement JSON protocol v0 for live updates.
    4.  Verify GUI renders live data from Server.

### Phase 3: Persistence

-   **Goal**: Server saves/loads data.
-   **Steps**:

    1.  Implement `TickBinaryLogger` and `TimeframeAggregator`.
    2.  Add SQLite metadata management.
    3.  Implement Server warmup (load from disk).
    4.  Verify history is available on Client reconnect.

### Phase 4: Optimization

-   **Goal**: Production-ready performance.
-   **Steps**:

    1.  Migrate Protocol to Binary (v1).
    2.  Optimize aggregation loops.
    3.  Stress test with high-frequency feeds.

---

## F. Risks & Mitigation

-   **Performance**: Streaming heavy JSON is slow. **Mitigation**: Move to binary protocol (Phase 4) quickly; use dense arrays for JSON initially.
-   **Threading**: Race conditions in `ServerDataModel`. **Mitigation**: Use `std::shared_mutex` for model access; stick to single-writer pattern (Dispatcher thread).
-   **Data Gaps**: Network instability. **Mitigation**: Client must request "catch-up" snapshot if sequence numbers are skipped.