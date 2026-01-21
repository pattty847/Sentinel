# Stream Pipeline Review Notes

Scope: HeatmapIntensityNode, RemoteGridDataSource, SentinelStreamClient, and stream ingestion.

## Findings

1) SentinelStreamClient writes are not thread-safe
- subscribe/unsubscribe can be called off the I/O thread; doWrite uses m_ws.async_write directly.
- Risk: Beast async_write must be serialized on the io_context/strand; data race on m_writeQueue.front().
- Fix: post all queue operations to io_context (strand) and only call async_write there.
- File: libs/core/protocol/SentinelStreamClient.cpp

2) Snapshot does not trigger UI refresh
- Snapshot updates the local replica book, but no signal is emitted to refresh UI.
- Risk: UI stays stale until the first incremental update arrives.
- Fix: emit liveOrderBookUpdated (with deltas) or orderBookUpdated after snapshot apply.
- File: libs/gui/datasources/RemoteGridDataSource.cpp

3) Remote book initialization uses hardcoded range/tick
- Snapshot init uses fixed min/max/tick for all symbols.
- Risk: server-authoritative grid invariants can be violated; non-BTC symbols will be wrong.
- Fix: add protocol fields (min/max/tick) or infer from snapshot.
- File: libs/gui/datasources/RemoteGridDataSource.cpp

4) Heatmap upload path depends on OpenGL backend
- HeatmapIntensityNode uses QNativeInterface::QSGOpenGLTexture.
- Risk: no uploads on Vulkan/Metal/D3D; GPU heatmap stalls when QSG backend is not OpenGL.
- Fix: add QRhi upload path or force OpenGL; note as a follow-up.
- File: libs/gui/render/HeatmapIntensityNode.cpp

## Notes

- Timeframe UI updates: prefer periodic refresh (e.g., 1s) and/or threshold-based updates for long slices so large timeframes do not stall.
- RHI migration: desired for OS portability; note for follow-up implementation.
