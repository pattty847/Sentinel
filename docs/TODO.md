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
2. **Order book widget pass** (owner reports 2026-10-06): following cannot keep the mid centred in a fast dump (`OrderBookDock.cpp` follow/recenter; DECISIONS 2026-10-03, 67 ms over a 2001-tick window); BTC rows under the best bid looked empty at 0.1 tick (thin book or display bug, unverified).
3. **GUI polish** (owner, 2026-10-06):
   - heatmap chunk loading after a timeframe change (1m->15m) or fast zoom-out is too slow for a local server (S6 A/B: 1h settle 509-1297 ms); profile the chunk request path before guessing;
   - put explanatory hover tooltips behind a "learning mode" setting (an order book tooltip covers the chart);
   - TPO history depth: diagnosed 2026-10-23, not a refusal: the GUI asks for `tpoSessions` (default 5, clamp 1..8 at `TpoOverlayRenderer.cpp:95`) h24 sessions, so it showed back to 10-03 on 10-08. Owner decides: deeper history (raise the clamp and `kMaxHistoryPages`), and whether REST-candle columns before the 24 h trade tape are labelled approximate (INV-039 target) or INV-030/039 are amended. The 250k-trade page failure is fixed on main (`46edf77c`, not deployed); open with it: candle-derived TPO letters carry no approximate marker (INV-039).
   - inline volume bars on the market chart (only the Stock Chart dock's `CandlestickBatched` has them);
   - heatmap and candles drift apart slightly on wheel zoom-out; both mappings have the same slopes, cause unknown; the owner's note on direction decides the next look (FM-001, INV-004);
   - local-time axis labels: `TimeAxisModel` has a `timezone` property no control sets; `HeatmapGpuLayer.cpp` hard-codes UTC; add a setting and route every time label through one timezone;
   - zoom-to-footprint: candles widen into bid/ask footprint cells, then numbers appear (extends Footprint and text LOD below);
   - **Trade bubbles rethink** (owner 2026-10-25, discussion pending; screenshots in the 2026-10-25 conductor chat): at 1m zoomed out the bubbles bury the chart and say little; zoomed in they sit at trade time inside the candle, which the owner expected at the candle's centre. Topics: x placement (trade time like Bookmap vs candle centre), size/magnitude scaling, separating price-moving or large trades from passive flow (minimum-size/volume filter, aggregation per time slot and price, sweep detection). Bookmap's bubbles are the floor, not the target. Owner 2026-10-27: after a GUI restart bubbles appear only from the start of the session (no history).
   - **Heatmap stops after sub-minute switches** (owner 2026-10-25, to debug): panning/zooming at 1 s, then 1m -> 1s -> 100ms, left the heatmap empty from the switch time on; back on 1m, time after the switch shows the hatched not-built background. Log `~/Library/Logs/Sentinel/sentinel-gui-20261024-235229-50461.log` 18:10-18:14 EDT: `Heatmap CPU ceiling refused visible spans ... tf=3600000 spans=7 bytes=1189108208` at 18:10:35 (the same ceiling refused spans twice overnight), live subscribe `sub=5206` at 18:12:56 on 1m, no live subscribe at 1 s/100 ms. Known context: live heatmap columns are requested only for tf >= 1 minute (`HeatmapSourceController.cpp:882`, order book plan CE-3). Check whether the 1m live resubscribe resumes and whether the CPU ceiling (about 1 GB committed after a long session) is what leaves spans unbuilt.
4. **Heatmap/candle zoom drift**: heatmap and candles drift apart slightly on wheel zoom-out (moved from the landed render performance pass, `3a54b829`; see item 3).
5. **Repeatable performance benchmark** on the owner's machines; cite its numbers in the README (the old 110 FPS figure was removed 2026-10-07).
6. **README + ARCHITECTURE pass** for the finished one-world architecture; also fix `docs/MARKETDATA.md` (TLS described as optional, single-quad heatmap, 110 FPS).
7. **CI** for macOS, Windows, Linux (no `.github/workflows` today). Hosted runners have no GPU, so GPU tests skip and report Passed; GPU coverage needs self-hosted runners on the Mac and the Windows machine.

## Security (owner decides when)

- Client falls back to `verify_none` with a warning when `ca_file` is missing (`SentinelStreamClient.cpp:175-181`).
- On a screener request the deployed server runs `screener/screener_fetch.py` from the repo checkout (`SentinelStreamServer.cpp:1142-1154`): agent-writable code with the owner's privileges.
- GUI Agent API (17100) has no authentication; client auth does not exist.
- The ntfy topic is in plain text in the grafana plist.

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

- Show only what is real (owner 2026-10-28, soon; one pass): (a) Live candle close lags trades: trade bubbles print below the close and the close reaches them later. Server gate `should_emit_update` (`libs/core/protocol/SentinelStreamServer.cpp:1297`, `candles:` in `config/server_config.yaml`) sends a 1 m+ update only on a new high/low or a close move >= max(2 ticks, 2 bps, about $20 on BTC); otherwise the 1000 ms silence rule, checked only on the next trade, so a quiet stretch's last close can stay stale. Owner direction: a close change of one native tick is always sent; trades that do not move price only add volume and ride the next coalesced update (a short timer, so it feels like riding price without no-op spam). Server change: deploy needs the owner OK. (b) Revisit the TWAP heatmap colouring (owner: also shows what is not real).
- Slow-timeframe history fill (owner 2026-10-28, investigate): on a slow tf a small band loads first and history fills in around it, as if fetched twice or filled from two places. Check for a duplicate or split history request before optimizing.
- Now-column flicker (owner 2026-10-28): the live column flickers very faintly; suspect the MSDF label update. Confirm on a live run before fixing.
- Candle restart-partial bars: the 1m bar and the 5m..1d rollup bars open at a server restart lose their pre-restart part (only 100 ms and 1 s are restored); a GUI keeps the partial closed bar. Owner to confirm it matches the 4h/1d report.
- Startup candle flicker: the auto tick commits 100 -> 500 -> 100 within ~60 ms around the first snapshot, so the settled tick differs run to run; do not commit an auto tick until the first snapshot and price window settle (owner-visible). Until fixed, do not chase price-row diffs in screenshots.
- Overlay teardown crash: `CandlestickOverlayItem`/`TradeBubbleOverlayItem` keep a dangling mapping provider/renderer after `UnifiedGridRenderer` is deleted (tests null them first); use a QPointer or disconnect on UGR destruction.
- `HeatmapGpuBinner` precision self-test intermittently reads back all zeros (1 of 6 starts) and silently drops the fast kernel; suspect readback before dispatch completion in `driveSelfTest`.
- Roller reconnect catch-up may retry forever if the already-applied cursor block is corrupt (LiveSource requires the first reopened record to equal the saved cursor; a recovering JournalReader can return a successor with gapBefore). Unverified.
- An anchor-resumed JournalFeed can re-emit a pre-anchor trade_id on a reconnect snapshot (the v1 anchor sidecar omits the trade-ID window); the fix changes on-disk state and needs owner approval.
- doc-health: bare `file.cpp:123` pointers (no directory) report as dead although the file exists (`MainWindowGpu.cpp:973`, `TopToolbar.cpp:90`); resolve by basename via `git ls-files`. Warning only.
- `config/client_config.yaml` top-level `server:` is ignored (only `client.server` is read); nearly pointed an agent GUI at :8080.
- Straight RGB into a premultiplied material (as the VP had): `CandlestickBatched.cpp` volume bars draw brighter than their alpha; `CandlestickOverlayItem.cpp` body opacity and wick alpha. Fixing changes looks (owner).
- PEPE at its native 1e-8 tick draws ~200 px rows and logs "native rows exceed int32". Direction: readable price units for tiny prices (as exchanges list 1000PEPE), a minimum visible price span or row-height cap in auto-scale; no fake sub-tick grid.
- Windows: Paper Trading looks for the backtest binary without the multi-config `Release/`/`Debug/` dir or `.exe` (`PaperTradingDock.cpp`).
- Tick-size combo text clips at its fixed 84 px width ("$0.15 (unavailable)").
- Capture fan-out `poll()` error loop has no sleep (`CaptureFanout.cpp:418`, low).
- GUI RSS ~1.5 GB after 10 min (unprofiled).
- FPS readout shows ~20 when idle (render on demand) and 110-125 while panning; show frame time or "idle" instead.
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
