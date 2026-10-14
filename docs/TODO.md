# Sentinel TODO

Open work only, one line per item. Done work is in `git log --first-parent`; decisions are in
`_agent/DECISIONS.md`. The conductor pulls the top Queue item into `docs/STATUS.md` Next when a slot
opens. Delete an item when it lands or is dropped; do not keep Done lists or session logs here.
Detail belongs in the linked plan or file, not in the item.

## Queue (ordered)

1. **Journal-built candles + liquidity events** (owner's strongest direction, 2026-10-06; needs a Fable plan; crosses roller, HMC2, wire, renderer):
   - candles for the recorded period from our own journal trades: 1m kept forever (~25 MB/product/year), 1s rolling 5 days (~20 MB/product) and rebuilt from the journal beyond that; REST only for pre-recording history and odd timeframes;
   - history requests may ask 1,000+ bars; the server answers from cache or fans out 350-bar Coinbase pages within the rate limit;
   - change-driven live publishing with a per-client max update rate;
   - EATEN vs PULLED liquidity, ABSORPTION, REPLENISHMENT at sub-minute resolution from the raw journal (a 1-minute TWAP column cannot show event order).
2. **Order book widget pass** (owner reports 2026-10-06): following cannot keep the mid centred in a fast dump (`OrderBookDock.cpp` follow/recenter; DECISIONS 2026-10-03, 67 ms over a 2001-tick window); BTC rows under the best bid looked empty at 0.1 tick (thin book or display bug, unverified); cumulative-size mode; one tick size shared with the heatmap (today separate, and the order book tick control is hard to find).
3. **GUI polish** (owner, 2026-10-06):
   - heatmap chunk loading after a timeframe change (1m->15m) or fast zoom-out is too slow for a local server (S6 A/B: 1h settle 509-1297 ms); profile the chunk request path before guessing;
   - put explanatory hover tooltips behind a "learning mode" setting (an order book tooltip covers the chart);
   - TPO history starts 2026-10-03 although built from REST candles (GUI log: "TPO history budget refusal");
   - inline volume bars on the market chart (only the Stock Chart dock's `CandlestickBatched` has them);
   - heatmap and candles drift apart slightly on wheel zoom-out; both mappings have the same slopes, cause unknown; the owner's note on direction decides the next look (FM-001, INV-004);
   - local-time axis labels: `TimeAxisModel` has a `timezone` property no control sets; `HeatmapGpuLayer.cpp` hard-codes UTC; add a setting and route every time label through one timezone;
   - zoom-to-footprint: candles widen into bid/ask footprint cells, then numbers appear (extends Footprint and text LOD below);
   - order book imbalance strip from the Coinbase book.
4. **Render performance pass**: `CandlestickBatched.cpp` allocates every frame; `VolumeProfileRenderer` rebuilds all geometry and copies bins every frame (AGENTS 5); fold in the drift check from item 3.
5. **Repeatable performance benchmark** on the owner's machines; cite its numbers in the README (the old 110 FPS figure was removed 2026-10-07).
6. **README + ARCHITECTURE pass** for the finished one-world architecture; also fix `docs/MARKETDATA.md` (TLS described as optional, single-quad heatmap, 110 FPS).
7. **CI** for macOS, Windows, Linux (no `.github/workflows` today). Hosted runners have no GPU, so GPU tests skip and report Passed; GPU coverage needs self-hosted runners on the Mac and the Windows machine.

## Security (owner decides when)

- Client falls back to `verify_none` with a warning when `ca_file` is missing (`SentinelStreamClient.cpp:175-181`).
- On a screener request the deployed server runs `screener/screener_fetch.py` from the repo checkout (`SentinelStreamServer.cpp:1142-1154`): agent-writable code with the owner's privileges.
- GUI Agent API (17100) has no authentication; client auth does not exist.
- The ntfy topic is in plain text in the grafana plist.

## Direction (owner, needs its own plan)

- Several Sentinel servers that replicate journals and serve many clients (owner 2026-10-07). Foundations: self-contained days (anchors), journal replication, fan-out above 8 clients (FM-207), client auto-reconnect, authentication.
- Where Sentinel runs: Raspberry Pi 5 NAS with the 5 TB drive for backup/archive (roller HMC2 writes prefer an SSD); server on the Pi or the Windows machine serving remote clients over TLS.
- On hold (owner 2026-10-06): TPO sessions become Session (New York, London, Asia, Australia drawn together, like ExoCharts), 24H, 1W, 1M. Needs a spec.
- Token-spend and model-choice study: data = `ma-panel` snapshots, `~/.codex/sessions` rollouts, subagent usage in conductor transcripts, per-slice review files in `.claude/acting-orchestrator/<slice>/` (keep those folders until the study), land trailers.
- Multi-venue aggregation (Binance and others) into one book view.
- Relaunch post (GitHub, Reddit) with recording-mode screenshots or a GIF.
- CopeNet: short-horizon ledger claims scored on the recorded tape.

## Backlog (bugs and small fixes)

- `config/client_config.yaml` top-level `server:` is ignored (only `client.server` is read); nearly pointed an agent GUI at :8080.
- Straight RGB into a premultiplied material (as the VP had): `CandlestickBatched.cpp` volume bars draw brighter than their alpha; `CandlestickOverlayItem.cpp` body opacity and wick alpha. Fixing changes looks (owner).
- PEPE at its native 1e-8 tick draws ~200 px rows and logs "native rows exceed int32". Direction: readable price units for tiny prices (as exchanges list 1000PEPE), a minimum visible price span or row-height cap in auto-scale; no fake sub-tick grid.
- Windows: Paper Trading looks for the backtest binary without the multi-config `Release/`/`Debug/` dir or `.exe` (`PaperTradingDock.cpp`).
- Tick-size combo text clips at its fixed 84 px width ("$0.15 (unavailable)").
- Capture fan-out `poll()` error loop has no sleep (`CaptureFanout.cpp:418`, low).
- GUI RSS ~1.5 GB after 10 min (unprofiled).
- FPS readout shows ~20 when idle (render on demand) and 110-125 while panning; show frame time or "idle" instead.
- S15 minors (Fable): delete the test-only `ChartLookSettings::notify` std::function (tests should QSignalSpy the group signals); fix the indentation in `ChartAgentPort.cpp:36-53`.
- Opt-in `ShadowTest.HandoffLatencyBenchmark` (`test_shadow.cpp`).
- SentinelBar `PlanLocation.resolve` misses `initiatives/` trailers and relative pinned paths outside `research/` (PR 44 review, minor).
- W2d live GUI display and the Windows runtime are unverified.
- Unverified since 2026-09-28 (owner's first look at recording mode): candles overlap the price axis when panned next to it (clip to the plot area); price-axis tick steps disagree with heatmap cell size; confirm the theme dropdown applies to recording mode.
- Nightly backup of `/Volumes/T7/sentinel-data` (to the MacBook Pro over Tailscale needs Remote Login + an SSH key, or to the owner's new HDD); internal disk space.

## Feature ideas (unverified against current code since 2026-03; check before starting)

- **Heatmap:** blending mode toggle (additive / max / overwrite); per-side bid/ask hue and saturation; value normalization modes (global, visible range, session); custom bid/ask colour pickers; magnifier.
- **Heatmap labels:** cache label quads per column; rebuild only when column, viewport or thresholds change.
- **Candles over heatmap:** body opacity control when the heatmap is visible; wick-only mode; candle aggregation synced with heatmap resolution.
- **Footprint:** bid/ask vs delta modes; cluster vs profile styles; rotation-aware width scaling; GPU-batched per-cell numbers.
- **Order book profile overlay** beside the heatmap, with normalization modes and labels.
- **Trade and liquidation bubbles** (size = volume; liquidations styled apart; toggles). FM-259: bubbles may not draw if the overlay attaches before trades.
- **Text:** LOD (numbers -> hints -> none); colour-coded numeric overlays.
- **TPO:** stable letter assignment per timeframe/session slice; column alignment with the shared time/price mapping; 24H vs 1W end to end; regression checks for session and timeframe switching.
- **Interaction:** consistent right-click menus; unified settings panel; keyboard shortcuts for major toggles.
- **History:** footprint and TPO history persistence and overview rollups (the heatmap has HMC2; these two do not).
- **Candles:** roll 1m up to custom timeframes on demand (may fold into Queue 1).
- **Lab:** pure candle viewer (`CandlestickBatched` on live data).
- **Paper trading:** TP/SL visual and interaction polish. FM-256: a TP/SL exit order can be left New.
- **Look vs TapeSurf:** volume bars at the bottom, live depth edge on the right, high/low price tags, relative time axis.
- **Code safety:** `DockablePanel` init-order guard (FM-029); raw `new` audit in `libs/gui/render/`; strong typedefs for coordinate systems; protocol round-trip and DataProcessor validation tests.
- Learning session: how HFT-grade terminals structure data paths and rendering, applied to Sentinel.
