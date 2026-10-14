# Sentinel TODO

> Personal feature tracker. Append new features, check off tasks, keep moving.

> FORGE re-entry initiative (2026-09-04): the continuous-history initiative coordinates F1, F4, and F8. Baseline, server lifecycle, one-minute durability, bounded heatmap paging, coverage/status UI, and off-I/O heatmap reads have landed; footprint, TPO, and overview rollups remain. Its planning brief is retained privately.

## Queue (moved from STATUS Next 2026-10-13; FIFO into STATUS)

The conductor pulls the top item into `docs/STATUS.md` Next when a slot opens. Items keep their old STATUS numbers for reference.

3. After D-b: candles for the recorded period built from our own journal trades: 1m kept forever (about 25 MB per product per year), 1s kept for a rolling 5 days (about 20 MB per product) and rebuilt from the journal on demand beyond that; REST serves only pre-recording history and odd timeframes (owner decisions 2026-10-06). Chart history requests may ask for 1,000+ bars in one request; the server answers from its cache or fans out 350-bar Coinbase pages in parallel within the rate limit. After D: change-driven live publishing with a per-client max update rate (see the owner decisions block). Then EATEN vs PULLED liquidity, ABSORPTION and REPLENISHMENT (levels refilled after being hit: algos defending a price), drawn at sub-minute resolution from the raw journal, since a 1-minute TWAP column cannot show the order of events (owner's strongest direction, restated 2026-10-06 while watching a live dump): Fable plan after slice D. It crosses
   roller, HMC2, wire and renderer.
3b. Long-term direction (owner 2026-10-07): several Sentinel servers that talk to each other and serve many clients, as TradingView does. Foundations: self-contained days (anchors), journal replication between servers, a fan-out above 8 clients (FM-207), client auto-reconnect, and authentication (the GUI Agent API is unauthenticated). This needs its own architecture plan after one-world.
4. On hold (owner, 2026-10-06): TPO session choices become Session (draws New York, London, Asia and Australia together, like ExoCharts), 24H, 1W, 1M. Changes what the TPO draws; needs its own spec.
5. Order book widget pass, right after D-b1 (D-b changes the server book source, so test and fix on the new source): Following cannot keep the mid centred during a fast dump (the mid row jumps around; `OrderBookDock.cpp` follow/recenter, DECISIONS 2026-10-03 67 ms publication over a 2001-tick window); rows under the best bid looked empty on BTC (thin book or display bug); cumulative mode; one tick size shared with the heatmap. Owner reports 2026-10-06.
6. After one-world (owner, 2026-10-06; GUI polish, not before slice E): FIRST: heatmap chunk loading after a timeframe change (1m->15m) or a fast zoom-out is too slow for a local server (owner, S8a test; S6 A/B already showed 1h settle 509-1297 ms; profile the chunk request path before guessing); hover tooltips are invasive (an order book row tooltip appears over the chart): put all explanatory hover tooltips behind a "learning mode" setting; TPO history starts at 2026-10-03 although it is built from REST candles (paging/budget limit, see the backlog "TPO history budget refusal"); inline volume bars on the market chart (exist only in the Stock Chart dock's CandlestickBatched; not an S8a regression); volume profile draws a fan of triangles from the chart's top-left corner (pre-existing on main; task text in `.claude/acting-orchestrator/parked/vp-triangles.md`; needs `UnifiedGridRenderer` after S8a lands; evidence copied to `.claude/acting-orchestrator/parked/vp-evidence/`); order book cumulative-size mode; one tick size shared by the heatmap and the order book (today they look separate, and where to change the order book tick is unclear); heatmap and candles drift apart slightly while zooming out with the wheel (suspect two scaling paths; check FM-001 / INV-004 first); README pass (owner 2026-10-06): rewrite `README.md` (260 lines, last real edit predates one-world) to match the finished one-world architecture, after slice E and S8b, so it describes the end state once; local-time axis labels (owner 2026-10-06): `TimeAxisModel` already has a `timezone` property (UTC default; America/New_York, Europe/London, Asia/Tokyo) but no control sets it, and `HeatmapGpuLayer.cpp:16-19` hard-codes UTC in its labels; add a setting and route every time label through one timezone; zoom-to-footprint (owner 2026-10-06, from an X demo): candles widen into bid/ask footprint cells and then show numbers as you zoom in; extends existing F4 footprint and TODO "text LOD system (numbers -> hints -> none)", not a new feature; an order book imbalance strip from the Coinbase book (multi-venue, perps and open interest would be a separate data-scope decision); BTC-USD order book at 0.1 tick showed no bids in the 20 rows under the best bid (thin book or display bug, unverified).
6a. After one-world (owner, 2026-10-07): (1) one render performance pass: `CandlestickBatched.cpp:498` allocates every frame, `VolumeProfileRenderer` rebuilds all geometry every frame, and the heatmap/candle drift while zooming out (item 6; suspect two scaling paths, check `TimeAxisModel`, FM-001 / INV-004) is done in the same pass. (1b) DECIDED 2026-10-08 (owner): rows AND columns at whole device pixels through one shared snapped chart mapping (rigid motion like TapeSurf; stepped zoom with hysteresis; every price/time layer locked; heatmap keeps floor()). Fable is writing the plan; coverage blending rejected and archived. Earlier notes: heatmap "wave" on tiny up/down pans (owner 2026-10-07, confirmed pixel pattern, vertical only): price rows have fractional device-pixel heights and the display shader picks exactly one row per pixel with `floor()` (`heatmap_display.frag:19-31`); 4x MSAA does not smooth these internal row boundaries (corrected 2026-10-08 from the GLM-packet brief), so rows render 2 or 3 px tall in a pattern that shifts on sub-pixel pans. Target: TapeSurf-smooth sub-pixel panning ("pixels within pixels"), i.e. exact area-coverage of rows per screen pixel in the heatmap shader (or a row texture with proper filtering), in device pixels (1x and 2x screens). Whole-pixel snapping was considered and rejected (stepped zoom; the owner wants smooth). Outside review (Meta Muse, 2026-10-08; conductor + Codex verification in `.claude/acting-orchestrator/glm-packet/`): fix design = exact vertical coverage in `heatmap_display.frag` (fwidth rows-per-pixel, at most 2 rows per pixel under the zoom clamp, states keep the centre-cell path), with one correction to Muse's shader: scale the colour by the coverage of non-empty rows, or isolated rows in thin books keep the wave. Same slice: the palette half-texel fix (`u = (0.5 + a*255)/512`, asks `+256`) in all three copies (`heatmap_display.frag:57`, `HeatmapPalette.cpp:141`, `heatmap_intensity.frag:94`); today the hottest ~5 bid and coolest ~5 ask texels are unreachable. Also verified TRUE (major): stale-timeframe span builds are not cancelled on a serial bump and occupy the 2-thread pool (likely part of the slow timeframe switch; fix: consumer-aware cancellation). Verified FALSE: the spare cell-buffer GPU race (Metal hazard tracking, 2 frames in flight), lost chunk deliveries via metatype names, `planGrid`/`gridCovers` dead code. Drift: both mappings have the same slopes, so the drift is still unexplained; the owner's note on its direction (horizontal or vertical) decides the next look. (2) A real, repeatable performance benchmark on the owner's machines, then cite its numbers in the README (the old 110 FPS figure was removed 2026-10-07; part of it came from demo data). (3) CI for macOS, Windows and Linux (none exists; no `.github/workflows`): GitHub-hosted runners have no GPU, so GPU tests skip and still report Passed; GPU coverage needs self-hosted runners on the Mac and the Windows machine. (4) README/ARCHITECTURE pass also fixes `docs/MARKETDATA.md:213`, `docs/FEATURES.md` and `docs/REENTRY_REVIEW.md:27` (TLS described as optional, single-quad heatmap, 110 FPS).
7. Backlog: S15 deferred minors (Fable): delete the test-only `ChartLookSettings::notify` std::function (tests should QSignalSpy the group signals) and fix the indentation in `ChartAgentPort.cpp:36-53`; Windows: the Paper Trading dock looks for the backtest binary without the multi-config `Release/`/`Debug/` dir or `.exe` (`PaperTradingDock.cpp:454-465`; pre-existing; with the Windows runtime check); security pass: the client falls back to `verify_none` with a warning when `ca_file` is missing (`SentinelStreamClient.cpp:168-181`); the same straight-RGB-into-premultiplied-material bug as the VP had remains in CandlestickBatched.cpp:575-582 (Stock Chart volume bars draw brighter than their alpha) and CandlestickOverlayItem.cpp:530 (body opacity and wick alpha); fixing them changes looks (owner); VP per-frame markDirty and bins copy (AGENTS 5 follow-up); security hardening pass after one-world: on a screener request the deployed server runs `uv run python scripts/screener/screener_fetch.py` from the repo checkout (agent-writable code with the owner's privileges); the ntfy topic is in plain text in the grafana plist; client auth; where Sentinel runs after one-world (owner 2026-10-06): a Raspberry Pi 5 NAS with the 5 TB drive (backup/archive; the roller's continuous HMC2 writes prefer an SSD), and the server on the Pi or the Windows machine serving remote clients over TLS; test across the owner's machines; token-spend and model-choice study (owner, after one-world): data = `ma-panel` snapshots, `~/.codex/sessions` rollouts, subagent usage in conductor transcripts, and the per-slice review files in `.claude/acting-orchestrator/<slice>/` (review rounds, findings, which were real); keep those folders until the study; optional later: git trailers on landing merges (Slice/Risk/Writer/Reviewer/Review-Rounds/Findings/Tokens) via `agent-worktree.sh land --trailer`, backfill with `git notes --ref=audit`; `config/client_config.yaml` top-level `server:` is ignored (only `client.server` is read; nearly pointed an agent GUI at :8080); PEPE at its native 1e-8 tick draws ~200 px rows and logs "native rows exceed int32" in the GUI (the tick is ~0.25-1% of PEPE's price, so few levels exist: the blocks are partly real. Fix direction from a 2026-10-06 look at other platforms: readable price-axis units for tiny prices, as exchanges list 1000PEPE; a minimum visible price span or row-height cap in auto-scale; no fake sub-tick grid); Codex writers could commit in the sandbox via a separate-gitdir clone (parked, `docs/AGENT_WORKFLOW.md`); tick-size combo text clips at its fixed 84 px width ("$0.15 (unavailable)", pre-existing); owner GUI Agent API (17100) is unauthenticated; GUI RSS ~1.5 GB after 10 min; capture fan-out
   `poll()` error loop without sleep (`CaptureFanout.cpp:417`, low); W2d live GUI display and Windows runtime
   unverified; TPO history budget refusal in the GUI log; nightly backup of `/Volumes/T7/sentinel-data` when the
   owner's new HDD arrives; internal disk space.

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
**Status:** v1 shipped (2026-05-08); follow-ups queued
**Created:** 2026-01-30
**Updated:** 2026-09-04

#### Now
- [x] Auto history request on symbol change / reconnect (2026-03-19)
- [x] Scroll-past-cache fetch (request older history) (2026-03-26)
- [x] Persist derived heatmap history to disk + reload on startup with gap handling (2026-05-08)
- [x] Disk fallthrough on `heatmap_history_request` past in-RAM ring (2026-05-08)
- [x] Protocol additive fields: `start_time`, `oldest_available_ms` (2026-05-08)
- [x] Retention policy: delete day files older than N days at startup (2026-05-08)
- [x] Preserve historical heatmap columns across band recenter with worker-side page resampling (2026-09-04)
- [x] Shade missing historical buckets separately from recorded zero-liquidity columns (2026-09-04)
- [x] Show heatmap history loading and storage-floor states on the chart (2026-09-04)
- [x] Move persisted heatmap page reads/encoding off the single network I/O thread with a bounded worker queue (2026-09-04)

#### Next
- [ ] Phase 5b: zstd-compress rotated day files + streaming decompressed reader (deferred from F1 v1)
- [ ] Periodic retention sweep (currently runs only at startup)
- [x] Client-side surface of `oldest_available_ms` to suppress redundant scroll-left fetches at the floor (2026-09-04)
- [ ] Apply the F1 template to footprint persistence
- [ ] Apply the F1 template to TPO persistence
- [ ] Derived timeframe rollups (non-anchor TFs from 1s/1m/1h/1d)
- [ ] Per-client TF stream (client asks for derived TF)
- [x] StatusBar metrics wiring — upload bandwidth MB/s wired (FPS/CPU/GPU already done) (2026-03-19)
- [x] Page persisted heatmap history into the bounded GPU ring so the canvas feels infinite without needing an infinite texture (2026-09-04)
- [ ] Define a sliding-window world-history architecture for heatmap/TPO/footprint so viewport panning pages through larger persisted datasets instead of treating fixed grid width/height as total history/range

#### Later
- [ ] Heatmap settings panel v2 (extra controls + templates)
- [ ] Candlestick overlay Phase 1 (viewport-driven candles)

#### Done
- [x] HMCL v1 on-disk format: fixed-stride slots, header + per-record CRC32 (2026-05-08, `5789f0c`)
- [x] HeatmapColumnStore writer: slot-addressed idempotent appends, POSIX `flock` exclusive lock (2026-05-08, `33105c3`)
- [x] Streamer integration behind `heatmap.persistence_enabled` config flag (2026-05-08, `c6c6135`)
- [x] `scripts/inspect_hmcol.py` read-only inspector (2026-05-08, `c6c6135`)
- [x] HeatmapColumnStore reader half + bootstrap: kill/rebuild/restart resume validated on real BTC-USD data (2026-05-08, `70e8fd6`)
- [x] `data/` added to `.gitignore` (2026-05-08, `ecaa654`)
- [x] Day-spanning `fetchRange` + disk fallthrough in `fetchHistory` (2026-05-08, `a237cdd`)
- [x] Protocol: `start_time` on request, `oldest_available_ms` on chunk (additive, schema_version unchanged) (2026-05-08, `fa901e2`)
- [x] Retention policy: per-startup deletion of day files older than `persistence_retention_days` (2026-05-08, `4eeadd9`)

#### Session log
- **2026-03-15** - Persistence audit found durable raw trade logs on disk, but heatmap history remains in-memory only and is cleared on band recenter/reset. Future direction: keep the live one-quad GPU path, but persist world-time history outside the renderer and page visible columns into the bounded ring.
- **2026-03-17** - Captured the next architectural issue explicitly: chart layers need a larger persisted world-history model with a bounded GPU window, so long-range heatmap/TPO browsing can feel infinite without fixed-grid hard stops.
- **2026-03-26** - Implemented scroll-past-cache fetch: UGR.onViewportChanged detects when visibleTimeStart < oldest cached slice, debounces 300ms, emits heatmapHistoryNeeded(tfMs, endTimeMs, count). MainWindowGpu wires it to requestHeatmapHistory with current symbol. Full project builds clean.
- **2026-05-07** - Drafted full persistence design: `../Sentinel-Private/private/plans/F1_HEATMAP_PERSISTENCE.md`. 6-phase plan covering on-disk `.hmcol` format (fixed-stride per (symbol,tf,day), per-record CRC, additive over existing protocol), writer hook into `HeatmapTwapStreamer::storeHistory`, server bootstrap via `primeRing`, range-query disk fallthrough in `getHeatmapHistory`, restart story for hot iteration. Existing `TickBinaryLogger` left untouched as raw-tape audit source. Awaiting review before any code change.
- **2026-05-08** - Owner clarified scope: v1 is 1m-only, no rollup engine, no compression. Slot-addressed idempotent writes (not pure append). OS-level exclusive lock for single-writer enforcement. Recenter explicitly handled by per-column self-describing price range — no transform at persistence. First milestone: kill server, rebuild, restart, see yesterday's 1m heatmap. Plan updated.
- **2026-05-08** - F1 v1 shipped end-to-end across phases 1-5a (commits `5789f0c`, `33105c3`, `c6c6135`, `70e8fd6`, `a237cdd`, `fa901e2`, `4eeadd9`). 42 servermodel tests green. First milestone validated by user on real BTC-USD data: kill server, restart, log shows "primed 6 column(s) for BTC-USD tf=60000 from data/heatmap" before live timer fires; `heatmap_history_request` now falls through to disk past the in-RAM ring; protocol additions are backward-compatible. zstd compression (phase 5b) deferred — adds external dep + reader-path rewrite, not worth taking on autonomously.
- **2026-09-04** - FORGE re-entry restored a clean build, bounded server session lifetimes and queues, enabled durable one-minute collection, and added 1,024-column historical GPU paging. Older replies now use a separate page path, recentered price bands are resampled, history-floor and stale-response checks stop redundant or incorrect loads, and returning to auto-scroll reloads the newest page. A separate coverage mask shades missing buckets while leaving recorded zero-liquidity columns unshaded. The chart reports page loading and shows the storage floor only when the manual viewport reaches it. Persisted page reads and JSON/base64 encoding now run on a two-worker pool capped at eight pending jobs instead of the single network I/O thread. Full automated suite passes; live visual soak remains.

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
**Updated:** 2026-09-04

#### Now
- [ ] Finalize TPO letter assignment per timeframe/session slice (stable sequence and rollover)
- [ ] Finish TPO column rendering alignment with shared time/price mapping (no drift vs candles/heatmap)
- [ ] Validate 24H vs 1W session behavior end-to-end (request, stream, renderer)
- [ ] Add focused regression checks for TPO session/timeframe switching

#### Done
- [x] Plan exists (`docs/plans/TPO.md`) (2026-02-02)
- [x] Baseline TPO overlay letter rendering refinements landed (`TpoOverlayRenderer`) (2026-03-13)
- [x] POC/VAH/VAL computation in TpoStreamState::computePocVahVal() (2026-03-26)
- [x] DataProcessor emits tpoPocVahValReady after each slice ingest (2026-03-26)
- [x] UGR stores and renders POC (gold 2.5px) + VAH/VAL (cyan 1.2px) as horizontal lines (2026-03-26)
- [x] Correct W1 and Australia session rollover arithmetic with focused boundary tests (2026-09-04)

#### Session log
- **2026-03-17** — Re-activated F8 as the primary delivery lane. Goal tonight: finish TPO/footprint behavior and session correctness before new feature work.
- **2026-03-26** — Implemented full POC/VAH/VAL pipeline: TpoStreamState computes histogram + 70% value area expansion, DataProcessor emits per-slice, UGR renders 3 colored horizontal line nodes (gold POC, cyan VAH/VAL) on the render thread under m_tpoPendingMutex.
- **2026-09-04** — SessionManager now selects the latest open at or before the query for all daily and weekly sessions; W1 Sunday-before-open and Australia-after-open regressions are covered by pure-core tests.

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
- [x] Add preset system: Electric, Fire, Ocean, Monochrome, Matrix — toolbar combo + UGR Q_INVOKABLE (2026-03-19)
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
- [x] Wire chart type combo (Candle, Hollow, Line) to rendering logic — CandleStyle enum + geometry per mode (2026-03-19)
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
**Updated:** 2026-03-26

> Full plan: `../Sentinel-Private/private/plans/SCREENER_AND_LAB.md`

#### Now
- [x] Wire crypto row click → heatmap symbol subscription in MainWindowGPU (already done via ScreenerDock::rowSelected → onAssetSymbolSelected)
- [x] Start screener_server.py on app launch (QProcess) (2026-03-26)
- [ ] Lab → pure candle viewer (wire CandlestickBatched to live data)

#### Done
- [x] Screener backend (Python server + fetcher + core) (2026-02-13)
- [x] ScreenerDock C++ widget with WS client and table (2026-02-13)
- [x] Auto-trigger initial screener fetch when stream client connects (2026-03-26)

#### Session log
- **2026-03-26** — Added startScreenerServer()/stopScreenerServer() to MainWindowGPU: uses QProcess + uv run, searches 5 candidate paths relative to app dir, logs to sLog_App, killed on closeEvent. Auto-starts on first showEvent.

---

### F18: Trading Simulation Stack (Paper + Replay)
**Status:** active
**Created:** 2026-03-14
**Updated:** 2026-03-26

> Design details are retained in the sibling Sentinel-Private repository.

#### Now
- [ ] Manual paper-trading polish pass for TP/SL visuals and interaction smoothing

#### Done
- [x] Shared backtest core types and execution model (2026-03-14)
- [x] Unified simulation broker and replay engine (2026-03-14)
- [x] Binary trade-log reader for captured tape (2026-03-16)
- [x] Trade-log replay path + paper trading chart overlays integrated (2026-03-16)
- [x] Polish manual paper trading for forward testing (live mark, renderer-backed order/position overlays, entry-price/PnL pill) (2026-03-16)
- [x] Manual-only TP/SL risk system with server-backed attached risk orders, staged drag UI, confirm/discard, and OCO clearing (2026-03-17)
- [x] Backtest tab in PaperTradingDock: file picker, symbol/spread/qty/maxpos params, QProcess sentinel_backtest runner, live stdout output, status label (2026-03-26)

#### Session log
- **2026-03-17** - Added server-backed manual TP/SL with stop-market SL, take-profit exits, chart-staged drag/confirm flow, renderer-owned bracket geometry, and targeted backend tests. Remaining work is polish, not core feature plumbing.
- **2026-03-26** - Added Backtest tab to PaperTradingDock with binary trade log file picker, AvendellaMM params, QProcess runner, real-time stdout/stderr output, and yellow/green/red status label. Binary located by searching candidate paths relative to app dir.

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

---

### F21: Recording mode — owner's first-look punch list
**Status:** active
**Created:** 2026-09-28
**Updated:** 2026-09-28

#### Now (tonight, together)
- [ ] Zoom logging session: log viewport time span, price span, pixels, px per row, requested vs server-chosen tick, layer (near/deep) at every zoom step; owner zooms, agents read the log (`SENTINEL_PROBES=heatmap.recording` + a viewport-math probe)
- [ ] Tick policy bug: zooming IN on the price axis raised the cell tick ($50 -> $100); close zoom showed $100-$200 cells where $1-$20 detail is wanted
- [ ] Tick policy design: heatmaps want long thin order lines (~2 px rows), not square cells; do not group liquidity pockets too much; consider 1-2.5-5 steps ($25, $250) and whether the deep layer's native $10 grid should become $5 so $25 rows are possible; TapeSurf appears to hold ~$50 rows at every zoom
- [ ] Price-axis tick labels vs heatmap cell size: make them agree (axis showed $20 and $200 steps; cells $20 and $100)
- [ ] Candles overlap the price axis when panned next to it (clip to the plot area / draw behind the axis)
- [ ] Smooth level-of-detail transitions: keep the previous band on screen until the new one arrives (no black flash), then crossfade
- [ ] Theme check: the colours seen in recording mode may come from the theme dropdown (owner may have switched themes); confirm which palette recording mode uses and that the dropdown applies to it

#### Next
- [ ] Make `heatmap.source: recording` the default; retire the legacy recorder (it only keeps a ~±1.2% window)
- [ ] FPS readout: shows ~20 when idle (render-on-demand, frames only when data changes) and 110-125 while panning; show frame time or "idle" instead of a misleading FPS
- [ ] Merge branch `lt-sol/label-style` (TapeSurf text: 3 significant figures, cell-tinted, value opacity, zoom fade) after the owner looks at it
- [ ] Candles: body width as a fraction of the column (thin slivers when zoomed in), like TapeSurf
- [ ] Look and feel vs TapeSurf: volume bars at the bottom, live depth edge on the right, visible high/low price tags, relative time axis
- [ ] Learning session: how HFT-grade terminals structure data paths and rendering (C++ performance), applied to Sentinel

#### Later
- [ ] Multi-venue aggregation (Binance, others) into one book view
- [ ] Relaunch post (GitHub, Reddit) with recording-mode screenshots / GIF (repo has ~20 stars, 3 forks)
- [ ] CopeNet: short-horizon ledger claims scored on the recorded tape

#### Done
- [x] Recording v2 live: near +/-5% @ $1 and whole-book deep layer @ $10 on the T7, fixed size scale, ~5.5 GB/year (2026-09-28)
- [x] Serving S1-S4: server-built LOD pages, client re-band, shader value mode, live provisional columns (2026-09-28)
- [x] Agent API v1 (state, viewport, candles, book, trades, walls, controls, render-ordered screenshots) + CopeNet `sentinel.*` tools (2026-09-28)
- [x] Multi-agent git workflow: rebase at hand-off, `agent-worktree.sh land` merge queue, rerere (2026-09-28)

#### Session log
- **2026-09-28** — Owner's first look at recording mode: "exactly how I want it"; the deep layer shows the resting order that stopped price near $83.9k. Live columns and MSDF text update. Items above captured from the owner's notes.
