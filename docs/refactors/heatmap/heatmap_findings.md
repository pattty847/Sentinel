# Heatmap Review Findings

This document captures the current state of Sentinel's heatmap pipeline and the issues that block scale and correctness. It is intentionally blunt so we can rebuild cleanly.

## Critical

1) **Cross-thread GUI access**
- `DataProcessor` reads and writes `GridViewState` from the worker thread, but `GridViewState` is GUI-thread only. This is undefined behavior and can cause missing slices, random gaps, and crashes.
- Examples: `libs/gui/render/DataProcessor.cpp` uses `m_viewState->setViewport` and reads viewport fields inside worker thread logic.

2) **Remote data source is not thread-safe**
- `RemoteGridDataSource` mutates `m_replicaBooks` on the GUI thread while `DataProcessor` reads `getDirectLiveOrderBook` on the worker thread. No lock or copy. This is a data race.
- `LiveOrderBook` itself is mutated with no synchronization between threads.

3) **Server session access is not serialized**
- `Session` (server) touches `subscriptions_` from both the IO thread and the Qt signal thread without protection. This can corrupt the set or cause incorrect filtering.

4) **Client websocket writes are not serialized**
- `SentinelStreamClient` can call `doWrite()` while other writes are in-flight. Boost.Beast requires a single in-flight async_write. This can lead to hard-to-debug failures.

## High

1) **Hardcoded book range and tick size**
- Both server and client force `min/max/tick` to `75000..125000` and `0.01`. This guarantees gaps when the true price range drifts or for other symbols.

2) **Snapshot protocol missing book params**
- Snapshot messages do not include min price, max price, tick size, or timestamp. The client has to guess, which leads to drift and incorrect reconstruction.

3) **Gaps are explicitly created**
- `captureOrderBookSnapshot` intentionally does not fill missed 100ms buckets. This creates empty columns if updates/timer ticks are skipped.

## Medium

1) **Remote trade cache is empty**
- `RemoteGridDataSource::getRecentTrades()` returns empty, so any GUI feature using it breaks in remote mode.

2) **No full-book signal on snapshot**
- On snapshot, the remote client does not emit a full-book update signal; downstream data paths may remain stale until deltas arrive.

3) **Server snapshot is synchronous and large**
- Full book JSON serialization happens on the session thread and can block IO. It is OK for MVP, not OK at scale.

## Heatmap-Specific Performance Blockers

1) **CPU-per-cell render loop**
- `HeatmapStrategy` builds geometry by iterating all visible cells, computing color, and doing world-to-screen conversion on the CPU. This is the old tax the report warns against.

2) **Geometry rebuilds are frequent**
- Any change to geometry/material triggers per-cell CPU work and a full buffer rewrite.

3) **No GPU-resident heatmap**
- The GPU is only drawing triangles. It is not shading a data texture or buffer.

## Additional Hot-Path Issues (DOD)

1) **Map-heavy snapshot format**
- `LiquidityTimeSeriesEngine` uses `std::map<double,double>` in `OrderBookSnapshot`, which is pointer-heavy and cache-unfriendly in tight loops.

2) **Slice range expansion reallocations**
- `addSnapshotToSlice` expands metric vectors when price range widens, moving large arrays frequently.

3) **Full scans on each snapshot**
- `updateDisappearingLevels` iterates all metrics each snapshot; cost grows with range size.

4) **Visible slice query overhead**
- `getVisibleSlices` builds a `std::map` per query to dedupe slices by time range.

5) **Dense snapshot scan cost**
- `LiveOrderBook::captureDenseNonZero` scans full arrays to find non-zero levels; if range is too large, this is expensive.

## Bottom Line

The current heatmap path cannot scale. We will replace it with a GPU-first texture-based renderer and fix concurrency/protocol correctness before performance tuning.
