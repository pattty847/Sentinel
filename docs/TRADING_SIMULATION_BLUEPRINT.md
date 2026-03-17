# Trading Simulation Blueprint

## Goal

Build one abstract trading simulation stack that can power:

1. live manual paper trading
2. live algo paper trading
3. historical replay / backtesting
4. later visual replay on the heatmap and chart

The design should stay generic enough that new algos, new execution models, and new data sources can plug in without rewriting the broker or UI integration.

## Core Principle

There is one execution core, with different operating modes layered on top of it.

Shared core:
- market events
- strategy intents
- broker state
- execution events
- execution model

Variable pieces:
- event source: live stream vs replay file vs later persisted datasets
- execution model: trade-driven now, book-aware later, queue-aware later
- UI mode: live trading session vs replay session

## Operating Modes

### 1. Live Manual Paper Trading

User actions place market and limit orders into the simulation broker.

Inputs:
- live trade stream
- later live order book

Outputs:
- order lifecycle
- fills
- positions
- realized/unrealized PnL
- chart overlays and paper-trading widget updates

### 2. Live Algo Paper Trading

Strategies such as Avendella consume the same live market-event stream and submit intents into the same broker.

Inputs:
- live trade stream
- later live order book

Outputs:
- algo orders and fills
- inventory / PnL
- chart overlays
- algo-specific diagnostics and controls

### 3. Historical Replay / Backtesting

Replay historical market-event files through the same broker and strategy interfaces.

Inputs:
- historical trade files now
- historical book files later

Outputs:
- order/fill logs
- PnL curve
- summary metrics
- later chart/heatmap visual replay

### 4. Visual Replay

Use recorded trade/book data to drive the same chart and heatmap views in replay mode.

Planned scope:
- replay candles and trade progression
- later replay heatmap/book evolution
- show strategy orders, fills, and state during playback

This should reuse replay session data and chart overlays rather than inventing a second visualization path.

## Architecture

### A. Market Event Layer

Canonical event family:
- `TradeEvent`
- reserved future `BookEvent`
- `MarketEvent`

Responsibilities:
- normalize live and historical inputs into one ordered stream
- keep timestamps deterministic
- stay transport-agnostic

### B. Strategy Layer

Canonical strategy contract:
- receives market events plus broker snapshot
- emits `OrderIntent`
- receives `ExecutionEvent` feedback

Requirements:
- strategy plug-and-play
- no GUI coupling
- reusable across live and replay

Current policy:
- existing `IAlgo` strategies can be adapted instead of rewritten

### C. Broker Layer

Canonical broker responsibilities:
- accept intents
- own orders, positions, and PnL state
- delegate fill decisions to an execution model
- emit execution events and result logs

This is the shared core for manual paper trading, live algos, and replay.

### D. Execution Model Layer

Pluggable fill-policy contract.

Planned models:
- `TradeDrivenExecutionModel` - current v1
- `BookAwareAggressiveExecutionModel` - future
- `LiveOrderBookExecutionModel` - future
- `QueueAwarePassiveExecutionModel` - future

Execution realism is a policy choice, not a broker rewrite.

### E. Session Layer

Session types:
- live paper session
- replay session

Responsibilities:
- choose the market-event source
- own the clock model
- expose session state to the UI

### F. UI Layer

The UI should consume broker/session outputs, not own execution logic.

Planned UI surfaces:
- Paper Trading dock
- chart overlays
- replay controls
- later heatmap/chart playback mode

## Phases

### Phase 1: Trade-Driven Shared Core

Scope:
- trade-event replay
- reusable broker
- pluggable execution model seam
- strategy adapter
- CLI backtest runner

Status:
- mostly complete

### Phase 2: Unify Live Paper Trading on the Shared Broker

Scope:
- route manual paper trades through the new broker
- route live algo paper trading through the new broker
- keep current UI, change the execution backend

Status:
- not started

### Phase 3: Persistent Historical Trade Capture

Scope:
- confirm what is already persisted vs memory-only
- capture long-lived trade history in a reusable dataset format
- enable meaningful replay windows without needing uptime accumulation

Status:
- not started

### Phase 4: Replay UI

Scope:
- replay session controls
- reuse existing chart widgets first
- later add dedicated replay tooling if needed

Status:
- not started

### Phase 4a: Live Paper Trading UX Polish

Scope:
- make manual paper trading feel real before broad replay work
- prioritize forward testing while historical capture is still sparse
- improve on-chart order and position interactions

Planned scope:
- resting limit orders render as strong horizontal lines
- filled orders transition from resting/pending styling to position styling
- show live close / mark cleanly in the paper-trading module
- show position pill with PnL dollars / percent
- add draggable TP / SL handles that create linked risk orders

Status:
- mostly complete

### Phase 5: Book-Aware Execution

Scope:
- use `LiveOrderBook` for aggressive fill realism first
- add historical book replay later
- defer queue modeling until after aggressive/book-aware fills work

Status:
- not started

### Phase 6: Visual Heatmap / Chart Replay

Scope:
- replay trade data through charts
- later replay heatmap/book progression
- show orders, fills, and strategy state in sync with playback

Status:
- not started

## Decisions Locked In

- Use one shared execution stack for live paper, algo paper, and replay.
- Keep strategies abstract and plug-and-play.
- Use trade-driven execution first.
- Add book-aware execution later as a new execution model.
- Migrate manual and algo live paper trading onto the shared broker before building book-aware fills.
- Build replay UI as a control layer on top of existing chart widgets before inventing a separate replay screen.
- Start durable data capture with trades before order-book persistence.

## Completed So Far

- [x] Basic paper-trading engine exists
- [x] AvendellaMM live algo path exists
- [x] Paper Trading dock exists
- [x] Algo overlays render on the chart / heatmap path
- [x] Trade-replay backtest core exists
- [x] Pluggable execution-model seam exists
- [x] Existing `IAlgo` strategies can run through replay via adapter
- [x] CLI backtest runner exists
- [x] Live server trading now routes manual orders and Avendella through the shared simulation broker
- [x] Backtest runner can read real captured hourly trade logs from `data/market/.../*.bin`
- [x] Manual paper trading now has renderer-backed order/position overlays with live mark, entry-price pill, and forward-testing polish
- [x] Manual paper trading now has server-backed TP/SL brackets with staged drag, confirm/discard, and OCO-style clearing

## Near-Term Next Work

- [x] Move live manual paper trading onto the shared simulation broker
- [x] Move live algo paper trading onto the shared simulation broker
- [x] Add a reader for existing `data/market/.../*.bin` trade logs into `MarketEvent` replay
- [ ] Build a thin replay path from those trade logs before broader replay UI work
- [ ] Polish TP/SL interaction and visuals now that the manual-only bracket system is in place

## Later Work

- [ ] Add `BookEvent` path and book-aware execution model
- [ ] Define durable historical trade dataset format once continuous capture is real
- [ ] Add replay controls in the UI
- [ ] Drive chart overlays from replay results
- [ ] Add heatmap visual replay from recorded data
- [ ] Extend risk orders beyond the first manual-only single-bracket implementation
- [ ] Add additional strategy implementations on the same interface
- [ ] Add experiment/report tooling on top of replay results

## Notes

- The current heatmap history path may already retain some derived liquidity history, but that is not the same as durable full book replay input. Confirm the exact persistence story before designing historical book datasets.
- Visual replay should be treated as a consumer of the replay session, not a second replay architecture.
