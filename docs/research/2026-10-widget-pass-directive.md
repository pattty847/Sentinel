# Widget pass: owner directive (2026-10-03)

Owner's decisions on `docs/research/2026-10-widget-audit.md`. Most technical findings are accepted; the overrides
below win over the audit. Execution plan and agent ownership: `docs/research/2026-10-widget-pass-plan.md`.

## Product direction

Sentinel is primarily a high-performance crypto market-microstructure terminal: GPU liquidity heatmap,
candles, order book / DOM, trade and microstructure information, paper execution, recorded-market navigation,
market/feed health, and eventually AI/CopeNet interpretation of that data. It is also a modular workstation: a
dock that is cheap while hidden and useful when opened may stay. Do not delete working product surfaces because
they are secondary to the core workflow; they need not dominate the default crypto workspace.

- **StockChartDock stays** (yfinance historical stock charts, working).
- **SEC/research stays** unless there is a concrete technical reason otherwise.
- **CopeNet stays.** Do not delete CopenetFeedDock or its integration. It is the AI half of the product (the
  standalone CopeNet project exists in JS/Python). A missing message producer is not a reason to delete it.
  Preserve UI/integration points, improve foundations where sensible, no CopeNet architecture rewrite.

## P1 Order book / DOM (highest priority)

A stable, legible, trustworthy liquidity ladder; a coherent DOM model, not a restyle.
Problems: ask/mid duplication in some bucket conditions; ambiguous BUYS/SELLS/DELTA; execution counts look like
volume; aggregation not disclosed; no base/quote units; no freshness/stale state; rows move with populated levels
instead of a stable ladder; QTableWidget items reallocated per update; work continues while hidden; front-erase of
the recent-trade vector; unknown/non-buy aggressors fall into sell; constant recentering blocks inspection.
Desired: retained rows/model; rows keyed by stable integer tick/bucket; exactly one row per bucket; no duplicated
best ask/mid row; explicit aggregation tick; explicit base/quote units; right-aligned numbers; accurate execution
wording (e.g. `last 1,000 trades`), never counts presented as volume; explicit unknown-aggressor policy; full
event ingestion with GUI paint coalesced to ~10-20 Hz; no GUI work while hidden; stable manual scrolling;
explicit recenter/follow; clear feed freshness. Defaults: compact ~20-24 px rows; cyan/amber (or existing
liquidity colours) for resting liquidity; green/red for executed buy/sell; side never by colour alone; a scrolled
DOM never yanks away; one click recenter/follow restores auto. Keep data/render boundaries; no GUI throttling in
ingestion. Tests: bid and ask in the same bucket; best-ask uniqueness; rapid book changes; sustained trade flow;
unknown aggressor; scrolling during updates; recenter; hide/show; stale/disconnected; counts stay counts.

## P2 Paper trading / execution ticket

Critical bug: a typed limit price is overwritten by new trades after the field loses focus. Once edited, the
limit is user-owned; live trades never replace it; optional explicit `Use Last` / `Use Market`; quantity changes,
buttons and focus moves never destroy it. Active symbol and PAPER mode obvious; base quantity / quote notional
semantics; instrument-aware precision incl. low-price crypto; order/log cells read-only; the log follows only
while the user is at its end; mutually exclusive PnL ranges behave exclusively; symbol transitions never imply
confirmed account/position state. Algo lifecycle stops lying: states from actual knowledge (idle, command
pending, acknowledged/running, rejected, stopping, stopped, unavailable). If the protocol cannot provide a state,
surface the limitation. Do not redesign or duplicate the execution model.

## P3 Top toolbar

Do not delete intended controls because they are unwired. Wire chart type, indicators, layouts, quick search,
fullscreen: trace the intended architecture; if a feature truly does not exist, build the smallest coherent
version or show an explicit disabled state with a reason. No clickable no-ops. Also: distinct TPO vs Volume
Profile affordances; label Tick; liquidity-range units clear (base-asset values where applicable); settings
reachable at narrow widths; fewer mystery glyphs; secondary appearance/debug settings into the gear path; reliable
overflow or no primary action depending on it. Keep readily accessible: symbol, timeframe, primary chart/field
mode, major overlays/layers, tick, settings. Test narrow and wide.

## P4 Shared market health / freshness model

One truthful vocabulary: Live, Initializing, Waiting for subscription, Waiting for book, Waiting for
heatmap/history, Reconnecting, Stale, History partial, Unavailable, Disconnected. Built from real signals:
transport, subscription ack, book receive time, heatmap/live receive time, history coverage/loading, renderer
holding/loading reason. Unknown is not zero; unavailable is not healthy; a quiet market is not necessarily stale.
Compact form in the StatusBar and trading panels; detail in HeatmapTelemetryDock.

## P5 Heatmap telemetry

Keep (good exposed-only polling). Trader/data health first (connection/subscription, symbol, book age,
heatmap/live age, history coverage, loading/holding reason); engineering counters beneath/collapsed. Never
relabel `data age at draw` as feed age. Label graph axes/scales. Missing QVariant values stay unknown, not 0.

## P6 Status bar

Concise: active symbol, market/feed state, freshness, major degraded/disconnected state. Noise to telemetry.
Remove the empty one-second timer if it does nothing. CPU/GPU values that cannot be measured correctly on the
platform say unavailable or are omitted.

## P7 Watchlist -> recorded market rail

Compact persistent launcher: recorded/supported crypto products prominent; active-chart marker; persistent
pinned/custom list if the settings infrastructure makes it easy; keyboard and mouse activation; pending switch
state; refused/unavailable state; never claim active before the switch is acknowledged; never advertise
unsupported products as recordable/live. Stock entries may route to StockChart. No full book subscription per
row; live price/change/as-of waits for a lightweight shared summary feed.

## P8 Screener

Keep. Preserve rows by symbol across refresh; no clear/rebuild/sort jumping; keep selection; missing JSON numbers
show an em dash, never 0; show source/as-of; show service errors/timeouts; switching asset class never leaves
stale results; keyboard activation; compact crypto preset. No new microstructure-screener backend.

## P9 Chart dock / DepthChartView

Protect the core renderer (the GPU heatmap/candles are the reference); no speculative renderer rewrites.
Shell only: chart/symbol identity, default width allocation, narrow toolbar behaviour, accessible axis controls,
distinguish the small auto-price control from keyboard follow-live, tooltips/keyboard, developer overlays into
telemetry where practical, shared theme roles where it does not cost renderer performance. Keep
viewport/render-thread invariants.

## Stock chart (keep)

Fix: a ticker change never shows old candles under the new label; retained data stays tied to its symbol;
loading/error visible even with old candles; request identity / stale-response protection; asynchronous
cancellation, never blocking the GUI thread; provider/as-of shown; shared theme/font roles. Keep yfinance.

## SEC filing dock (keep)

Fix: never convert BTC-USD -> BTC and pretend it is an equity; old results never survive under a new ticker
unlabelled; results bound to request/symbol identity; loading/error state; read-only numeric tables; source and
timestamp; no GUI-blocking cancellation. Do not force crypto symbols into SEC workflows.

## CopeNet (keep, strategic)

Preserve CopenetFeedDock and its contracts. Inspect how CommentaryFeedDock/CopenetFeedDock relate before deleting
any shared base. Remove only genuinely dead siblings that cannot affect CopeNet. Do not collapse CopeNet into
generic AI commentary; keep room for symbol-linked AI output. Document (not necessarily implement) the cleanest
path for CopeNet/models to consume Sentinel data without coupling to GUI internals: a structured read-only
microstructure API/snapshot/event boundary (active instrument, top of book, spread, depth/liquidity around price,
heatmap/liquidity summaries, trades/aggressor flow, imbalance/pressure, recent structural changes, rolling
derived metrics). Never dump the raw high-rate stream into an LLM; a summarisation/query boundary is needed.

## AI commentary / commentary base

AICommentaryFeedDock may be removed if truly unused with no strategic purpose. CopenetFeedDock remains; any base
it needs remains or is refactored safely. Do not delete the whole commentary family.

## Trade blotter

Delete TradeBlotterDock if confirmed superseded by PaperTradingDock, uninstantiated, outside any workflow: source,
header, CMake entries, dead MainWindow member/declaration, stale registrations. Keep PaperTrading's order model,
logs, overlays and execution.

## Lab dock

Remove LabDock from the production workspace (factory/menu, default layout, obsolete API target, MainWindow
references, LabView production resource if unused). Keep the sentinel-lab executable, shared renderers, renderer
tests, and CandlestickBatched if used elsewhere.

## Settings / theme / font

No new theme system: extend DarkTheme, ThemeBridge, Theme.qml, FontManager; normalise retained widgets
gradually. Charcoal surfaces, restrained borders, readable secondary text, tabular numerals, cyan/amber for
resting liquidity, green/red for executed buy/sell and PnL, never colour alone. Font settings must actually reach
widgets (no hard-coded Menlo/Roboto Mono/pixel sizes defeating them). Settings dialog: keep gradient/palette,
compact common controls, advanced/engineering config separated, explicit units, hide unsupported settings.
LiquidityRangeSlider: keyboard/focus/accessibility, visible base-unit meaning, numeric editing if appropriate,
keep the logarithmic/frozen-domain drag.

## Dock infrastructure / validation

Keep DockablePanel, LayoutManager, ServiceLocator. Removing docks needs safe saved-layout migration. Add safe
grab support for retained widgets (host allowlist, AgentHostMode/API allowlist, QWidget grab, QQuickView handling)
so agents can validate each one; never unrestricted main-window capture. Then inspect retained surfaces in
meaningful states.

## Integration order, performance, validation, done

Order: DOM, paper ticket, toolbar, shared health, telemetry/status, watch rail, screener, stock/SEC, CopeNet
prep, cleanup/theme/accessibility/validation. Correctness never waits on cosmetics.
Performance: ingestion separate from paint rate; no per-event widget/table allocation; no needless cross-thread
copies; no GUI-thread blocking waits; no expensive polling while hidden; GPU heatmap architecture preserved; GUI
update cost measured separately from renderer timing; no FPS conclusions from idle render counts.
Validation (where relevant): live BTC-USD, high-rate updates, same-bucket bid/ask, narrow/wide docks, long
symbols, low-price instruments, hide/show, keyboard, disconnect/reconnect, stale, initial loading, failed
requests, symbol switching, scrolled DOM, scrolled paper log, typed limit then market updates, pending/rejected
commands, stock ticker switch with old data, SEC switching, toolbar actions. Compilation is not visual proof.
Before destructive cleanup verify references. Escalate only genuine product decisions.
