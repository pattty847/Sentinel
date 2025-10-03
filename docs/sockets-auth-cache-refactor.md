# SENTINEL MARKET DATA REFACTOR — GOD PROMPT (FOR CURSOR/GPT-5)

You are refactoring Sentinel’s real-time market-data stack to remove the MarketDataCore “God class”, improve reliability, and make the system modular, testable, and exchange-agnostic — **without regressing performance**.

You must work in **phases**, gathering context first, then making surgical fixes, then extracting modules behind crisp interfaces, and finally wiring tests. Use the directory plan and acceptance criteria below. Generate your own sub-TODOs at each phase and check them off.

---

## GLOBAL NON-NEGOTIABLES (READ THIS CLOSELY)

* **No perf regression on hot path** (parsing → cache update → Qt signal).
* **No behavior change** for the GUI signal surface.
* **Async errors must never crash**; route them to unified error emissions.
* **Reset reconnect backoff on successful WS handshake** (bug fix).
* **Protect queued Qt lambdas from dangling `this`** (use `QPointer`).
* **Subscriptions are declarative**: keep a desired set; replay on connect; never drop requests if WS is down.
* **JWT creation is injectable/testable** (no hardcoded path assumptions).
* **LiveOrderBook bounds are configurable**, not magic constants.
* **Keep logging categories and throttling behavior intact**.

---

## PHASE 0 — DISCOVERY & CONTEXT INDEX (SEMANTIC SEARCH + GREP)

Before writing code, build a **context index**. Create a scratch doc in the branch with:

1. **Locate all market-data terms & channels**

* Grep for channel strings (exact AND case-insensitive):

  * `"market_trades"`, `"level2"`, `"l2_data"`, `"subscriptions"`, `"snapshot"`, `"update"`, `"offer"`, `"ask"`, `"bid"`.
* Note all send/receive points and the structure of messages (who constructs, who parses).

2. **Find all error pathways** (must be centralized)

* Grep in `MarketDataCore.*` for error sources: `onResolve`, `onConnect`, `onSslHandshake`, `onWsHandshake`, `onRead`, `onWrite`, `onClose`, `scheduleReconnect`.
* Record which ones currently print to `std::cerr`, which use `sLog_*`, which (don’t) emit `errorOccurred`.

3. **Signal surface inventory**

* Find definitions & all emits: `tradeReceived`, `liveOrderBookUpdated`, `connectionStatusChanged`, `errorOccurred`.
* Map which thread context they’re emitted from (Qt queued), and **where the GUI expects them** (listener locations).

4. **Authenticator usage map**

* Grep for `createJwt()` calls, who calls, and when. Identify hardcoded paths/assumptions.
* Confirm current JWT fields and expiry logic.

5. **DataCache & LiveOrderBook usage map**

* Identify all code paths calling: `addTrade`, `initializeLiveOrderBook`, `updateLiveOrderBook`, `getDirectLiveOrderBook`, `getLiveOrderBook`.
* Note **hardcoded price range** logic for LiveOrderBook (BTC defaults, etc).
* Note any **OOB logs** (out-of-bounds updates dropped).

6. **Write queue & reconnect behavior**

* Confirm the **write queue** logic and where it’s cleared.
* Identify what happens to queued subscription writes during reconnects (should be: drop old frames, but **replay desired subscriptions** cleanly after WS handshake).

7. **Logging categories**

* Confirm usage of `logApp`, `logData`, `logRender`, `logDebug`. Keep category semantics stable.

8. **CMake linkage & include paths**

* Note how `libs/core` and `libs/gui` targets are defined and linked. Anticipate moving files into `libs/core/marketdata/**` and reflect that in CMake with minimal churn.

**Output of Phase 0:** a short index with file paths, callsites, and exact lines or snippets you’ll touch.

---

## PHASE 1 — SURGICAL FIXES (NO REFACTOR YET)

Apply zero-risk corrections inside current structure. This stabilizes runtime behavior before moving files.

1. **Unified error emission**

* Implement a small helper on `MarketDataCore`:

  ```cpp
  inline void MarketDataCore::emitError(QString msg) {
      QPointer<MarketDataCore> self(this);
      QMetaObject::invokeMethod(this, [self, m = std::move(msg)]{
          if (!self) return;
          emit self->errorOccurred(m);
          emit self->connectionStatusChanged(false);
      }, Qt::QueuedConnection);
  }
  ```
* Replace all raw `std::cerr` / scattered `sLog_Error` in error branches with `emitError(...)` **in addition to logging**.

2. **Backoff reset on success**

* In `onWsHandshake` after success, set `m_backoffDuration = std::chrono::seconds(1);` (and log it).

3. **QPointer guard all Qt-queued lambdas**

* Anywhere you do `QMetaObject::invokeMethod(this, [this](){...}, Qt::QueuedConnection)`, wrap `this` in `QPointer` and early-return if null.

4. **Subscription replay on connect**

* On successful WS handshake, automatically send a **single consolidated “subscribe”** for current `m_products`.
* If `sendSubscriptionMessage` is called while WS closed, **stage** the request and flush it after connect (or just rely on replay from desired set). Avoid double-subscribes.

5. **Write queue hygiene on reconnect**

* When scheduling reconnect, **clear** `m_writeQueue` and set `m_writeInProgress=false`. Desired subscriptions will be replayed post-handshake.

6. **Channel constants**

* Replace string literals with constants (e.g., `ch::kTrades`, `ch::kL2Sub`, `ch::kL2Data`) and normalize “offer” vs “ask”.

7. **Heartbeat/ping**

* Add a 20–30s ping/keepalive in the transport loop to detect half-opens faster than TCP timeouts.

**Acceptance for Phase 1:**

* Manual run: kill network; watch `errorOccurred` fire; reconnect resets backoff; upon recovery, current subscriptions are re-added; no crashes on shutdown.

---

## PHASE 2 — DIRECTORY MIGRATION (NO LOGIC CHANGES)

Create domain-parent and subfolders inside core:

```
libs/core/marketdata/
  ws/
  dispatch/
  auth/
  cache/
  sinks/
```

**Move files (minimal churn):**

* `MarketDataCore.*` → `libs/core/marketdata/` (temp home before split)
* `TradeData.h` → `libs/core/marketdata/model/TradeData.h` (create `model/`)
* `DataCache.*` → `libs/core/marketdata/cache/`
* `Authenticator.*` → `libs/core/marketdata/auth/`
* `MessageParser.hpp` (if used) → `libs/core/marketdata/dispatch/` (or superseded)
* Leave `SentinelLogging.*` in `libs/core/` (shared infra)

**CMake:**

* Add a `CMakeLists.txt` under `libs/core/marketdata` to build a `marketdata` static lib.
* Parent `libs/core/CMakeLists.txt` links `marketdata` into the existing `core` target or directly into the app as you prefer.
* Update include paths (`target_include_directories`) so includes become `#include "marketdata/.../Header.hpp"`.

**Acceptance for Phase 2:** build still passes, app runs as before.

---

## PHASE 3 — MODULE EXTRACTION (SPLIT THE GOD CLASS)

Create three new modules + a sink adapter. Keep the same external Qt signal surface in `MarketDataCore` (glue only).

### 3.1 WsTransport (marketdata/ws)

**Purpose:** Pure transport: connect, read, write, close, backoff, heartbeat. Knows nothing about Coinbase JSON.

**Interface:**

```cpp
class WsTransport {
public:
  using MessageCb = std::function<void(std::string_view)>;
  using StatusCb  = std::function<void(bool)>;
  using ErrorCb   = std::function<void(std::string)>;

  explicit WsTransport(net::io_context&, ssl::context&);

  void connect(std::string host, std::string port, std::string target);
  void close();
  void send(std::string msg);              // strand-serialized

  void onMessage(MessageCb);
  void onStatus(StatusCb);
  void onError(ErrorCb);
};
```

* Internally: owns resolver, ws stream, strand, timers, work_guard, write queue, jittered backoff, ping timer.
* On successful handshake → calls `StatusCb(true)` and **resets backoff to 1s**.
* On any error → calls `ErrorCb(...)` and `StatusCb(false)`.

### 3.2 MessageDispatcher (marketdata/dispatch)

**Purpose:** **Pure** JSON → typed events (no I/O, no Qt). Parse and return vector of domain events.

**Types:**

```cpp
using Event = std::variant<TradeEvent, BookSnapshotEvent, BookUpdateEvent, SubscriptionAckEvent, ProviderErrorEvent>;

struct DispatchResult { std::vector<Event> events; };

class MessageDispatcher {
public:
  static DispatchResult parse(const nlohmann::json& j);
};
```

* Encapsulate Coinbase quirks: channels `"market_trades"`, `"l2_data"`, event types `"snapshot" | "update"`, sides `"bid" | "offer"/"ask"`.
* **No allocations beyond JSON DOM**; reuse strings/views where practical.

### 3.3 SubscriptionManager (marketdata/ws or marketdata/dispatch)

**Purpose:** Keep a **desired product set** and generate minimal subscribe/unsubscribe frames; replay on connect.

**Interface:**

```cpp
class SubscriptionManager {
public:
  void setDesiredProducts(std::vector<std::string>);
  const std::vector<std::string>& desired() const;

  // Build wire messages (two channels): level2 + market_trades
  std::vector<std::string> buildSubscribeMsgs(std::string jwt) const;
  std::vector<std::string> buildUnsubscribeMsgs(std::string jwt) const;
};
```

### 3.4 DataCacheSinkAdapter (marketdata/sinks)

**Purpose:** Bridge parsed events → `DataCache` calls; isolates DataCache from parsing details and lets you swap sinks later.

**Interface:**

```cpp
class IMarketDataSink {
public:
  virtual void onTrade(const Trade&) = 0;
  virtual void onBookSnapshot(...)=0;
  virtual void onBookUpdate(...)=0;
  virtual ~IMarketDataSink() = default;
};

class DataCacheSinkAdapter : public IMarketDataSink {
  DataCache& cache_;
  // ctor takes ref or shared_ptr
};
```

### 3.5 Slim `MarketDataCore` (marketdata/)

* Owns `WsTransport`, `SubscriptionManager`, `IAuthenticator`, and a sink adapter to `DataCache`.
* Hooks:

  * `WsTransport::onMessage` → parse → visit events → call sink → emit Qt signals.
  * `WsTransport::onStatus(bool)` → emit `connectionStatusChanged`. On true, **replay subscriptions** with fresh JWT.
  * `WsTransport::onError(string)` → emit `errorOccurred`.
* Public API remains:

  * `start()`, `stop()`
  * `subscribeToSymbols(...)`, `unsubscribeFromSymbols(...)`
  * Signals: `tradeReceived`, `liveOrderBookUpdated`, `connectionStatusChanged`, `errorOccurred`

**Acceptance for Phase 3:** Build and run; GUI behavior identical; disconnect/reconnect path and subscription replay verified.

---

## PHASE 4 — CONFIG & AUTH IMPROVEMENTS

1. **Authenticator injection**

* Define `IAuthenticator` interface; implement `CoinbaseAuthenticator` using your current logic.
* Accept `std::shared_ptr<IAuthenticator>` in `MarketDataCore` ctor.
* Remove any hardcoded file path assumptions; provide path via config or dependency injection.

2. **LiveOrderBook configuration**

* Introduce a config struct (symbol → min/max/tick) or a strategy to set bounds at snapshot time.
* Replace magic constants inside `DataCache` with this config.

3. **Logging**

* Keep the same categories and throttling semantics (don’t regress logs).
* Replace scattered `std::cerr` with your logging macros + `errorOccurred`.

---

## PHASE 5 — TESTS & FIXTURES

Add unit tests under `libs/core/tests/` (or your test location). You can stub Beast or inject fakes for transport.

1. **Message Dispatcher** (pure, fastest ROI)

* Golden JSON samples (trades, snapshot, updates, sub ack, provider error) → assert `Event` sequence and fields.
* Include side normalization (“offer” → “ask”).
* Include failure cases (malformed JSON → empty events + error event).

2. **Subscription Manager**

* Desired set transitions: `[] → [A,B] → [B,C]` produce minimal unsub/ sub frames.
* Deterministic output (order stable).

3. **Transport (smoke with fake stream)**

* Simulate resolve/connect/read errors; assert error and status callbacks ordering; assert backoff reset on success.

4. **DataCacheSinkAdapter**

* Snapshot then updates produce expected book counts; OOB updates are dropped; timestamp = exchange time.

**Acceptance for Phase 5:** Tests compile and pass; manual run still good.

---

## TARGET DIRECTORY MAP (FINAL)

```
libs/core/
  logging/
    SentinelLogging.hpp
    SentinelLogging.cpp

  marketdata/
    model/
      TradeData.h
      OrderBookEvents.hpp  // new
      Errors.hpp           // new (string codes or enum)
    auth/
      IAuthenticator.hpp
      CoinbaseAuthenticator.hpp
      CoinbaseAuthenticator.cpp
    cache/
      DataCache.hpp
      DataCache.cpp
    dispatch/
      MessageDispatcher.hpp
      MessageDispatcher.cpp
      CoinbaseParsers.hpp   // optional split
      CoinbaseParsers.cpp   // optional split
    ws/
      WsTransport.hpp
      WsTransport.cpp
      ReconnectPolicy.hpp   // header-only policy
      SubscriptionManager.hpp
      SubscriptionManager.cpp
    sinks/
      IMarketDataSink.hpp
      DataCacheSinkAdapter.hpp
      DataCacheSinkAdapter.cpp
    MarketDataCore.hpp
    MarketDataCore.cpp
```

---

## CMAKE NOTES

* Add a `marketdata` static lib with subfolders as sources.
* `target_include_directories(marketdata PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})` so includes like `#include "marketdata/ws/WsTransport.hpp"` work.
* Link `marketdata` where `MarketDataCore` used to be linked.
* Ensure OpenSSL, Boost::asio, Boost::beast, Qt are linked in `marketdata` (or in parent if you centralize linkage).

---

## RISK CHECKS & ROLLBACK

* **Risk:** queued Qt lambdas targeting dead `this`. **Mitigation:** `QPointer` in all lambdas.
* **Risk:** subscription storms on reconnect. **Mitigation:** replay desired set exactly once after handshake; clear old write queue.
* **Risk:** LiveOrderBook memory blowout with bad bounds. **Mitigation:** config strategy; log and cap.
* **Rollback plan:** branch isolates changes; Phase-1 commits are safe to keep even if Phase-3 extraction is reverted.

---

## DONE = PASS CRITERIA

* Manual: unplug network → `errorOccurred` fires; `connectionStatusChanged(false)`; backoff climbs; replug → connects; backoff resets; subscriptions replay; GUI updates resume.
* Unit tests pass for dispatcher, subscription manager, sink, and transport smoke.
* `MarketDataCore` is slim glue; no JSON parsing or Beast calls.
* No perf hit on trade throughput or book updates.

---

## FILES & BEHAVIOR YOU MUST HONOR (READ THEM)

* **Trade & book models** (keep types/semantics; add events as needed for dispatcher):
  Trade struct, AggressorSide enum, LiveOrderBook methods and O(1) dense arrays.
* **Authenticator behavior** (JWT ES256; `kid`, `nonce`, `iss="cdp"`, short expiry): keep this contract; just inject it.
* **DataCache** (threading with `shared_mutex`, snapshot semantics, LiveOrderBook OOB logging & exchange timestamps): do not regress.
* **Logging categories** (app/data/render/debug; atomic-throttled macros): keep categories & usage.

---

## OPTIONAL NICE-TO-HAVES (IF TIME)

* Add a tiny **MetricsSink** that counts trades/sec and book updates/sec (plug in parallel to DataCacheSinkAdapter).
* Add a **ReplayTransport** (file-backed) for deterministic dev runs.
* Add **feature flag** to switch between Coinbase vs future provider (same dispatcher API with provider-specific parsers).