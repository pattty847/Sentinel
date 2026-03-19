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
**Status:** mostly done
**Created:** 2026-01-30
**Updated:** 2026-03-16

#### Now
- [ ] Label geo cache: cache per-column quads; rebuild only when column/viewport/thresholds change
- [x] Thresholds: store raw liquidity/intensity per cell so past columns can be re-evaluated (2026-03-19)

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
**Updated:** 2026-03-17

#### Now
- [ ] Auto history request on symbol change (no manual subscribe needed)
- [ ] Scroll-past-cache fetch (request older history)
- [ ] Preserve historical heatmap columns across band recenter instead of clearing visual history
- [ ] Persist derived heatmap history to disk and reload on startup with explicit gap handling for dev/offline periods

#### Next
- [ ] Derived timeframe rollups (non-anchor TFs from 1s/1m/1h/1d)
- [ ] Per-client TF stream (client asks for derived TF)
- [ ] StatusBar metrics wiring (FPS, GPU mem, upload bandwidth)
- [ ] Page persisted heatmap history into the bounded GPU ring so the canvas feels infinite without needing an infinite texture
- [ ] Define a sliding-window world-history architecture for heatmap/TPO/footprint so viewport panning pages through larger persisted datasets instead of treating fixed grid width/height as total history/range

#### Later
- [ ] Heatmap settings panel v2 (extra controls + templates)
- [ ] Candlestick overlay Phase 1 (viewport-driven candles)

#### Done
_(nothing yet)_

#### Session log
- **2026-03-15** - Persistence audit found durable raw trade logs on disk, but heatmap history remains in-memory only and is cleared on band recenter/reset. Future direction: keep the live one-quad GPU path, but persist world-time history outside the renderer and page visible columns into the bounded ring.
- **2026-03-17** - Captured the next architectural issue explicitly: chart layers need a larger persisted world-history model with a bounded GPU window, so long-range heatmap/TPO browsing can feel infinite without fixed-grid hard stops.

---

### F2: Heatmap Settings Expansion
**Status:** paused
**Created:** 2026-02-02
**Updated:** 2026-03-16

#### Now
- [ ] Add toggle for heatmap blending mode (additive / max / overwrite)
- [ ] Add per-side color controls (bid/ask hue + saturation)
- [ ] Add heatmap value normalization modes (global, visible range, session)
- [ ] Add toggle for heatmap magnifier (already stubbed in UI)

#### Done
- [x] Add Heatmap Visual Settings section (opacity, contrast, gamma) (2026-02-02)
- [x] Core heatmap rendering stabilized (2026-02-02)

#### Session log
* **2026-02-02** — Axis refactor unlocked stable panning; heatmap is now performant enough to justify rich settings.

---

### F3: Candlestick ↔ Heatmap Unification
**Status:** paused
**Created:** 2026-02-02
**Updated:** 2026-03-17

#### Now

#### Next
- [ ] Add candle body opacity control when heatmap visible
- [ ] Add wick-only mode for minimal obstruction
- [ ] Sync candle aggregation with heatmap resolution

#### Done
- [x] Dedicated plan created (`docs/plans/CANDLES_HEATMAP_UNIFICATION.md`) (2026-02-02)
- [x] Define shared coordinate system for candles + heatmap (2026-02-02)
- [x] Overlay candlesticks directly on heatmap canvas (2026-02-02)
- [x] Ensure Z-ordering works with opacity + blending (2026-02-02)
- [x] Add toggle: "Candles Over Heatmap" (2026-02-02)
- [x] Rename HeatmapDock → ChartDock (class + files) (2026-03-17)

#### Session log
- **2026-02-02** — Performance headroom finally makes unified rendering viable without hacks.
- **2026-02-15** — Completed major UGR refactor for candle stability; axis perf guardrail added for pan/zoom fluidity.
- **2026-03-17** — Renamed HeatmapDock → ChartDock across all source files and CMakeLists.
- **2026-03-17** — De-scoped duplicate toolbar items from F3; chart toolbar wiring tracked under F12.

---

### F4: Footprint Chart Core
**Status:** paused
**Created:** 2026-02-02
**Updated:** 2026-03-16

#### Now
- [ ] Finalize Bid/Ask vs Delta display modes
- [ ] Implement cluster vs profile visual styles
- [ ] Implement width scaling modes (rotation-aware)
- [ ] Render per-cell numbers efficiently (GPU batched)

#### Done
- [x] Basic footprint rendering functional (2026-02-02)

---

### F5: Orderbook Profile Overlay
**Status:** paused
**Created:** 2026-02-02
**Updated:** 2026-03-16

#### Now
- [ ] Render aggregated orderbook profile alongside heatmap
- [ ] Add profile normalization modes
- [ ] Add toggle for profile labels

#### Done
- [x] Profile rendering stub exists (2026-02-02)

---

### F6: Market & Liquidation Bubbles
**Status:** paused
**Created:** 2026-02-02
**Updated:** 2026-03-16

#### Now
- [ ] Add trade bubble rendering (size = volume)
- [ ] Add liquidation bubble rendering (distinct style)
- [ ] Add toggles for both

#### Done
- [x] UI toggles wired (2026-02-02)

---

### F7: MSDF Text Rendering System
**Status:** paused
**Created:** 2026-02-02
**Updated:** 2026-03-16

#### Now
- [ ] Add text LOD system (numbers → hints → none)
- [ ] Color-coded numeric overlays

#### Done
- [x] Plan created (`docs/plans/MSDF_TEXT_LAB.md`) (2026-02-02)
- [x] Implement MSDF font atlas generation (2026-02-02)
- [x] Replace QML Text with GPU MSDF text (2026-02-02)
- [x] Integrate zoom-stable label rendering (2026-02-02)

---

### F8: TPO / Market Profile
**Status:** active
**Created:** 2026-02-02
**Updated:** 2026-03-17

#### Now
- [ ] Finalize TPO letter assignment per timeframe/session slice (stable sequence and rollover)
- [ ] Finish TPO column rendering alignment with shared time/price mapping (no drift vs candles/heatmap)
- [ ] Complete POC/VAH/VAL calculation + render in TPO mode
- [ ] Validate 24H vs 1W session behavior end-to-end (request, stream, renderer)
- [ ] Add focused regression checks for TPO session/timeframe switching

#### Done
- [x] Plan exists (`docs/plans/TPO.md`) (2026-02-02)
- [x] Baseline TPO overlay letter rendering refinements landed (`TpoOverlayRenderer`) (2026-03-13)

#### Session log
- **2026-03-17** — Re-activated F8 as the primary delivery lane. Goal tonight: finish TPO/footprint behavior and session correctness before new feature work.

---

### F9: Chart Interaction & UX Polish
**Status:** paused
**Created:** 2026-02-02
**Updated:** 2026-03-16

#### Now
- [ ] Right-click context menu consistency
- [ ] Unified settings panel behavior
- [ ] Keyboard shortcuts for major toggles

#### Done
- [x] Alert UX documented (2026-02-02)

---

### F10: Coinbase REST Candle History
**Status:** paused
**Created:** 2026-02-02
**Updated:** 2026-03-16

#### Now
- [ ] Roll up 1m candles to custom timeframes (on-demand)
- [ ] Add server-side candle cache per (symbol, timeframe)

#### Done
- [x] Coinbase REST candles fetcher + WS chunking implemented (2026-02-02)
- [x] 1s candles from trade aggregation history (2026-02-02)
- [x] Live candle WS updates with gating (threshold + silence) (2026-02-02)

---

### F11: Heatmap Color System Overhaul
**Status:** active
**Created:** 2026-02-02
**Updated:** 2026-03-16

#### Now

#### Next
- [ ] Add preset system: Classic, Fire, Ocean, Monochrome, Matrix
- [ ] Add color picker widgets for custom bid/ask colors

#### Done
- [x] Multi-stop gradient system implemented (2026-03-01)
- [x] Electric cyan (bid) and Hot orange (ask) palettes implemented (2026-03-01)
- [x] Shader punch-up (gamma/contrast/black floor) (2026-03-01)
- [x] Add palette regeneration trigger — setPaletteGamma/setBidGradient/setAskGradient + dirty flag (2026-03-17)

---

### F12: Chart Toolbar Implementation
**Status:** active
**Created:** 2026-02-02
**Updated:** 2026-03-17

#### Now
- [ ] Wire chart type combo (Candle, Hollow, Line) to rendering logic
- [x] Wire liquidity threshold slider to heatmap filter — retroactive filter on ring + log scale + 80ms debounce (2026-03-19)

#### Done
- [x] Toolbar UI structure (TopToolbar.cpp) (2026-01-XX)
- [x] Symbol search and mode switching functional (2026-02-02)
- [x] Timeframe switching wired and stale-column/wide-column mismatch fixed (2026-03-17)

#### Session log
- **2026-03-17** — Moved timeframe-wiring ownership fully into F12 to remove overlap with F3 and keep chart control work in one lane.

---

### F15: GUI Architecture Refactor (RendererHost + Overlays)
**Status:** active
**Created:** 2026-02-09
**Updated:** 2026-03-16

#### Now
- [ ] Extract Footprint overlay module behind host orchestration
- [ ] Extract Heatmap overlay module behind host orchestration
- [ ] Introduce explicit TimeAuthority and decouple cadence from heatmap ownership

#### Done
- [x] Phase 1: MappingProvider seam and FrameContext snapshotting (2026-02-09)
- [x] Phase 2: Candlestick overlay extraction (2026-02-09)
- [x] Added synthetic candle continuity for sparse sessions (2026-02-10)

---

### F16: Screener Dock + Lab Candle Viewer
**Status:** active
**Created:** 2026-02-11
**Updated:** 2026-03-16

> Full plan: `docs/private/plans/SCREENER_AND_LAB.md`

#### Now
- [ ] Wire crypto row click → heatmap symbol subscription in MainWindowGPU
- [ ] Start screener_server.py on app launch (QProcess)
- [ ] Lab → pure candle viewer (wire CandlestickBatched to live data)

#### Done
- [x] Screener backend (Python server + fetcher + core) (2026-02-13)
- [x] ScreenerDock C++ widget with WS client and table (2026-02-13)

---

### F18: Trading Simulation Stack (Paper + Replay)
**Status:** active
**Created:** 2026-03-14
**Updated:** 2026-03-17

> Blueprint: `docs/TRADING_SIMULATION_BLUEPRINT.md`

#### Now
- [ ] Build a thin replay path from binary trade logs
- [ ] Manual paper-trading polish pass for TP/SL visuals and interaction smoothing

#### Done
- [x] Shared backtest core types and execution model (2026-03-14)
- [x] Unified simulation broker and replay engine (2026-03-14)
- [x] Binary trade-log reader for captured tape (2026-03-16)
- [x] Trade-log replay path + paper trading chart overlays integrated (2026-03-16)
- [x] Polish manual paper trading for forward testing (live mark, renderer-backed order/position overlays, entry-price/PnL pill) (2026-03-16)
- [x] Manual-only TP/SL risk system with server-backed attached risk orders, staged drag UI, confirm/discard, and OCO clearing (2026-03-17)

#### Session log
- **2026-03-17** - Added server-backed manual TP/SL with stop-market SL, take-profit exits, chart-staged drag/confirm flow, renderer-owned bracket geometry, and targeted backend tests. Remaining work is polish, not core feature plumbing.

---

### F19: Refactoring — Safety & Correctness
**Status:** active
**Created:** 2026-03-16
**Updated:** 2026-03-16

> Reference: `docs/REFACTOR_PLAN.md` (Areas 5, 6, 7, 8)

#### Now
- [ ] Implement `DockablePanel` CRTP guard to enforce init order (Area 6)
- [ ] Smart pointer sweep: Audit raw `new` calls in `libs/gui/render/` (Area 5)
- [ ] Introduce strong typedef wrappers for coordinate systems (Area 7)
- [ ] Add protocol round-trip and DataProcessor validation tests (Area 8)

#### Session log
- **2026-03-16** — Consolidated safety-focused refactors from the master plan. Priority: Fix FM-029 (dock lifecycle).

---

### F20: Refactoring — Architecture & Performance
**Status:** active
**Created:** 2026-03-16
**Updated:** 2026-03-16

> Reference: `docs/REFACTOR_PLAN.md` (Areas 1, 2, 3, 4)

#### Now
- [ ] Formalize `IOverlayRenderer` interface and reduce UGR scope (Area 1)
- [ ] Extract `TextureOverlayBase` for heatmap/footprint DRY (Area 2)
- [ ] Implement batch signal coalesce gate in `DataProcessor` (Area 3)
- [ ] Implement Message handler registry in `SentinelStreamClient` (Area 4)

#### Session log
- **2026-03-16** — Consolidated architectural refactors. Goal: decouple overlays and improve GUI thread throughput.
