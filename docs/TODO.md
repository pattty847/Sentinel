# Sentinel TODO

Open work only, one line per item; delete it when it lands or is dropped. History is git log.
The conductor pulls the next wave item into `docs/STATUS.md` Next. Detail goes in the linked plan or file.

## Waves (top-down; one recorder/server item at a time; deploys wait for the owner)

- **Wave 3 (now):** footprint/why-it-moved plan (Fable: footprint, CVD, eaten/pulled, bubbles), aesthetics, heatmap seam, trade bubbles rethink.
- **Wave 4:** Security, land follow-ups, Feature ideas check. Then the Queue.

## Queue (ordered)

1. **Journal-built candles + liquidity events** (owner's strongest direction, 2026-10-06; needs a Fable plan; crosses roller, HMC2, wire, renderer):
   - candles for the recorded period from our own journal trades: 1m kept forever (~25 MB/product/year), 1s rolling 5 days (~20 MB/product) and rebuilt from the journal beyond that; REST only for pre-recording history and odd timeframes;
   - history requests may ask 1,000+ bars; the server answers from cache or fans out 350-bar Coinbase pages within the rate limit;
   - change-driven live publishing with a per-client max update rate;
   - EATEN vs PULLED liquidity, ABSORPTION, REPLENISHMENT at sub-minute resolution from the raw journal (a 1-minute TWAP column cannot show event order).
2. **Order book widget pass** (owner reports 2026-10-06): following cannot keep the mid centred in a fast dump (`OrderBookDock.cpp` follow/recenter; DECISIONS 2026-10-03, 67 ms over a 2001-tick window); BTC rows under the best bid looked empty at 0.1 tick (thin book or display bug, unverified).
3. **GUI polish** (owner, 2026-10-06):
   - put explanatory hover tooltips behind a "learning mode" setting (an order book tooltip covers the chart);
   - TPO history depth: diagnosed 2026-10-23, not a refusal: the GUI asks for `tpoSessions` (default 5, clamp 1..8 at `TpoOverlayRenderer.cpp:95`) h24 sessions, so it showed back to 10-03 on 10-08. Owner decides: deeper history (raise the clamp and `kMaxHistoryPages`), and whether REST-candle columns before the 24 h trade tape are labelled approximate (INV-039 target) or INV-030/039 are amended. The 250k-trade page failure is fixed on main (`46edf77c`, not deployed); open with it: candle-derived TPO letters carry no approximate marker (INV-039).
   - inline volume bars on the market chart (only the Stock Chart dock's `CandlestickBatched` has them);
   - heatmap and candles drift apart slightly on wheel zoom-out; both mappings have the same slopes, cause unknown; the owner's note on direction decides the next look (FM-001, INV-004);
   - local-time axis labels: `TimeAxisModel` has a `timezone` property no control sets; `HeatmapGpuLayer.cpp` hard-codes UTC; add a setting and route every time label through one timezone;
   - zoom-to-footprint: candles widen into bid/ask footprint cells, then numbers appear (extends Footprint and text LOD below);
   - **Trade bubbles rethink** (owner 2026-10-25; folded into the wave 3 footprint plan): at 1m zoomed out bubbles bury the chart; zoomed in they sit at trade time, the owner expected candle centre. Topics: x placement, size scaling, filtering large/price-moving trades from passive flow (min size, per-slot aggregation, sweeps). Bookmap is the floor. No bubble history after a GUI restart.
   - **Heatmap stops after sub-minute switches** (owner 2026-10-25, to debug): panning/zooming at 1 s, then 1m -> 1s -> 100ms, left the heatmap empty from the switch time on; back on 1m, time after the switch shows the hatched not-built background. Log `~/Library/Logs/Sentinel/sentinel-gui-20261024-235229-50461.log` 18:10-18:14 EDT: `Heatmap CPU ceiling refused visible spans ... tf=3600000 spans=7 bytes=1189108208` at 18:10:35, live subscribe `sub=5206` at 18:12:56 on 1m, no live subscribe at 1 s/100 ms. Live columns are requested only for tf >= 1m (`HeatmapSourceController.cpp:882`). Check whether the 1m live resubscribe resumes and whether the CPU ceiling (about 1 GB committed after a long session) is what leaves spans unbuilt.
4. **Repeatable performance benchmark** on the owner's machines; cite its numbers in the README (the old 110 FPS figure was removed 2026-10-07).
5. **README + ARCHITECTURE pass** for the finished one-world architecture; also fix `docs/MARKETDATA.md` (TLS described as optional, single-quad heatmap, 110 FPS).
6. **CI** for macOS, Windows, Linux (no `.github/workflows` today). Hosted runners have no GPU (GPU tests skip and report Passed); GPU coverage needs self-hosted runners on the Mac and Windows.

## Security (owner decides when)

- Client falls back to `verify_none` with a warning when `ca_file` is missing (`SentinelStreamClient.cpp:175-181`).
- On a screener request the deployed server runs `screener/screener_fetch.py` from the repo checkout (`SentinelStreamServer.cpp:1142-1154`): agent-writable code with the owner's privileges.
- GUI Agent API (17100) has no authentication; client auth does not exist.
- The ntfy topic is in plain text in the grafana plist.
- Writer sandbox: Codex writers commit into a quarantine object directory that land imports by fetch; drop their write access to the shared object store (2026-11-08 audit follow-up; details in Sentinel-Private `private/security-audit-verified.md`).
- REST candle client: check the server certificate hostname like the WebSocket transport does (`CoinbaseRestClient.cpp:123-171`).
- Land follow-ups (2026-11-08 review minors): rebase onto the checked `main_tip`, not `main`; reuse the main checkout's rerere cache in the private land repo; manual `agent-worktree.sh remove` refuses a dirty worktree unless forced.

## Direction (owner, needs its own plan)

- Live sub-minute heatmap (owner 2026-10-28, another day): 1 s and 100 ms heatmap slices kept in RAM for the live edge, built client-side from the live book replica (no recording change; history stays minute columns). Today live heatmap requests need tf >= 1 min (`libs/gui/render/heatmap/HeatmapSourceController.cpp:882`), so 1 s / 100 ms show minute columns behind sub-second candles. HIGH (GPU heatmap core). Lock-to-mid a1 follows trades meanwhile.
- Several Sentinel servers that replicate journals and serve many clients (owner 2026-10-07). Foundations: self-contained days (anchors), journal replication, fan-out above 8 clients (FM-207), client auto-reconnect, authentication.
- Where Sentinel runs: Raspberry Pi 5 NAS with the 5 TB drive for backup/archive (roller HMC2 writes prefer an SSD); server on the Pi or the Windows machine serving remote clients over TLS.
- On hold (owner 2026-10-06): TPO sessions become Session (New York, London, Asia, Australia drawn together, like ExoCharts), 24H, 1W, 1M. Needs a spec.
- Token-spend and model-choice study: data = `ma-panel` snapshots, `~/.codex/sessions` rollouts, subagent usage in conductor transcripts, per-slice review files in `.claude/acting-orchestrator/<slice>/` (keep those folders until the study), land trailers.
- Multi-venue aggregation (Binance and others) into one book view.
- Relaunch post (GitHub, Reddit) with recording-mode screenshots or a GIF.
- CopeNet: short-horizon ledger claims scored on the recorded tape.

## Backlog (bugs and small fixes)
- Parked (owner 2026-11-07): fetch only the visible price range plus a margin. Rollups fixed load time (1h 76 chunks / 0.87 s, 1d 6 / 0.22 s). Revive only if 1m wide zoom misses history after raising the GPU cap (Heatmap settings > Budgets, default 320 MiB).

- Roller: delete rollup .shr files after rewriting closed-day minutes (from `lt-claude/hmc2-rollup-levels`): the server rebuilds a day's 1h/1d rollups only when its minute files' names, sizes or mtimes change, and running GUIs keep sealed chunks until restart. Any tool that rewrites a closed UTC day's HMC2 minute files (roller backfill, `libs/core/roller/ShadowRoller.cpp`) must delete `<rollup_dir>/<symbol>/<layer>/{3600000,86400000}/<day>.shr` (`recording::rollupRootFor`). Contract: docs/MARKETDATA.md "Changed content".
- Heatmap reference numbers from TapeSurf's debug HUD (owner 2026-11-06, reading of a screenshot, not measured): at most 48 chunks held with a prefetch radius of 6, two preallocated ~200 MB GPU buffers (raw + LOD), ~11.5 B per entry on the GPU, ~2.6 s fresh load. Candidate next slice after rollups ship: fixed chunk budget with prefetch radius and preallocated GPU buffers.
- Show only what is real (owner 2026-10-28, soon; one pass): (a) Live candle close lags trades: trade bubbles print below the close and the close reaches them later. Server gate `should_emit_update` (`libs/core/protocol/SentinelStreamServer.cpp:1297`, `candles:` in `config/server_config.yaml`) sends a 1 m+ update only on a new high/low or a close move >= max(2 ticks, 2 bps, about $20 on BTC); otherwise the 1000 ms silence rule, checked only on the next trade, so a quiet stretch's last close can stay stale. Owner direction: a close change of one native tick is always sent; trades that do not move price only add volume and ride the next coalesced update (a short timer, so it feels like riding price without no-op spam). Server change: deploy needs the owner OK. (b) Revisit the TWAP heatmap colouring (owner: also shows what is not real).
- Aesthetics pass (owner 2026-11-08, after the recorder fixes): replace the red-to-black palette (owner dislikes it; owner may bring colour variants from the Claude app) and add a zoom-out smoothing mode (blend cells smaller than a pixel instead of picking one; crisp cells when zoomed in). Fix size saturation (PEPE billions draw flat cyan/yellow) in every theme and palette. Owner A/B in the lab.
- Startup: live book and heatmap wait for the candle replay (owner 2026-11-08, parked; up to ~23 s for BTC late in the UTC day, ~2.7 s without the replay). Read-only investigation: ordering only (one roller worker per product: `restoreCandles` before `roll`, ShadowRoller.cpp ~1526); book/HMC2 need no candle state. Fix sketch: replay on a helper thread with its own journal order (`beyond = {}`; `LiveSource::older` mutates inventory), set `tap.delivered = through`, roll at once, ModelTap buffers trades/watermarks until the result, then handoff + install + flush; bounded buffer, join on stop/retry, ~9 tests incl. TSan. Risk medium-high; tape also pauses unless onTrade is split. A trades-only parse is not a free alternative: skipping book records broke trade delivery in the restart-bars slice.
- Heatmap seam left of the now column (owner 2026-11-08): when zooming, a vertical seam shows between the live (now) column and the history to its left. Check where live columns meet fetched history chunks (boundary column drawn twice or skipped).
- Fine-tick grey hatch (owner 2026-11-05, BTC-USD): switching 1m -> 5m (tick was about $10) dropped the tick to $2 and drew a tall grey hatched block over part of the heatmap, continuing down when panned; toolbar showed "veiled", status "History partial". Happens at $1 and $2, gone at >= $5. Check what draws the hatch (coverage/veil fill?) and why the tf switch changed the tick.
- Time-axis labels stop partway (owner 2026-11-05): at some x zooms the date labels cover only the left part of the axis (e.g. 24 Sep..25 Oct, then blank), at others they reach the right edge. Suspect the "nice tick" step choice in the time-axis label layout.
- Slow-timeframe history fill (owner 2026-10-28, investigate): on a slow tf a small band loads first and history fills in around it, as if fetched twice or filled from two places. Check for a duplicate or split history request before optimizing.
- Now-column flicker (owner 2026-10-28): the live column flickers very faintly; suspect the MSDF label update. Confirm on a live run before fixing.
- Owner 2026-11-07: fix even if not visible. Candle restart-partial bars: the 1m bar and the 5m..1d rollup bars open at a server restart lose their pre-restart part (only 100 ms and 1 s are restored); a GUI keeps the partial closed bar. Owner to confirm it matches the 4h/1d report.
- `HeatmapGpuBinner` precision self-test intermittently reads back all zeros (1 of 6 starts) and silently drops the fast kernel; suspect readback before dispatch completion in `driveSelfTest`. Seen again 2026-11-05 22:46 (384/384 zeros, Metal M4). Seen again 2026-11-07 on Metal M4 in a reviewer ctest run (fast kernel failed self-test, precise kernel used).
- Straight RGB into a premultiplied material (as the VP had): `CandlestickBatched.cpp` volume bars draw brighter than their alpha; `CandlestickOverlayItem.cpp` body opacity and wick alpha. Fixing changes looks (owner).
- PEPE: no last-price label on the price axis after switching to it (owner 2026-11-08, minor).
- Symbol switch bins a frame with the old view/tick: "native rows exceed int32" for ~35-400 ms on BTC<->PEPE (`HeatmapGpuBinner.cpp:573`; evidence `/Volumes/T7/sentinel-datalab/pepe-units/int32-diag-gui.log`). Fix: view, tick and spans agree on one symbol.
- Windows: Paper Trading looks for the backtest binary without the multi-config `Release/`/`Debug/` dir or `.exe` (`PaperTradingDock.cpp`).
- GUI RSS ~1.5 GB after 10 min (unprofiled).
- SentinelBar `PlanLocation.resolve` misses `initiatives/` trailers and relative pinned paths outside `research/` (PR 44 review, minor).
- W2d live GUI display and the Windows runtime are unverified.
- Unverified since 2026-09-28 (owner's first look at recording mode): candles overlap the price axis when panned next to it (clip to the plot area); price-axis tick steps disagree with heatmap cell size; confirm the theme dropdown applies to recording mode.
- Nightly backup of `/Volumes/T7/sentinel-data` (to the MacBook Pro over Tailscale needs Remote Login + an SSH key, or to the owner's new HDD); internal disk space.
- Liq slider range carries over on symbol switch (ETH went very high).

## Feature ideas (unverified against current code since 2026-03; check before starting)

- **Heatmap:** blending mode toggle (additive / max / overwrite); per-side bid/ask hue and saturation; value normalization modes (global, visible range, session); custom bid/ask colour pickers; magnifier.
- **Heatmap labels:** cache label quads per column; rebuild only when column, viewport or thresholds change.
- **Candles over heatmap:** body opacity control when the heatmap is visible; wick-only mode; candle aggregation synced with heatmap resolution.
- **Footprint:** bid/ask vs delta modes; cluster vs profile styles; rotation-aware width scaling; GPU-batched per-cell numbers.
- **Order book profile overlay** beside the heatmap, with normalization modes and labels.
- **Trade and liquidation bubbles** (size = volume; liquidations styled apart; toggles). Bubbles may never draw when the overlay exists before any trades (suspected `setVertexCount(0)` at construction, `TradeBubbleNode.cpp:47`; ChartGoldenTests scene 10 works around it).
- **Text:** LOD (numbers -> hints -> none); colour-coded numeric overlays.
- **TPO:** stable letter assignment per timeframe/session slice; column alignment with the shared time/price mapping; 24H vs 1W end to end; regression checks for session and timeframe switching.
- **Interaction:** consistent right-click menus; unified settings panel; keyboard shortcuts for major toggles.
- **History:** footprint and TPO history persistence and overview rollups (the heatmap has HMC2; these two do not).
- **Candles:** roll 1m up to custom timeframes on demand (may fold into Queue 1).
- **Lab:** pure candle viewer (`CandlestickBatched` on live data).
- **Paper trading:** TP/SL visual and interaction polish. A TP/SL exit order stays New when a bad TP or slippage config gives a non-positive exit price (`TradingEngine.cpp:193`): validate before the upsert.
- **Look vs TapeSurf:** volume bars at the bottom, live depth edge on the right, high/low price tags, relative time axis.
- **Code safety:** `DockablePanel` init-order guard (FM-029); raw `new` audit in `libs/gui/render/`; strong typedefs for coordinate systems; protocol round-trip and DataProcessor validation tests.
- Learning session: how HFT-grade terminals structure data paths and rendering, applied to Sentinel.
