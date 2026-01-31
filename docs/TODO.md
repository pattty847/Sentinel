# Sentinel TODO

> Personal feature tracker. Append new features, check off tasks, keep moving.

## Format guide (for agents & future-me)

Each feature is a self-contained block. Use this template:

```
---
### F<N>: <Short name>
**Status:** active | paused | done
**Created:** YYYY-MM-DD
**Updated:** YYYY-MM-DD

#### Now (current sprint — do these first)
- [ ] Task description

#### Next (queued — pick up when Now is clear)
- [ ] Task description

#### Later (ideas / low-priority)
- [ ] Task description

#### Done
- [x] Task (YYYY-MM-DD)

#### Session log
- **YYYY-MM-DD** — What happened, what's next.
```

**Rules:**
- One feature per block. Don't merge unrelated work.
- Move tasks between Now/Next/Later freely. Keep Now short (3-5 items max).
- When you finish a task, move it to Done with the date. Don't delete it.
- Session log is append-only. Write a line every time you touch the feature.
- If a feature is fully done, set Status to `done` and collapse the block.
- Agents: do NOT reorder or renumber existing features. Append new ones at the bottom.

---

### F0: MSDF heatmap labels
**Status:** active
**Created:** 2026-01-30
**Updated:** 2026-01-31

#### Now
- [ ] Label geo cache: cache per-column quads; rebuild only when column/viewport/thresholds change
- [ ] Thresholds: store raw liquidity/intensity per cell so past columns can be re-evaluated

#### Next
_(empty)_

#### Later
- [ ] Wire MSDF atlas cache into label path (load from disk on startup, upload once)

#### Done
- [x] Swap GlyphAtlas -> MsdfAtlas and HeatmapGlyphNode -> MsdfGlyphNode in UnifiedGridRenderer (2026-01-30)
- [x] MSDF label UVs: use full cell UVs (cell + padding), keep linear filtering, single atlas size (2026-01-30)

#### Session log
- **2026-01-30** — MSDF Lab text looks correct; heatmap labels still bitmap. Next: wire MSDF into heatmap labels, then label-geometry cache.
- **2026-01-30** — Swapped heatmap labels to MSDF atlas/nodes and padded UVs. Next: add label-geometry cache + threshold reevaluation.
- **2026-01-31** — Fixed MSDF atlas projection units (range/translate), bumped cache version, added cache dir override.
- **2026-01-31** — Added MSDF debug stats logging gated by SENTINEL_MSDF_DEBUG_STATS.
- **2026-01-31** — Inverted MSDF shader sign to make outside transparent; rebuilt shaders.
- **2026-01-31** — Oriented contours, restored standard MSDF sign, and switched atlas generation to Y-up output.

---

### F1: Heatmap / Caching
**Status:** active
**Created:** 2026-01-30
**Updated:** 2026-01-30

#### Now
- [ ] Auto history request on symbol change (no manual subscribe needed)
- [ ] Scroll-past-cache fetch (request older history)

#### Next
- [ ] Derived timeframe rollups (non-anchor TFs from 1s/1m/1h/1d)
- [ ] Per-client TF stream (client asks for derived TF)
- [ ] StatusBar metrics wiring (FPS, GPU mem, upload bandwidth)

#### Later
- [ ] Heatmap settings panel v2 (extra controls + templates)
- [ ] Candlestick overlay Phase 1 (viewport-driven candles)

#### Done
_(nothing yet)_

#### Session log
_(no entries yet)_

---

### F2: UI shell & chart interactions
**Status:** active
**Created:** 2026-01-30
**Updated:** 2026-01-30
**Origin:** `docs/plans/UI_ROADMAP.md`

#### Now
- [ ] Toolbar polish: spacing, typography, label hierarchy, hover tooltips, compact dropdown styling
- [ ] Chart interactions: live axis updates during pan, CTRL+wheel zoom at cursor, drag-zoom rectangle

#### Next
- [ ] Watchlist enhancement: real data wiring, persistence, column config & sections
- [ ] SEC / Commentary polish: clean up integrations and UI consistency
- [ ] Theme switching: runtime theme selector that updates Qt stylesheet + QML bridge

#### Later
- [ ] Dock-specific micro-toolbars for non-chart widgets

#### Done
- [x] Charts dock owns its own toolbar — chart-specific controls live inside the dock (2026-01-30)
- [x] Global menu bar for app-level actions (View/Layouts/Tools/Debug) (2026-01-30)
- [x] Left control panel removed; chart controls consolidated into toolbar (2026-01-30)
- [x] Right watchlist dock with placeholder data (2026-01-30)
- [x] Dock title bars visible with stable drag/resize (2026-01-30)
- [x] Icons loaded via Qt resources under `:/icons/*` (2026-01-30)
- [x] Runtime font switching (system + resource fonts) (2026-01-30)
- [x] QML theme bridged to QWidget theme (shared palette) (2026-01-30)
- [x] Heatmap dock renamed to Charts with mode switching (2026-01-30)
- [x] MSDF scoped to heatmap/charts only; Qt Widgets/QML use standard fonts (2026-01-30)

#### Session log
- **2026-01-30** — Migrated from `docs/UI_ROADMAP.md`. Phase 1 (UI shell) complete. Next: toolbar polish + chart interactions.

---

### F3: SEC Filing Viewer v2
**Status:** active
**Created:** 2026-01-31
**Updated:** 2026-01-31

#### Now
_(empty)_

#### Next
- [ ] Filing detail view: render full filing text/HTML in dock (not just table rows)
- [ ] Insider transaction charting: plot insider buys/sells on a timeline
- [ ] Financial summary: key ratios, revenue/earnings at a glance

#### Later
- [ ] Filing diff view: compare two filings of same type side-by-side
- [ ] Filing search / full-text filter across cached filings
- [ ] Filing alerts: notify on new 8-K / insider tx for watchlist symbols

#### Done
- [x] Basic SecFilingDock UI with Filings / Insider Tx / Financial Summary tables (ported from DearPyGui) (2026-01-30)
- [x] Define server-side SEC API route (client requests filings/insider tx/financials via WebSocket or REST) (2026-01-31)
- [x] Wire server to call existing Python SEC backend (already has caching, rate limits, direct EDGAR access) (2026-01-31)
- [x] Client-side SEC data model: parse server responses into display-ready structs (2026-01-31)
- [x] Rip out Python subprocess call from SecFilingDock — replace with server API calls (2026-01-31)

#### Session log
- **2026-01-31** — Created. Python subprocess is broken (missing dotenv). Plan: route through server, server calls existing Python backend, client gets clean data. Ported UI exists but untested since DearPyGui migration.
- **2026-01-31** — Added server SEC endpoints, hardened Python entrypoints, and moved SEC dock to server-backed data model.

---

### F4: Copenet — AI market insights
**Status:** paused
**Created:** 2026-01-31
**Updated:** 2026-01-31

#### Now
- [ ] Define Copenet data contract: what the backend sends (insight type, symbol, timestamp, body, confidence, sources)
- [ ] Server-side Copenet endpoint: client subscribes per symbol, server streams insights
- [ ] Client-side CopenetFeed model: parse insights into displayable items

#### Next
- [ ] Wire CopenetFeedDock to display live insight stream (replace placeholder)
- [ ] Merge AI Commentary into Copenet (single feed, tagged by source/type)
- [ ] Insight types: microstructure reads (from heatmap), candle pattern calls, news sentiment, trade flow analysis
- [ ] Context injection: backend receives current heatmap snapshot / candle state as input for LLM

#### Later
- [ ] User-prompted queries: ask Copenet about current chart ("why is this level holding?")
- [ ] Insight pinning: pin an insight to a price level / time on the heatmap
- [ ] Confidence scoring + historical accuracy tracking
- [ ] Multi-symbol digest: cross-asset correlation insights

#### Done
- [x] CopenetFeedDock and AICommentaryFeedDock placeholder UI (2026-01-30)

#### Session log
- **2026-01-31** — Created. Copenet = AI insights on microstructure, candles, trades, news. AI Commentary merging into Copenet as single feed. Backend TBD but will stream via server WebSocket.

---

### F5: Candlestick overlay
**Status:** paused
**Created:** 2026-01-31
**Updated:** 2026-01-31
**Origin:** `docs/plans/CANDLES_HEATMAP_UNIFICATION.md`

#### Now
- [ ] Phase 1 MVP: add viewport properties to CandlestickBatched, refactor updatePaintNode for viewport coords
- [ ] QML overlay wiring: CandleChartView forwards viewport from UnifiedGridRenderer, overlayMode + z:2
- [ ] Demo data verification: candles align with heatmap grid, zoom/pan synced, no input conflict

#### Next
- [ ] Phase 2 real data: CandleDataProvider bridge (GUI thread, receives trades via queued slot)
- [ ] Wire IGridDataSource::tradeReceived → CandleDataProvider → CandlestickBatched
- [ ] Server OHLCV history: extend WebSocket protocol with ohlcv_bar / ohlcv_history messages
- [ ] Candle TF selector on toolbar (independent from heatmap TF)

#### Later
- [ ] Volume bars (third geometry node, bottom 15% of chart)
- [ ] Crosshair (shared QQuickItem at z:3 across heatmap + candles)
- [ ] Candle appearance polish: doji handling, min body height, opacity control

#### Done
- [x] CandlestickBatched QQuickItem with basic rendering (standalone) (2026-01-30)
- [x] Architecture decision: QML overlay approach, not integrated into UGR (2026-01-29)

#### Session log
- **2026-01-31** — Created from `docs/plans/CANDLES_HEATMAP_UNIFICATION.md`. Phase 1 (viewport-driven rendering) is next. CandlestickBatched exists but not wired to heatmap viewport yet.

---

### F6: Multi-symbol heatmap layouts
**Status:** paused
**Created:** 2026-01-31
**Updated:** 2026-01-31

#### Now
_(empty — blocked until F1 caching is more solid)_

#### Next
- [ ] Allow multiple ChartDock instances, each with independent symbol + viewport
- [ ] Layout presets: 2x1, 2x2, 1+3 (one large + three small)
- [ ] Per-dock symbol selector with independent server subscriptions
- [ ] Shared crosshair sync across docks (optional, toggle)

#### Later
- [ ] Correlation view: overlay two symbols' heatmaps with transparency
- [ ] Symbol-group templates: save/load multi-symbol layouts by name

#### Done
- [x] Custom dock layout system via LayoutManager (2026-01-30)

#### Session log
- **2026-01-31** — Created. Depends on F1 caching being stable enough for multiple concurrent streams. Layout system already exists.

---

### F7: Agent UX / Automation hooks (zoom, inspect, remote control)
**Status:** active
**Created:** 2026-01-31
**Updated:** 2026-01-31

#### Now
- [ ] Screenshot API: add `scale` (or `zoom`) param for supersampled captures (e.g. 1x/2x/3x) without changing live UI layout
- [ ] Screenshot API: add `crop` (x,y,w,h) to capture ROI (heatmap quadrant / toolbar / orderbook) for agent vision
- [ ] Add a "debug overlay" toggle endpoint to render viewport info (symbol, TF, viewport bounds, viewportVersion, FPS) into screenshots

#### Next
- [ ] Add a read-only "state dump" endpoint (JSON) for current symbol/TF/viewport so agents can reason without OCR
- [ ] Add a minimal input endpoint (mouse click / keypress) gated to localhost + explicit env flag (unsafe by default)
- [ ] Add a deterministic "scripted demo" mode: load symbol, set TF, set viewport, pause stream, take screenshots

#### Later
- [ ] Record short MP4/GIF from render thread (N frames) for animation/interaction debugging
- [ ] Expose per-dock capture targets (watchlist, SEC, charts) via stable IDs (avoid brittle coordinates)

#### Done
_(nothing yet)_

#### Session log
- **2026-01-31** — Added feature block. Goal: make Sentinel inspectable by agents via high-res/cropped screenshots + optional safe automation hooks.
