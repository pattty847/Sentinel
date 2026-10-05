# Widget pass: execution plan (orchestrator, 2026-10-03)

Directive: `docs/research/2026-10-widget-pass-directive.md` (owner decisions win). Audit:
`docs/research/2026-10-widget-audit.md`. Loop: `docs/AGENT_WORKFLOW.md`.

## Constraints that shape the plan

- **Shared hubs.** Every dock add/remove/wire goes through `MainWindowGpu.{h,cpp}`,
  `mainwindow/DockFactory`, `mainwindow/LayoutOrchestrator`, `mainwindow/MenuBuilder`. Each wave gives
  exactly one branch ownership of these. Other branches stay inside their widget files; if a one-line hook in a
  hub is unavoidable they declare it in their report and the orchestrator resolves it at land time.
- **Budget (2026-10-03 22:30).** Claude weekly 7% (resets 2026-10-06 00:00 EDT), Codex weekly 25% plus reset
  credits held by the owner. The orchestrator spends roughly 1% of the Claude week per branch (dispatch, review
  routing, land). Historical gate: wave 2 waited on the Claude orchestrator budget. Owner correction 2026-10-04:
  Codex now orchestrates; wave 2 may start after the per-product tick slice and an owner-applied Codex reset
  credit. At most three writing lieutenants at once. Codex writes; Claude Fable
  reviews subject to before/after weekly-budget checks; a separate Fable allowance is not yet proven; no Claude opus/sonnet subagents until the owner confirms the Claude weekly reset.
- **Visual proof.** Sandboxed Codex uses the GUI host for visual proof. W1c has landed: the host accepts
  `launch --build <worktree>` and safe retained-dock / `window` grabs, so a lieutenant can validate its
  unlanded branch after a queued build. One hosted GUI session at a time. Owner visual approval remains
  required before unseen visual changes become the default. No branch claims visual correctness from compilation.
- Builds/tests through `scripts/dev/build-queue.sh`. No renderer rewrites. Ingestion untouched by GUI throttling.

## Wave 1 (dispatched 2026-10-03)

| Id | Branch | Model | Owns | Must not touch |
|---|---|---|---|---|
| W1a | `lt-astra/dom` | gpt-6-astra | `widgets/OrderBookDock.*`, new DOM model files under `libs/gui/models/`, its tests | hubs (show/hide via the dock's own events), RemoteGridDataSource, renderer |
| W1b | `lt-sol/paper-ticket` | gpt-6-sol | `widgets/PaperTradingDock.*`, paper models/log, algo-ack handling in `RemoteGridDataSource` (algo/trade-command paths only) | hubs, OrderBookDock, server protocol semantics (document gaps instead) |
| W1c | `lt-sol/dock-infra` | gpt-6-sol | hubs; dock grab targets (`MainWindowGpu` grab registry, `scripts/dev/gui-host.py` allowlist, AgentHostMode, `docs/AGENT_API.md`); delete TradeBlotterDock; remove production LabDock; saved-layout migration | OrderBookDock, PaperTradingDock internals, sentinel-lab executable, renderers/tests |
| W1d | read-only | gpt-6-sol | CopeNet + commentary-family dependency map and the CopeNet microstructure-context boundary design (doc only) | all code |

Land order: W1c first if ready (it gives every later branch visual validation), then W1a, W1b; each lands after
a Fable review and a rebase+retest (merge-queue rule).

## Wave 2 (after tick slice and Codex reset credit; owner correction 2026-10-04)

| Id | Scope | Owns |
|---|---|---|
| W2a | P4 shared health model + P6 StatusBar + P5 telemetry; DOM/paper adopt it | new GUI `MarketHealth` model, `RemoteGridDataSource` status signals, `StatusBar`, `HeatmapTelemetryDock` |
| W2b | P3 toolbar wiring + P9 chart-dock shell + LiquidityRangeSlider | hubs (this wave's owner), `TopToolbar`, `LiquidityRangeSlider`, chart dock shell (not the renderer) |
| W2c | P7 watch rail + P8 screener | `WatchlistDock`, `ScreenerDock` |
| W2d | Stock chart + SEC correctness; AICommentary was already removed by W1c | `StockChartDock`, `StockChartView.qml`, `SecFilingDock`, `SecApiClient`; preserve CopeNet and its commentary base |

W2b remains the sole hub-file owner. Other branches declare any required hub hooks instead of editing the hubs.
Dispatch at most three writers; start the fourth only after a slot is free. Fable reviews each branch while the
budget gate permits; stop Claude reviews and notify the owner before landing if Claude weekly remaining drops.

Owner interim override (2026-10-04 after deployment): Claude weekly remaining fell from 3% to 2% across Fable review, so Claude calls stay stopped; a separate allowance remains unverified. Until Claude returns, use a different GPT model for each review (Sol reviews Astra work, Astra reviews Sol work). Codex reset credit is now visible. W2a/W2b/W2c are writing; visual owner approval and all build/landing gates still apply.

## Wave 3

Theme/font/settings normalisation across retained widgets (extend DarkTheme/ThemeBridge/Theme.qml/FontManager),
then a full visual QA pass through the grab targets (Fable visual A/B) and the owner.

## Status

Tracked in `docs/STATUS.md` (In flight table).
