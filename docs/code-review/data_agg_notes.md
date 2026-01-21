# Data Cache + Aggregation Review Notes

Scope: DataCache, TimeframeAggregator, TickBinaryLogger.

## Findings

1) RingBuffer snapshot order is not chronological
- RingBuffer::snapshot returns the raw backing vector without reordering by head.
- After wrap, recentTrades/tradesSince can return trades out of order or interleaved.
- Risk: UI/consumers that assume chronological order will be wrong.
- Files: libs/core/marketdata/cache/DataCache.hpp, libs/core/marketdata/cache/DataCache.cpp

2) DataCache LiveOrderBook uses hardcoded price ranges
- initializeLiveOrderBook hardcodes min/max ranges (75000-125000) for all symbols.
- Risk: wrong book size for non-BTC symbols; violates server-authoritative range/tick invariants.
- Fix: infer from snapshot or accept ranges from server/protocol.
- Files: libs/core/marketdata/cache/DataCache.cpp

3) TimeframeAggregator emits signals while holding its mutex
- onTrade/updateBar emit barClosed/barUpdated under the unique lock.
- Risk: reentrancy or deadlock if slots touch the aggregator; also blocks producers.
- Fix: release lock before emitting or use queued connections with copies.
- Files: libs/core/servermodel/TimeframeAggregator.cpp

4) TickBinaryLogger uses system time for book deltas
- Trades are logged with exchange timestamps, but book updates use system time.
- Risk: timeline skew between trades and book updates in logs.
- Fix: include exchange timestamp where available or tag source for replay alignment.
- Files: libs/core/servermodel/TickBinaryLogger.cpp
