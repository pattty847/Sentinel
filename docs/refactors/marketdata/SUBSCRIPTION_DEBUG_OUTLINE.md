# Subscription Flow Debugging - Issue Outline

## Problem Statement

The Sentinel trading terminal successfully connects to Coinbase Advanced Trade WebSocket API, but subscription messages are not being sent after the user clicks "Subscribe". The connection establishes (status shows "Connected"), but no market data flows because the subscription handshake never completes.

## Current Symptoms

### What Works
- ✅ WebSocket connection establishes successfully (`wss://advanced-trade-ws.coinbase.com/`)
- ✅ TLS/SSL handshake completes (no certificate errors)
- ✅ Transport status callback fires: `onStatus(true)` → `m_connected = true`
- ✅ Status bar shows "Connected"
- ✅ `subscribeToSymbols()` is called and logs correctly
- ✅ `sendSubscriptionMessage()` is called and logs "posting to strand"

### What Doesn't Work
- ❌ Subscription lambda **never executes** (no "lambda executing" log)
- ❌ No subscription frames are sent to Coinbase
- ❌ No `SubscriptionAckEvent` received from exchange
- ❌ No market data (trades/order book) flows into `DataCache`
- ❌ Heatmap remains empty (no data to render)

## Code Flow Analysis

### Expected Flow
```
GUI Thread (MainWindowGPU)
  ↓ onSubscribe() called
  ↓ subscribeToSymbols({"BTC-USD"})
MarketDataCore (GUI thread)
  ↓ sendSubscriptionMessage("subscribe", symbols)
  ↓ net::post(m_strand, lambda)
IO Context Thread (worker thread)
  ↓ Lambda executes on strand
  ↓ Build subscription frames via SubscriptionManager
  ↓ m_transport->send(frame)
BeastWsTransport (strand)
  ↓ Queue message, call doWrite()
  ↓ async_write() to WebSocket
Coinbase Exchange
  ↓ Receives subscription frame
  ↓ Sends SubscriptionAckEvent
MarketDataCore::dispatch()
  ↓ Parses SubscriptionAckEvent
  ↓ Market data starts flowing
```

### Actual Flow (Broken)
```
GUI Thread
  ↓ subscribeToSymbols() ✅
  ↓ sendSubscriptionMessage() ✅
  ↓ net::post(m_strand, lambda) ✅ (call completes)
IO Context Thread
  ↓ Lambda NEVER EXECUTES ❌
  ↓ (No further execution)
```

## Technical Context

### Architecture
- **Language**: C++20
- **Framework**: Qt6 (GUI), Boost.Asio (networking)
- **Threading Model**: 
  - GUI thread: Qt event loop
  - Worker thread: Boost.Asio `io_context::run()` on dedicated thread
  - Communication: Qt signals/slots (QueuedConnection) + Boost.Asio strand

### Key Components

#### MarketDataCore
- Owns `net::io_context m_ioc` (runs on `m_ioThread`)
- Owns `net::strand<net::io_context::executor_type> m_strand{m_ioc.get_executor()}`
- `sendSubscriptionMessage()` called from GUI thread
- Posts work to `m_strand` for thread-safe execution

#### BeastWsTransport
- Owns its own `net::strand<net::io_context::executor_type> strand_{ioc.get_executor()}`
- Uses `net::post(strand_, ...)` for `send()` method
- Transport's strand operations work correctly (connection succeeds)

### Strand Initialization
```cpp
// MarketDataCore.hpp
net::io_context m_ioc;
net::strand<net::io_context::executor_type> m_strand{m_ioc.get_executor()};

// MarketDataCore::start()
m_workGuard.emplace(m_ioc.get_executor());
m_ioc.restart();
m_ioThread = std::thread(&MarketDataCore::run, this);
m_ioc.run(); // Blocks on worker thread
```

## What We've Tried

### Attempt 1: Direct Strand Posting
```cpp
net::post(m_strand, [this, type, symbolsCopy]() {
    // Lambda never executes
});
```
**Result**: ❌ Lambda never executes

### Attempt 2: Post to io_context with Strand Binding
```cpp
boost::asio::post(m_ioc, boost::asio::bind_executor(m_strand, [this, type, symbolsCopy]() {
    // Lambda never executes
}));
```
**Result**: ❌ Lambda never executes (current attempt)

### Attempt 3: Added Diagnostic Logging
- Changed `sLog_DataN(1, ...)` to `sLog_Error(...)` (always-on, no throttling)
- Added logs before/after `net::post` call
- Added logs at start of lambda

**Result**: 
- ✅ See "About to call net::post" log
- ✅ See "net::post call completed" log  
- ❌ Never see "lambda executing" log

### Attempt 4: Exception Handling
- Wrapped `net::post` call in try/catch
- Wrapped lambda body in try/catch

**Result**: ❌ No exceptions thrown

### Attempt 5: Verified io_context is Running
- Added log in `MarketDataCore::run()`: "IO context running"
- Confirmed connection works (proves io_context processes work)

**Result**: ✅ io_context is definitely running

## Key Observations

1. **Strand posting from transport callbacks works**: When `onStatus(true)` fires (from transport's strand), posting to `m_strand` works:
   ```cpp
   m_transport->onStatus([this](bool up){
       // This runs on transport's strand
       net::post(m_strand, [this]() {
           replaySubscriptionsOnConnect(); // ✅ This executes
       });
   });
   ```

2. **Strand posting from GUI thread doesn't work**: When `sendSubscriptionMessage()` is called from GUI thread:
   ```cpp
   void MarketDataCore::sendSubscriptionMessage(...) {
       // Called from GUI thread
       net::post(m_strand, [this, ...]() {
           // ❌ This NEVER executes
       });
   }
   ```

3. **Both use same pattern**: Both use `net::post(m_strand, ...)` but only one works.

4. **io_context is running**: Connection succeeds, so `m_ioc.run()` is definitely processing work.

5. **No exceptions**: No errors logged, work appears to be queued but never executed.

## Questions for Research

1. **Boost.Asio Strand Behavior**: 
   - Can `net::post(strand, ...)` be called from a thread that's not running the io_context?
   - Does posting to a strand require the posting thread to be associated with the io_context?
   - Is there a difference between posting from within a strand callback vs. posting from an external thread?

2. **Thread Safety**:
   - Is `net::post(strand, ...)` thread-safe when called from non-io_context threads?
   - Does Boost.Asio require special handling when posting from Qt GUI thread?

3. **Executor Binding**:
   - Does `boost::asio::bind_executor(strand, handler)` work correctly when posted from external threads?
   - Should we use `boost::asio::post(io_context, bind_executor(strand, handler))` instead?

4. **Work Guard**:
   - Does the `executor_work_guard` affect strand posting behavior?
   - Could the work guard be preventing strand work from executing?

5. **Strand vs Direct Posting**:
   - Should we post directly to `io_context` and handle synchronization differently?
   - Is there a known issue with strand posting from non-io_context threads in Boost.Asio?

6. **Qt + Boost.Asio Integration**:
   - Are there known issues with posting Boost.Asio work from Qt GUI thread?
   - Should we use Qt's `QMetaObject::invokeMethod` to marshal to io_context thread first?

## Code References

### MarketDataCore.cpp (lines 245-320)
- `sendSubscriptionMessage()` - Entry point from GUI thread
- Current implementation uses `boost::asio::post(m_ioc, boost::asio::bind_executor(m_strand, ...))`

### MarketDataCore.cpp (lines 76-98)
- `onStatus` callback - Works correctly, posts to `m_strand` successfully

### MarketDataCore.cpp (lines 161-180)
- `start()` method - Initializes io_context and worker thread

### MarketDataCore.cpp (lines 206-213)
- `run()` method - Worker thread entry point, runs `m_ioc.run()`

## Environment

- **OS**: Windows 10 (Build 26200)
- **Compiler**: MSVC (Visual Studio)
- **Boost Version**: Latest via vcpkg
- **Qt Version**: Qt6
- **Build**: Debug configuration

## Next Steps

After research identifies the root cause:
1. Implement the fix based on research findings
2. Verify lambda executes (should see "lambda executing" log)
3. Verify subscription frames are sent
4. Verify `SubscriptionAckEvent` is received
5. Verify market data flows
6. Clean up diagnostic logging

## Additional Context

The codebase follows a strict architecture:
- Core layer (`libs/core`) - Pure C++, no Qt
- GUI layer (`libs/gui`) - Qt/QML
- MarketDataCore bridges between core (Boost.Asio) and GUI (Qt)

The subscription flow worked before recent refactoring (dataflow changes, TLS fixes for MSVC), but the exact point of breakage is unclear.

