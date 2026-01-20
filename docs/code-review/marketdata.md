# MarketData Review Notes

## libs/core/marketdata/MarketDataCore.hpp / .cpp

Purpose
- Owns WebSocket transport, subscription management, JSON dispatch, and publishes trades/order book deltas.

Keep
- Clear separation of transport callbacks vs. parsing/dispatch.
- Strand usage for transport callbacks and watchdog timers.
- Resilient io_context loop to keep worker thread alive.
- Explicit CA bundle support for portability.

Risks / Issues
- Qt dependency inside core layer (QObject/QMetaObject/QPointer, signals, qEnvironmentVariableIsSet). This violates core layer purity; should be split into core engine + Qt adapter. (See `libs/core/marketdata/MarketDataCore.hpp:38` and usage in `libs/core/marketdata/MarketDataCore.cpp`.)
- Data race on `m_products`: `subscribeToSymbols`/`unsubscribeFromSymbols` mutate the vector on caller thread while other accesses occur on the strand (subscribe/replay). Needs strand-only mutation or a mutex. (`libs/core/marketdata/MarketDataCore.cpp:128-160`, `libs/core/marketdata/MarketDataCore.cpp:271-314`, `libs/core/marketdata/MarketDataCore.cpp:590-594`)
- `m_lastSeqByProduct` cleared without mutex and then cleared again under mutex. If `checkAndTrackSequence` ever runs on another thread, this is a race. (`libs/core/marketdata/MarketDataCore.cpp:82-88`)
- Transport lifecycle calls (`connect/close`) are invoked from multiple threads; whether this is safe depends on `BeastWsTransport` threading guarantees. If it expects strand-only access, this is a latent race. (`libs/core/marketdata/MarketDataCore.cpp:163-205`, `libs/core/marketdata/MarketDataCore.cpp:235-263`, `libs/core/marketdata/MarketDataCore.cpp:621-631`)

Refactor Suggestions
- Split into core-only engine (no Qt) + Qt-facing wrapper that forwards signals to GUI. Keep core API in `libs/core`, move Qt wrapper to `libs/gui` or `apps/`.
- Gate all `m_products` mutations behind `net::post(m_strand, ...)` or protect with a mutex; avoid mixed-thread access.
- Remove redundant `m_lastSeqByProduct.clear()` outside the mutex, or make all accesses strand-only.
- Confirm `BeastWsTransport` threading contract and enforce it by routing all calls through the strand.

Re-architecture
- Medium: boundary fix (core/GUI separation) is the biggest required structural change here.
