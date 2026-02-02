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
**Updated:** 2026-02-02

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

---

### F4: Footprint Chart Core

**Status:** active
**Created:** 2026-02-02
**Updated:** 2026-02-02

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
---

### F10: Chart Toolbar Implementation

**Status:** active
**Created:** 2026-02-02
**Updated:** 2026-02-02

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

---
