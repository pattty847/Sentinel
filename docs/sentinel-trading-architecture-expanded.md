# Sentinel Trading Engine — Expanded Architecture & Implementation Plan (v0.31)

## 0. Preface & Objectives

Sentinel already ships with Coinbase Advanced Trade authentication (`libs/core/marketdata/auth/Authenticator.cpp`) and live market data streaming for order books and trades through the MarketDataCore pipeline. The goal of this plan is to add a full trading stack—real trading plus paper trading—without regressing existing market data capabilities. The design follows the existing dock-style interface and pub/sub pattern that the market data stack uses, keeping business logic off the render thread while exposing clean signals/events for the GUI.

## 1. Five-Layer Citadel (Recap + Implementation Detail)

### L1 — Connectivity Layer (REST + WS)
- **Responsibility:** Pure network I/O for Coinbase REST + WebSocket. No business rules.
- **Key classes/files:**
  - `connectivity/CoinbaseRESTClient.{hpp,cpp}` — async Boost.Beast REST wrapper with retry and JWT header injection.
  - `connectivity/CoinbaseWSClient.{hpp,cpp}` — WS client for both market data (existing) and user channels, sharing auth logic with MarketDataCore.
  - `connectivity/JwtProvider.{hpp,cpp}` — thin wrapper around `Authenticator` to issue/refresh JWTs for both REST and WS (120s expiry cadence).
  - `connectivity/HeartbeatMonitor.{hpp,cpp}` — monitors WS heartbeats and triggers reconnect.
- **Behavior:**
  - REST: `placeOrder`, `cancelOrders`, `listOrders`, `listAccounts`, `listProducts`, `listFills` with exponential backoff.
  - WS: subscribes to `user`, `market_trades`, and `heartbeats`; forwards raw JSON to the dispatch bus.
  - Threading: All network I/O on Boost.Asio `io_context` + strand; Qt/GUI notified via signals only.

### L2 — Execution Engine (Order State Machine)
- **Responsibility:** Order lifecycle and routing between REST submissions and WS user-channel updates.
- **Key classes/files:**
  - `execution/OrderRegistry.{hpp,cpp}` — thread-safe store keyed by client order ID.
  - `execution/OrderStateMachine.{hpp,cpp}` — manages transitions `PENDING_NEW → NEW → PARTIALLY_FILLED → FILLED → DONE` with cancellation and rejection exits (`PENDING_NEW/NEW/PARTIALLY_FILLED → CANCELED`; `PENDING_NEW/NEW → REJECTED`).
  - `execution/ExecutionService.{hpp,cpp}` — façade API (`sendOrder`, `cancelOrder`, `replaceOrder`) plus callbacks for WS updates.
  - `execution/OrderRouter.{hpp,cpp}` — dispatches outbound orders to Coinbase REST or Paper Trader based on active mode.
- **Behavior:**
  - Receives WS user updates, normalizes into `OrderUpdate` model, and applies to registry.
  - Emits events on the internal bus: `orderAccepted`, `orderRejected`, `orderUpdated`, `orderFilled`, `orderCanceled`.
  - Never blocks the render thread; all heavy work on Asio worker.

### L3 — Portfolio & Account State
- **Responsibility:** Sentinel’s internal truth for balances, positions, PnL, and risk controls.
- **Key classes/files:**
  - `portfolio/AccountState.{hpp,cpp}` — balances, holds, available funds.
  - `portfolio/PositionBook.{hpp,cpp}` — per-product positions and PnL computation.
  - `portfolio/RiskManager.{hpp,cpp}` — buying power checks, kill switch, order size validation.
  - `portfolio/AccountSynchronizer.{hpp,cpp}` — sync loop to Coinbase accounts endpoint and WS delta application.
- **Behavior:**
  - Listens to fills + account update events to keep state in sync.
  - Exposes read-only snapshots to GUI via dock/pub-sub interfaces.

### L4 — Synthetic Orders / Paper Trader Engine
- **Responsibility:** Local mock exchange and advanced order types that mimic Coinbase contract.
- **Key classes/files:**
  - `paper/PaperExchange.{hpp,cpp}` — receives the same `PlaceOrder`/`CancelOrder` payloads as Coinbase.
  - `paper/PaperOrderBook.{hpp,cpp}` — simple or depth-based matching with slippage and partial fills.
  - `paper/PaperLedger.{hpp,cpp}` — balances, PnL, and fee simulation.
  - `paper/SyntheticOrders.{hpp,cpp}` — iceberg, bracket, OCO, trailing stop, TWAP orchestrators that expand into native orders.
- **Behavior:**
  - Publishes events on the same bus as real trading so L2/L3 are oblivious to the backing engine.
  - Configurable latency, fill probability, and spread-based slippage knobs for testing.

### L5 — UI / Terminal Integration
- **Responsibility:** Trading widgets subscribing to the event bus; no direct exchange calls.
- **Key widgets/docks:** Order Ticket, Orders/Fills panel, Positions panel, Account panel, Notifications, Chart/Heatmap click-to-trade overlay, Execution audit log.
- **Behavior:**
  - Qt signals connect GUI docks to L2/L3 data streams (pub/sub).
  - All user actions call `ExecutionService` façade methods; responses come back via events.

## 2. Data Models (C++20, nlohmann::json)
- `CoinbaseModels.hpp` defines:
  - `enum class OrderSide { Buy, Sell }`, `enum class OrderType { Market, Limit, Stop, StopLimit }` (native Coinbase types; synthetic-only types live under Paper/Synthetic), `enum class OrderStatus { PendingNew, New, PartiallyFilled, Filled, Canceled, Rejected, Done }`.
  - `struct MoneyAmount { std::string currency; std::string amount; };` (use string for decimal safety).
  - `struct Order`, `OrderUpdate`, `Fill`, `Balance`, `Product`, `Heartbeat` with `std::optional<>` for nullable Coinbase fields.
- `from_json`/`to_json` helpers for each model (placed in `CoinbaseModels.cpp`).
- Mapping helpers: `OrderUpdate toStateChange(const nlohmann::json& wsUserMessage)`.

## 3. Event Bus & Pub/Sub Interfaces
- **Core interface:** `struct TradeEventBus { Signal<OrderUpdate> orderUpdated; Signal<Fill> fillReceived; Signal<Balance> balanceChanged; Signal<Heartbeat> heartbeat; };`
- **Implementation:**
  - Backed by Qt signals/slots for GUI consumers and lightweight observer list for worker-side consumers.
  - Thread-safe post from Asio strand to Qt via `QMetaObject::invokeMethod` to avoid GUI thread access violations.
- **Integration:**
  - MarketDataCore remains separate but can publish `market_trades` into the same bus for validation/monitoring overlays.

## 4. Threading & Process Model
- **Asio Worker Thread(s):** Connectivity, Execution Engine, Paper Trader, and Portfolio updates run on Asio `io_context` with strand serialization.
- **Qt Render Thread:** GUI docks/widgets only. Receives events through queued Qt signals; never performs I/O.
- **Timers:**
  - JWT refresh timer (~90s) to renew tokens before 120s expiry.
  - Heartbeat watchdog timer per WS connection; triggers reconnect on missed beats.
- **Shutdown:** Graceful stop sequence: stop order submissions → flush pending events → close WS/REST → join worker thread.

## 5. File & Module Layout (proposed under `libs/core/trading`)
```
libs/core/trading/
  connectivity/
    CoinbaseRESTClient.{hpp,cpp}
    CoinbaseWSClient.{hpp,cpp}
    JwtProvider.{hpp,cpp}
    HeartbeatMonitor.{hpp,cpp}
  model/
    CoinbaseModels.{hpp,cpp}
  execution/
    OrderRegistry.{hpp,cpp}
    OrderStateMachine.{hpp,cpp}
    ExecutionService.{hpp,cpp}
    OrderRouter.{hpp,cpp}
  portfolio/
    AccountState.{hpp,cpp}
    PositionBook.{hpp,cpp}
    RiskManager.{hpp,cpp}
    AccountSynchronizer.{hpp,cpp}
  paper/
    PaperExchange.{hpp,cpp}
    PaperOrderBook.{hpp,cpp}
    PaperLedger.{hpp,cpp}
    SyntheticOrders.{hpp,cpp}
  ui/
    TradingDockCoordinator.{hpp,cpp}
    widgets/ (OrderTicket, OrdersPanel, PositionsPanel, AccountPanel, Notifications, ChartOverlay)
  bus/
    TradeEventBus.{hpp,cpp}
```

## 6. Phase-by-Phase Implementation Plan

### Phase 1 — Models & JSON Contracts
- Implement `CoinbaseModels` (models + inline `from_json`/`to_json` helpers) with exhaustive unit tests using recorded Coinbase payloads.
- Add validation helpers: `validateOrderRequest`, `parseHeartbeat`, `parseUserMessage`.
- Deliverable: header-only models ready for both REST and WS clients; tests in `tests/trading/models`.

### Phase 2 — Connectivity Layer
- Build REST and WS clients leveraging the existing `Authenticator` for JWTs.
- Implement retry/backoff, heartbeat monitoring, and auto-reconnect for user channel.
- Deliverable: `ExecutionService` can log "Connected to Coinbase Advanced Trade" after receiving heartbeats; integration test with mocked WS server.

### Phase 3 — Execution Engine
- Wire `ExecutionService` to REST submissions and WS user updates.
- Implement order registry + state machine transitions with deduplicated client order IDs.
- Deliverable: programmatic API to place/cancel orders (no UI) with deterministic unit tests using mocked REST/WS.

### Phase 4 — Portfolio Engine
- Implement `AccountState`, `PositionBook`, `RiskManager`, and `AccountSynchronizer`.
- Add risk hooks to `ExecutionService` (pre-flight checks) and update portfolio on fills.
- Deliverable: internal account mirrors Coinbase balances and positions; kill switch stops new orders when triggered.

### Phase 5 — Paper Trader (Mock Exchange)
- Implement `PaperExchange` and `PaperOrderBook` with configurable fill models (immediate, partial, delayed) and slippage.
- Ensure it emits the same events as Coinbase so UI and execution logic are unchanged.
- Deliverable: environment flag to toggle between real and paper modes; regression tests comparing fill events between modes.

### Phase 6 — Synthetic / Advanced Orders
- Add orchestrators for iceberg, bracket, OCO, client-managed trailing stop, and TWAP; expand into native child orders with linkage.
- Deliverable: strategy definitions plus lifecycle management (e.g., cancel siblings on fill for OCO).

### Phase 7 — UI Integration
- Build trading docks/widgets that subscribe to the bus: order ticket, orders/fills, positions, account, notifications, chart overlay.
- Add monitoring views for trade streams (leveraging existing order book + trade sinks) to visualize execution quality.
- Deliverable: end-to-end trading from GUI with feedback and audit logging.

### Phase 8 — Observability & Safety
- Logging: structured logs for all order lifecycle events and portfolio changes (reuse `SentinelLogging`).
- Metrics: counters for orders placed/canceled/rejected, WS reconnects, PnL, and latency histograms.
- Safety: kill switch UI toggle, max position limits, rate-limit guardrails.

## 7. Integration Notes
- **Authenticator reuse:** `JwtProvider` wraps `Authenticator` to share key loading and ES256 signing logic for both REST and WS.
- **Market data reuse:** Existing MarketDataCore order book/trade streams continue to feed UI and can optionally validate fills (e.g., slippage analysis) by subscribing to the bus.
- **Dock/pub-sub alignment:** Trading docks use the same dock interface style as existing widgets, subscribing to bus signals for updates and invoking `ExecutionService` methods for actions.
- **Testing strategy:**
  - Unit tests per module (models, execution state machine, portfolio math, paper fills).
  - Integration tests with mocked REST/WS servers and with Paper Trader.
  - UI smoke tests for dock wiring (Qt test framework).

## 8. Risk & Mitigation Checklist
- **JWT expiry drift:** refresh at 90s; log clock skew.
- **WS disconnects:** heartbeat watchdog + backoff reconnect; persist pending REST submissions (place/cancel/replace) and unconfirmed client-order IDs, then on reconnect reconcile by querying order status (idempotent by client order ID) before replaying; surface a toast/banner in the UI while pending actions are in-flight and clear once reconciled.
- **Order/portfolio divergence:** periodic account sync plus idempotent application of WS deltas; if divergence is detected, raise a user alert, pause new submissions until a resync completes, and auto-correct local snapshots from REST results before re-enabling trading.
- **Paper vs real modes:** explicit mode flag and visual indicator; segregated configs to avoid accidental real trades.

## 9. Deliverable Summary
- New trading module under `libs/core/trading` with connectivity, execution, portfolio, paper, synthetic, UI, and bus submodules.
- Full documentation (this file) in `docs/sentinel-trading-architecture-expanded.md` for onboarding engineers.
- Ready-to-implement phase plan enabling safe rollout of real and paper trading inside Sentinel.
