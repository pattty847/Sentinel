# Progress: 

*   **Phase 0: Abstractions (Completed)**: Decoupled the GUI from the data layer using `IGridDataSource`. The monolith mode now uses `LocalGridDataSource` which wraps the existing logic seamlessly.
*   **Phase 1: Server Skeleton (Completed)**: Built the `sentinel-server` headless application, complete with `ServerDataModel` and `SentinelStreamServer` (using Boost.Beast). It connects to Coinbase and is ready to stream.
*   **Phase 2: Client Remote Mode & Streaming (Completed)**: 
    *   Implemented `SentinelStreamClient` and `RemoteGridDataSource`.
    *   Added runtime toggle (`SENTINEL_REMOTE=1`).
    *   Implemented `SentinelSession` in `SentinelStreamServer`.
    *   **Control Plane**: Wired up subscription handshake (Client -> Server: "subscribe" ACK).
    *   **Data Plane**: Wired `ServerDataModel` -> `SentinelSession` to broadcast live `trade` and `l2update` messages.
    *   **Protocol v0**: Implemented basic JSON serialization for Trades and Book Updates (with index-to-price conversion on server).

*   **Phase 3: Snapshot & Reconstitution (Completed)**:
    *   **Server Side**: 
        *   `MarketDataCore` emits `liveOrderBookInitialized` signal.
        *   `ServerDataModel` maintains a live `LiveOrderBook` replica.
        *   `SentinelStreamServer` sends full book `snapshot` JSON on client subscription.
    *   **Client Side**:
        *   `SentinelStreamClient` parses `snapshot` and `l2update` messages.
        *   `RemoteGridDataSource` reconstitutes the book from snapshot + updates.

*   **Phase 4: Persistence & Aggregation (Completed)**:
    *   **TickBinaryLogger**: Implemented raw binary logger for Trades and Book Updates. Rotates files hourly. Wired into `ServerDataModel`.
    *   **TimeframeAggregator**: Implemented OHLCV aggregation (1s, 1m, 5m, 1h). Stores history in RAM. Wired into `ServerDataModel`.
    *   **Status**: Server now persists raw data to disk and builds live history in memory.

**Status:**

We have a fully functional **Client-Server Streaming Architecture** with **Persistence**.
*   Server ingests -> Logs to Disk -> Aggregates in RAM -> Updates Model -> Broadcasts.

**Pending / Unsolved:**

1.  **History Backfill**: Clients still start with empty charts (only live bars). Need to expose `get_history` in the protocol.
2.  **Performance**: JSON is verbose. Moving to binary (Phase 5) is critical for high-volatility days.
3.  **Client-Side History**: `LiquidityTimeSeriesEngine` needs to consume the backfilled history.

**Next Steps (Phase 5):**

1.  **Protocol Extension**: Add `get_history` request/response.
2.  **Binary Protocol**: Switch streaming to binary.
