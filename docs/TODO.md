# Sentinel TODO

> Personal feature tracker. Append new features, check off tasks, keep moving.

### Format guide (for agents & future-me)

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
**Updated:** 2026-01-30

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

### F2: Heatmap Settings Expansion

**Status:** active
**Created:** 2026-02-02
**Updated:** 2026-02-02

#### Now (current sprint — do these first)

* [ ] Add Heatmap Visual Settings section (opacity, contrast, gamma)
* [ ] Add toggle for heatmap blending mode (additive / max / overwrite)
* [ ] Add per-side color controls (bid/ask hue + saturation)
* [ ] Add heatmap value normalization modes (global, visible range, session)
* [ ] Add toggle for heatmap magnifier (already stubbed in UI)

#### Next (queued — pick up when Now is clear)

* [ ] Add heatmap smoothing kernel (none / box / gaussian)
* [ ] Add decay rate control for historical liquidity
* [ ] Add heatmap persistence slider (time-weighted fading)

#### Later (ideas / low-priority)

* [ ] Preset profiles (Scalp / Swing / HTF / Meme)
* [ ] Hotkey cycling between heatmap modes

#### Done

* [x] Core heatmap rendering stabilized (2026-02-02)

#### Session log

* **2026-02-02** — Axis refactor unlocked stable panning; heatmap is now performant enough to justify rich settings.

---

### F3: Candlestick ↔ Heatmap Unification

**Status:** active
**Created:** 2026-02-02
**Updated:** 2026-02-03

#### Now (current sprint — do these first)

* [ ] Rename HeatmapDock → ChartDock (class + files) to reflect multi-chart purpose
* [ ] Define shared coordinate system for candles + heatmap
* [ ] Overlay candlesticks directly on heatmap canvas
* [ ] Ensure Z-ordering works with opacity + blending
* [ ] Add toggle: "Candles Over Heatmap"

#### Next (queued — pick up when Now is clear)

* [ ] Wire timeframe combo to actual timeframe switching
* [ ] Wire chart type combo (Candle/Hollow/Line) to rendering
* [ ] Add candle body opacity control when heatmap visible
* [ ] Add wick-only mode for minimal obstruction
* [ ] Sync candle aggregation with heatmap resolution

#### Later (ideas / low-priority)

* [ ] Candle heatmap fusion mode (color candles by delta / absorption)
* [ ] Auto-hide candles at extreme zoom levels

#### Done

* [x] Dedicated plan created (`docs/plans/CANDLES_HEATMAP_UNIFICATION.md`) (2026-02-02)

#### Session log

* **2026-02-02** — Performance headroom finally makes unified rendering viable without hacks.
* **2026-02-02** — Added HeatmapDock → ChartDock rename task; dock already titled "Charts" but class name needs update.
* **2026-02-03** - Defaulted chart mode to hybrid candles+heatmap and auto-request candle history on reconnect.
* **2026-02-03** - Gate auto-subscribe on user action; size candle history request to visible window.
* **2026-02-03** - Wire toolbar timeframe to renderer/candles and add 1s option.
* **2026-02-03** - Add SENTINEL_CHART_DEBUG logging for timeframe + candle overlay diagnostics.
* **2026-02-03** - Apply panVisualOffset in viewport transform to prevent candle snap.

---

### F4: Footprint Chart Core

**Status:** active
**Created:** 2026-02-02
**Updated:** 2026-02-08

#### Now (current sprint — do these first)

* [ ] Finalize Bid/Ask vs Delta display modes
* [ ] Implement cluster vs profile visual styles
* [ ] Implement width scaling modes (rotation-aware)
* [ ] Render per-cell numbers efficiently (GPU batched)

#### Next (queued — pick up when Now is clear)

* [ ] Add imbalance detection logic
* [ ] Add imbalance highlighting thresholds
* [ ] Add candle-level stats (V, Δ, max imbalance)

#### Later (ideas / low-priority)

* [ ] Footprint replay mode (tick-by-tick build)
* [ ] Footprint compression at low zoom

#### Done

* [x] Basic footprint rendering functional (2026-02-02)

#### Session log

* **2026-02-02** — Footprint UI exists; logic depth now the bottleneck, not rendering.
* **2026-02-08** - Switched server footprint_slice from synthetic heatmap-derived values to trade-side delta ladder per bucket with q16 quant+quant_scale; next verify visual semantics vs target clustered footprint.

---

### F5: Orderbook Profile Overlay

**Status:** active
**Created:** 2026-02-02
**Updated:** 2026-02-02

#### Now (current sprint — do these first)

* [ ] Render aggregated orderbook profile alongside heatmap
* [ ] Add profile normalization modes
* [ ] Add toggle for profile labels

#### Next (queued — pick up when Now is clear)

* [ ] Session-scoped profiles
* [ ] Compare two profiles (before/after move)

#### Later (ideas / low-priority)

* [ ] Profile divergence alerts
* [ ] AI-annotated profile commentary

#### Done

* [x] Profile rendering stub exists (2026-02-02)

#### Session log

* **2026-02-02** — Profile view pairs naturally with heatmap; must stay lightweight.

---

### F6: Market & Liquidation Bubbles

**Status:** active
**Created:** 2026-02-02
**Updated:** 2026-02-02

#### Now (current sprint — do these first)

* [ ] Add trade bubble rendering (size = volume)
* [ ] Add liquidation bubble rendering (distinct style)
* [ ] Add toggles for both

#### Next (queued — pick up when Now is clear)

* [ ] Fade bubbles over time
* [ ] Bubble clustering at low zoom

#### Later (ideas / low-priority)

* [ ] Bubble-to-heatmap interaction (impact highlight)
* [ ] Event replay mode

#### Done

* [x] UI toggles wired (2026-02-02)

#### Session log

* **2026-02-02** — Visual noise risk acknowledged; must remain optional.

---

### F7: MSDF Text Rendering System

**Status:** active
**Created:** 2026-02-02
**Updated:** 2026-02-02

#### Now (current sprint — do these first)

* [ ] Implement MSDF font atlas generation
* [ ] Replace QML Text with GPU MSDF text
* [ ] Integrate zoom-stable label rendering

#### Next (queued — pick up when Now is clear)

* [ ] Add text LOD system (numbers → hints → none)
* [ ] Color-coded numeric overlays

#### Later (ideas / low-priority)

* [ ] Animated text emphasis (delta spikes)
* [ ] Text outlines for dense heatmaps

#### Done

* [x] Plan created (`docs/plans/MSDF_TEXT_LAB.md`) (2026-02-02)

#### Session log

* **2026-02-02** — Axis refactor exposed text as next performance frontier.

---

### F8: TPO / Market Profile

**Status:** active
**Created:** 2026-02-02
**Updated:** 2026-02-02

#### Now (current sprint — do these first)

* [ ] Implement TPO letter assignment per time slice
* [ ] Render TPO columns aligned with price axis
* [ ] Add POC, VAH, VAL calculation

#### Next (queued — pick up when Now is clear)

* [ ] Session segmentation (RTH / ETH / custom)
* [ ] TPO vs Volume Profile toggle

#### Later (ideas / low-priority)

* [ ] Composite profiles
* [ ] AI explanation of profile shape

#### Done

* [x] Plan exists (`docs/plans/TPO.md`) (2026-02-02)

#### Session log

* **2026-02-02** — Core market structure feature; cannot be half-assed.

---

### F9: Chart Interaction & UX Polish

**Status:** active
**Created:** 2026-02-02
**Updated:** 2026-02-02

#### Now (current sprint — do these first)

* [ ] Right-click context menu consistency
* [ ] Unified settings panel behavior
* [ ] Keyboard shortcuts for major toggles

#### Next (queued — pick up when Now is clear)

* [ ] Quick presets menu
* [ ] Per-chart state persistence

#### Later (ideas / low-priority)

* [ ] Command palette
* [ ] “Explain what I’m seeing” AI button

#### Done

* [x] Alert UX documented (2026-02-02)

#### Session log

* **2026-02-02** — UX debt acceptable short-term, dangerous long-term.

---

### F10: Coinbase REST Candle History
**Status:** active
**Created:** 2026-02-02
**Updated:** 2026-02-08

#### Now
- [x] Serve 1s candles from live trade aggregation (no REST history)
- [ ] Roll up 1m candles to custom timeframes (on-demand)
- [ ] Add server-side candle cache per (symbol, timeframe)
- [x] Stream live candle updates over WS (barUpdate/barClosed)
- [x] Add server-side gating for candle updates (threshold + silence)

#### Next
- [ ] Merge REST history with live candles (overwrite forming bar when closed)
- [ ] Add gap rendering hints for no-trade buckets
- [ ] Candle stream verifier script (auto-start server + swap config; manual fallback)

#### Later
- [ ] Persist candle cache to disk for fast restart

#### Done
- [x] Coinbase REST candles fetcher + REST JWT (2026-02-02)
- [x] WS candle_history_request / candle_history_chunk (server + client) (2026-02-02)
- [x] 1s candles from trade aggregation history (2026-02-02)
- [x] Live candle WS updates (barUpdate/barClosed) (2026-02-02)
- [x] Server-side candle update gating (2026-02-02)

#### Session log
- **2026-02-02** — Added Coinbase REST candle history endpoint and WS request/response. Next: 1s candles + rollups + caching.
- **2026-02-02** — Implemented 1s candle history from trade aggregation for WS requests.
- **2026-02-02** — Added live candle WS updates with seq per (symbol, tf); gating next.
- **2026-02-02** — Added basic gating on candle updates (high/low, close move, silence).
- **2026-02-02** — Deferred robust candle test harness; add script later with auto-run + manual fallback.
- **2026-02-08** — Explored toolbar timeframe/candle path; found protocol schema gating mismatch and mapping-timeframe coupling blocking reliable TF switching.

---

### F11: Heatmap Color System Overhaul
**Status:** active
**Created:** 2026-02-02
**Updated:** 2026-02-02

#### Now
- [ ] Fix hardcoded palette in `ensureHeatmapPaletteImage()` - make it dynamic
- [ ] Implement multi-stop gradient system (3-5 color stops per side)
- [ ] Create vibrant "Sentinel" preset (dark→bright with full saturation)
  - Bids: (0,30,30) → (0,120,100) → (0,255,200) electric cyan
  - Asks: (40,0,0) → (180,40,20) → (255,100,30) → (255,200,50) hot orange
- [ ] Fix palette gamma (change 0.65 → 2.0+ for dramatic contrast)
- [ ] Lower shader floor to 0.0-0.01 (allow true blacks for low liquidity)
- [ ] Add palette regeneration trigger when color settings change

#### Next
- [ ] Add preset system: Classic, Fire, Ocean, Monochrome, Matrix
- [ ] Replace number spinboxes with QSliders in HeatmapSettingsDialog
- [ ] Add color picker widgets for custom bid/ask colors
- [ ] Add "Intensity Curve" slider (replaces hardcoded 0.65 gamma)
- [ ] Add live preview in settings dialog

#### Later
- [ ] Per-exchange color profiles (Coinbase vs Binance palettes)
- [ ] Time-of-day adaptive colors (bright during trading hours, dark at night)
- [ ] Heatmap color based on velocity/absorption (not just liquidity)
- [ ] "Hot spot" mode - highlight extreme liquidity with pulsing effect

#### Done
- [x] Identified hardcoded palette as root cause of bland colors (2026-02-02)
- [x] Analyzed reference heatmap (Binance-style) for color strategy (2026-02-02)

#### Session log
- **2026-02-02** — Discovered hardcoded RGB values and backwards gamma (0.65) in palette generation. Settings dialog was wired correctly but palette never regenerated! Solution: dynamic multi-stop gradients with high gamma (2.0+) and near-zero floor for dramatic dark→bright progression.

---

### F12: Chart Toolbar Implementation

**Status:** active
**Created:** 2026-02-02
**Updated:** 2026-02-08

#### Now (current sprint — do these first)

**Symbol Controls (DONE):**
- [x] Symbol search input (QLineEdit) - working
- [x] Subscribe button - working

**Chart Mode Switcher (DONE):**
- [x] Heatmap mode button (icon-heatmap.svg) - working
- [x] Candles mode button (icon-candles.svg) - working
- [x] Hybrid mode button (icon-hybrid.svg) - working

**Timeframe & Type (PARTIAL):**
- [ ] Wire timeframe combo to actual data refresh (1m, 5m, 15m, 1h, 4h, 1D)
- [ ] Wire chart type combo (Candle, Hollow, Line) to rendering logic

**Liquidity Controls (PARTIAL):**
- [ ] Wire liquidity mode combo (Asset/USD) to label formatting
- [ ] Wire liquidity threshold slider to heatmap filter

#### Next (queued — pick up when Now is clear)

**Action Buttons (MISSING):**
- [ ] Implement Indicators panel (icon-indicators.svg)
- [ ] Implement Layouts menu (icon-layout.svg) - save/load/default layouts
- [ ] Implement Quick Search (icon-search.svg) - fuzzy symbol search
- [ ] Implement Settings panel (icon-settings.svg) - heatmap/candle/general settings
- [ ] Implement Fullscreen toggle (icon-fullscreen.svg) - F11 keybind
- [ ] Wire Screenshot button (icon-camera.svg) to GuiApiServer

#### Later (ideas / low-priority)

**Toolbar Enhancements:**
- [ ] Add Drawing Tools submenu (trend lines, fib, rectangles)
- [ ] Add Alerts submenu (price alerts, indicator alerts)
- [ ] Add Replay mode controls (time travel / tick replay)
- [ ] Add Watchlist quick-add button
- [ ] Toolbar customization (show/hide controls, reorder)
- [ ] Keyboard shortcuts for all toolbar actions

**Visual Polish:**
- [ ] Hover tooltips with keyboard shortcuts
- [ ] Active state highlighting for toggled buttons
- [ ] Toolbar themes (compact / spacious / minimal)
- [ ] Icon animation on mode switches

#### Done

- [x] Toolbar UI structure created (TopToolbar.cpp) (2026-01-XX)
- [x] Chart mode switching functional (2026-01-XX)
- [x] Symbol subscription functional (2026-01-XX)

#### Session log

- **2026-02-02** — Inventoried all toolbar controls; identified wiring gaps for timeframe/liquidity/actions.
- **2026-02-02** — Toolbar is visually complete but many buttons emit signals without backend implementation.
- **2026-02-08** — Verified toolbar signals are wired in MainWindowGPU; timeframe emits history requests, but end-to-end behavior is blocked by protocol/schema and renderer timeframe coupling issues.

---

### F13: Comment Cleanup (libs/ — senior-dev standard)
**Status:** complete
**Created:** 2026-02-03
**Updated:** 2026-02-03

#### Now
- [x] Continue cleanup pass on remaining files with egregious comments

#### Next
- [x] **Audit:** Generate list of files with `//` comments (e.g. `rg "//" libs/ --files-with-matches` or search in IDE). ~102 files, ~1016 comments.
- [x] **Triage per file:** Open each file; for each comment decide: **gut** (remove), **concise** (one line “why”), or **deeper** (keep/expand only if non-obvious invariant or protocol).
- [x] **Gut:** Remove COT, “what” comments, Python-style `#`, filler, TODOs with no ticket, and file-header essays.
- [x] **Concise:** Replace verbose blocks with a single “why” line where intent is subtle.
- [x] **Deeper:** Add or keep “why” only for protocol/threading/magic numbers/invariants; ensure C++ style (`//` or `/* */`), no stray `#`.
- [x] **Check:** Build + quick sanity run after each batch of files.

#### Later
- [x] Apply same policy to `apps/` and `libs/gui` if desired.
- [x] Add or point to commenting rules in AGENTS.md (done: §3).

#### Done
- [x] Commenting guidelines added to AGENTS.md §3 (2026-02-03)
- [x] Outline for comment cleanup added to TODO (2026-02-03)
- [x] Cleaned up 10 egregious files: SentinelLogging.cpp, AxisModel.hpp, Authenticator.hpp, LayoutManager.cpp, HeatmapDock.cpp, TimeframeAggregator.hpp, CandlestickBatched.cpp, MenuBuilder.cpp, ShortcutBinder.cpp, UnifiedGridRenderer.cpp (2026-02-03)

#### Session log
- **2026-02-03** — Outlined efficient pass: audit → triage per file (gut / concise / deeper) → batch by dir, verify build. Deferred to dedicated session.
- **2026-02-03** — Re-added a handful of “why” comments for platform quirks, latency sanity, and zoom/pan thresholds.
- **2026-02-03** — Cleaned up 10 worst offenders: removed verbose section headers, Doxygen blocks, redundant file headers, and “what” comments. Kept minimal “why” comments for threading/invariants only. Files now follow AGENTS.md §3 standards.

---

### F14: Agent Notes System (_agent)
**Status:** done
**Created:** 2026-02-06
**Updated:** 2026-02-06

#### Now
_(empty)_

#### Next
_(empty)_

#### Later
_(empty)_

#### Done
- [x] Add `_agent/` folder with standardized notes files (2026-02-06)
- [x] Seed invariants, failure modes, repo map, and decisions (2026-02-06)
- [x] Document agent notes format in AGENTS.md (2026-02-06)
- [x] Gitignore `_agent/` (2026-02-06)

#### Session log
- **2026-02-06** — Created `_agent` notes system and seeded initial content; documented format and rules in AGENTS.md.

---

### F15: GUI Architecture Refactor (RendererHost + Overlays)
**Status:** active
**Created:** 2026-02-09
**Updated:** 2026-02-09

#### Now (current sprint � do these first)
- [x] Phase 1: MappingProvider seam for candles (remove direct CandlestickOverlayItem -> UnifiedGridRenderer mapping dependency)
- [x] Phase 1: Remove QML write path for renderer `primaryField`; route mode authority from ChartModeController
- [x] Phase 1: Introduce FrameContext construction in updatePaintNode with immutable snapshots and generation counters
- [ ] Phase 2: Start overlay extraction with candle path first

#### Next (queued � pick up when Now is clear)
- [ ] Extract Footprint overlay module behind host orchestration
- [ ] Extract Heatmap overlay module behind host orchestration
- [ ] Introduce explicit TimeAuthority and decouple cadence from heatmap ownership

#### Later (ideas / low-priority)
- [ ] Full folder reorg after boundaries stabilize
- [ ] Add diagnostics overlay and per-phase perf gates dashboard

#### Done
- [x] Week 0 stabilization fixes completed (2026-02-09)

#### Session log
- **2026-02-09** � Started Phase 1 implementation: added `ITimeAxisMappingProvider`, switched candles to provider contract, and moved renderer primary field control to ChartModeController-driven flow. Next: FrameContext + generation counters.
- **2026-02-09** - Completed Phase 1 FrameContext snapshot plumbing: provider now exposes `MappingFrameContext`; candlestick overlay consumes immutable frame snapshots and generation counters.

