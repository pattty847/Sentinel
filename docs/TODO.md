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
