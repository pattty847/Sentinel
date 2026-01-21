# Server Pipeline Review Notes

Scope: HeatmapTwapStreamer, ServerDataModel, SentinelStreamServer.

## Findings

1) Heatmap reset flag never emitted
- HeatmapTwapStreamer sets state.pendingReset when the mid-price drifts outside the edge band, but finalizeBucket() always emits reset=false.
- Risk: clients never rebuild when the server recenters/reinitializes the heatmap grid.
- Fix: propagate state.pendingReset into the emitted reset flag and clear it after emission.
- Files: libs/core/servermodel/HeatmapTwapStreamer.cpp

2) Server-mode order book updates not broadcast
- MarketDataCore emits liveOrderBookLevelUpdates in server mode, which updates the local book, but ServerDataModel does not forward any deltas to bookUpdateBroadcast.
- Risk: remote clients only get the snapshot, then no subsequent order book updates.
- Fix: derive BookDelta from level updates or compute deltas in LiveOrderBook::applyUpdates and emit bookUpdateBroadcast.
- Files: libs/core/servermodel/ServerDataModel.cpp

3) Hardcoded order book range
- onLiveOrderBookInitialized uses fixed min/max ranges (75000-125000) for all symbols.
- Risk: breaks server-authoritative range/tick invariants and yields wrong books for non-BTC symbols.
- Fix: include range/tick in protocol or infer from snapshot; remove hardcoded ranges.
- Files: libs/core/servermodel/ServerDataModel.cpp
