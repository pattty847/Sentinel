# S8: delete the legacy heatmap (plan, 2026-10-06)

Read-only packet by Claude Fable, verified against main @ d64aa49. The conductor accepted it with the decisions in
section 8. The owner approved S8 on 2026-10-06.

## 0. Findings that change the plan

1. **FM-079 extraction is done.** `TradeOverlayPublisher` exists (`libs/core/servermodel/TradeOverlayPublisher.cpp`). `SentinelStreamServer::on_heatmap_slice` (`SentinelStreamServer.cpp:1528-1560`) emits only the `heatmap_slice` wire, and the server has its own `trade_overlays` config block. Phase 2 of `docs/research/2026-09-legacy-heatmap-removal-inventory.md` (server production) has not started: `HeatmapTwapStreamer` is constructed unconditionally (`ServerDataModel.cpp:103,130`) and persists HMCL to `data/heatmap`.
2. **"Legacy" means three layers:**
   - (a) The GUI page renderer (`heatmap.renderer: legacy`).
   - (b) The server recording page path: `heatmap_recording_view/unview`, `heatmap_history_request source=recording`, `LiveService::subscribe` + `LiveBuilder`, `recording::buildPage`.
   - (c) The TWAP/HMCL stream: `heatmap_slice`, `heatmap_history_chunk`, `HeatmapTwapStreamer`, `HeatmapColumnStore`, `HmcolFormat`, and the client `heatmap.source: legacy`.

   The GPU renderer uses none of them; it uses `heatmap_live_subscribe` (raw tail) and `heatmap_chunk_request`.
3. **S8 must stay in GUI and client files during the R1 soak.** The server items (layers b and c) live in `SentinelStreamServer.cpp`, `ServerDataModel.*` and `RecordingLive.*`, which D-b owns next. One-world section 9 places legacy phase 2 with the deletion slice (E). The work is split into S8a (GUI and client, now) and S8b (server, after R2, before or with slice E).
4. **`recording::buildPage` and `LiveBuilder` are the GPU path's parity oracle** in five test suites. `RecordingPage.{hpp,cpp}` stays in S8a; S8b chooses between goldens and a test-support target.
5. **The GPU walls types (`heatmap_window::{WallQuery, Wall, WallError, WallsSnapshot}`) live in `libs/gui/render/HeatmapColumnWindow.hpp:81-104`.** They are used by `HeatmapCellQuery`, `HeatmapGpuLayer`, `GuiApiServer` and `AgentApiTypes`. Move them to `libs/gui/render/heatmap/HeatmapWalls.hpp` before deleting the file.
6. **GPU mode still reads the legacy stream service** for its frame clock and `TimeAuthority` (`UnifiedGridRenderer.Render.cpp:582-600,646-654`; `FrameContext.hpp:46`). Move these first.
7. **Name trap.** `m_useGpuHeatmap` (`UnifiedGridRenderer.h:157`) is the legacy texture flag. `m_gpuHeatmap` (`:160`) is the S6 renderer. Do not pattern-delete.
8. **Shared code that stays:**
   - `HeatmapIntensityNode`, `HeatmapColumnTexture`, `heatmap_intensity.*` and `HeatmapRowGrouping.hpp` (used by footprint).
   - `UgrFrameMath` (only its ring rects go).
   - `idealTick` (used by the lab bench).

   `HeatmapLabelRenderer` has only the legacy caller.
9. **`scripts/dev/recording-history-probe.py` speaks the page wire.** It goes in S8b.
10. **The client `heatmap.source` key is read by nothing in gpu mode.** The key and its assertion go.
11. **The S8 GO verdict exists only in STATUS history** (`f92bf84`).
12. **The locked branch `lt-claude/heatmap-ab-isolation` (`88e7ef4`) isolates `heatmap-ab.sh` with `--agent-host`.** See decision 3.

## 1. Delete and keep

### S8a: GUI and client (this slice)

**Whole files:**
- `libs/gui/render/HeatmapColumnWindow.{hpp,cpp}`, after finding 5.
- `HeatmapStreamState.{hpp,cpp}` and `HeatmapStreamService.{hpp,cpp}`, after finding 6.
- `HeatmapOverlayRenderer.{hpp,cpp}`.
- `RecordingBandPolicy.hpp`. Move `recording_view::kFinestNativeTick` and `kRows` first; they have GPU callers at `UnifiedGridRenderer.cpp:173-176,1120`.
- `HeatmapLabelRenderer.{hpp,cpp}`.
- The matching entries in `libs/gui/CMakeLists.txt`.

**`UnifiedGridRenderer.{h,cpp,Render.cpp,Init.cpp}`:**
- `ensureHeatmapRootNode`, `computeAndApplyFrameMapping`, and `updateLabelGeometry` with the label rings.
- The legacy half of `updatePaintNode` (`Render.cpp:656-724`).
- `startHeatmapRenderLoop`, `m_useGpuHeatmap`, `applyHeatmapRangeReset`, `heatmapHistoryNeeded`, `m_heatmapOverlay`, and the history request state.
- The `Init.cpp:138-205` connections.
- The legacy branch of `setHeatmapRenderer`, then the method itself.
- The `heatmapTickSize` fallback and the `setRecordingConfig` call.
- `gpuHeatmapActive()` and its guards.

**`render/DataProcessor.{hpp,cpp}`:**
- Delete:
  - every heatmap slice, history and recording slot, plus the window, fetch and band helpers;
  - `captureHeatmapWalls`;
  - the matching signals;
  - every `m_recording*` member.
- Keep:
  - the footprint, TPO and volume profile slots and signals;
  - `setActiveSymbol`, `clearData` and the timeframe helpers.
- Check: `setHeatmapGridDimensions/Height/IntensityScale` against the footprint grid fallbacks before removing them.

**`IGridDataSource.hpp` and `RemoteGridDataSource.{hpp,cpp}`:** the heatmap history and recording-view requests, `HeatmapHistoryColumn`, and the slice, history and recording signals.

**`libs/core/protocol/SentinelStreamClient.{hpp,cpp}` (client side only):**
- Delete the recording-view, recording-history and heatmap-history requests and their parsers and signals.
- Keep the `MessageType` entries until S8b.
- The server keeps sending `heatmap_slice` until S8b, so unknown legacy frames must be dropped without logging a warning per message.

**`MainWindowGpu.cpp`:** the renderer override, the legacy walls branch, the legacy connections, the legacy `heatmapReceivedAtMs`, and `activeRenderer`.

**Settings and config:**
- The renderer combo in `HeatmapSettingsDialog`, plus `HeatmapSettingsModel::setProcessRenderer`, `HeatmapSettingsStore`, `HeatmapChartSettings::renderer` and `HeatmapChartControls` `mode.gpu`.
- `GuiConfigStore`.
- `apps/sentinel_gui/main.cpp` `--heatmap-renderer`.
- `ClientConfig.heatmap.renderer/.source` and `config/client_config.yaml:6-7`.
- The `GuiApiServer` `recording_required` path and `AgentApiCodec.cpp:643`.

**Tooling:**
- `scripts/dev/gui-shot.sh` `--renderer`, `scripts/dev/gui-host.py`, and `scripts/dev/test_gui_host.py`.
- Retire `scripts/dev/heatmap-ab.sh` after the final capture.
- `docs/AGENT_WORKFLOW.md:33`.

**Docs:** the legacy sentences in `CONFIG.md`, `AGENT_API.md` and `ARCHITECTURE.md:83`.

**Agent API:** `renderer`, `activeRenderer` and `savedRenderer` stay as the constant `"gpu"` in S8a and go in S8b. `fit_unavailable` and `auto_scale_unavailable` keep only their "nothing known yet" meaning.

### S8b: server (after R2, before or with slice E)

**`SentinelStreamServer.cpp`:** `heatmapConn_`, the recording view and history handlers, `on_heatmap_slice`, `buildHeatmapHistoryChunk`, the recording view, gate and slot state, and the history worker.

**`ServerDataModel.*`:** `m_heatmapStreamer`, `heatmapSliceReady`, `getHeatmapHistory` and `oldestHeatmapPersistedMs`.

**Files:**
- `HeatmapTwapStreamer.*`, `HeatmapColumnStore.*` and `IHeatmapDataSource.hpp`.
- The HMCL parts of `HmcolFormat.*`. Extract CRC32 first; it is used by `Hmc2Store.cpp` and `RawCapture.cpp`.

**`RecordingLive.*`:**
- Delete: `LiveService::subscribe`, `Subscription`, `LiveBuilder` and `LiveView`.
- Keep: `LiveCache`, `RawTailBuilder`, `subscribeRaw`, `LiveCadence`, `LiveWriteSlot/Budget` and `LiveRegistrationGate`.

**Wire and config:**
- `RecordingHistoryWire.hpp`: everything except `capability`.
- `MessageType`: `HeatmapSlice`, `HeatmapHistoryRequest`, `HeatmapRecordingLive` and `HeatmapHistoryChunk`.
- The server `heatmap.*` config.

**Tests:**
- `HeatmapTwapStreamerTests`, `HeatmapColumnStoreTests` and the HMCL parts of `HmcolFormatTests`.
- `HeatmapHistoryFixtureTests`, `RecordingHistoryWireTests` and `ProtocolValidation.HeatmapFamilySchemaGating`.
- `ServerFeedAdmissionTest.ReleasedGuiFeedStopsLegacyColumnStore`.
- The `LiveBuilder` parts of `RecordingLiveTests`, and `recording_live_bench`.

**Scripts:** `scripts/inspect_hmcol.py` and `scripts/dev/recording-history-probe.py`.

**`_agent`:** retire INV-041, INV-042, INV-043 and INV-050.

**Oracle:** the `RecordingPage` decision (finding 4).

**Docs:** `MARKETDATA.md:173-279,317` and `ARCHITECTURE.md:317`.

### Keep for one-world

- `LiveService`'s raw path and `LiveCache` (the roller publishes into them).
- `ChunkWire.hpp`, `RecordingLoader`, `Hmc2Store/Reader` and `BookRecorder`.
- `TradeOverlayPublisher`.
- The engine slots that D-b replaces.

## 2. Split

### S8a

- **Who:** now, one writer, Claude `opus` (needs Metal). About 4,500 lines removed.
- **Hot files:** `UnifiedGridRenderer.cpp`, `DataProcessor.cpp` and `MainWindowGpu.cpp`.
- **Not touched:** `SentinelStreamServer.cpp`, `HeatmapTwapStreamer.cpp`, `ServerDataModel.*`, `RecordingLive.*` and `libs/core/roller`.
- **Order:**
  1. The A/B capture.
  2. Move the walls types, the frame clock and `TimeAuthority`, and the two `recording_view` constants.
  3. Delete.
  4. Tests and docs.

### S8b

- **When and who:** after R2, before or with slice E. Owned by whoever then owns the server files.
- **Risk:** high (recorder). Reviewer: Fable if Codex writes it, `gpt-6-astra` if Claude writes it.

## 3. Acceptance checks for S8a (each must fail without its change)

1. `POST /api/v1/heatmap/settings {"renderer":"legacy"}` returns 422; today it returns 200. Covered by `HeatmapPlumbingTests` (`test_heatmap_plumbing.cpp:127-243`).
2. `sentinel-gui --heatmap-renderer legacy` logs one warning and runs gpu. `gui-host.py POST /launch {"renderer":"legacy"}` returns 400, tested in `scripts/dev/test_gui_host.py`; state the command, because it does not run under ctest.
3. Trade overlays stay live (the client side of FM-079). In an isolated GUI with footprint, TPO and volume profile on, `GET /api/v1/state` `lastReceivedAtMs.footprint/tpo/volumeProfile` advance over 3 min. Take a screenshot (`heatmap`) with all three layers.
4. `TradeOverlayTests` (`test_TradeOverlay.cpp:141`): drop `setRecordingConfig`/`setHeatmapViewport` and keep the grid-generation assertions.
5. `UgrGpu.RendererFlipsBothWays` is replaced by a test that the chart has exactly one root path. Update `test_MainWindowSymbolLifecycle.cpp:266`.
6. `GET /api/v1/heatmap/walls` works after the type move, and `rg HeatmapColumnWindow.hpp libs` finds 0 hits. `HeatmapCellQueryTests` pass.
7. The legacy rows in `ChartUiTests` are removed; the labels toggle is always shown.
8. The legacy cases in `HeatmapSettingsUiTests` are rewritten; the Debug tab has no renderer combo.
9. Legacy frames from the still-running server produce no `W` lines from the client parser in the GUI run log.
10. The guarded set in `AgentHostModeTests` (`test_AgentHostMode.cpp:248`) is updated.
11. `RecordingDataProcessorTests`, `HeatmapColumnWindowTests` and `RecordingBandPolicyTests` are gone from ctest, and the ring cases are removed from `UgrFrameMathTests`.
12. One-world: the D-a legacy-renderer limit is gone, because no client subscribes to a page view.

### Existing tests over S8a's files (FM-201)

**Must pass after S8a:**
- `RecordingDataProcessorTests`, `HeatmapColumnWindowTests`, `RecordingBandPolicyTests`, `HeatmapRowGroupingTests` and `UgrFrameMathTests`.
- `TradeOverlayTests`, `UgrGpuTests`, `UgrGpuServiceTeardownGuardMalloc`, `HeatmapTileNodeTests` and `HeatmapTileNodeTeardownGuardMalloc`.
- `HeatmapSettingsUiTests`, `HeatmapPlumbingTests`, `ChartUiTests`, `HeatmapLabelLayoutTests`, `HeatmapCellQueryTests` and `HeatmapSourceControllerTests`.
- `MainWindowSymbolLifecycleTests`, `AgentHostModeTests`, `AgentApiCodecTests`, `AgentApiDocksTests` and `AgentApiInputTests`.
- `DomTests` and `PaperTradingDockTests` (stub overrides).
- The `CandleDataSource`, `CandleBackfillState`, `CandleSeriesBuffer` and `market_health` tests.
- `MarketDataEngineReconnectTests`.

**Must stay green and unchanged:** `RecordingPageTests`, `RecordingLiveTests`, `RecordingChunkTests`, `HeatmapModelTests` and the book_recorder suite.

## 4. The A/B capture (first, before any deletion)

1. Run the 13 steps of `docs/research/2026-10-s6-plan.md` "Sequence and timings" (`:595-612`) on both renderers on the unmodified S8a branch. Add a 10-minute live-age soak for each renderer. Never use `target=main`.
2. Record under `screenshots/s8-final/<run>/` (gitignored) and write the before table and the after table into section 9 of this file.

## 5. Native evidence (GPU heatmap core, high risk)

- **Metal tests:** `UgrGpuTests`, `HeatmapTileNodeTests` and `HeatmapCellQueryTests`, through the build queue. Read the output for `GPU case skipped`, and check each log's `exe=... built=...` header.
- **Isolated GUI on the running recorder:** `--agent-host`, its own `--api-port`, `--no-screener`.
  - Steps: BTC-USD 1m, 5m and 1h; wheel ×5; price-axis drag; Manual $1; follow-live; symbol switch to ETH-USD and back.
  - `heatmap/state` must show `settled` true and `liveAgeP95Ms` within the S6d range.
  - Screenshots: `heatmap`, `window`, `settings:Debug` and `toolbar`.
- **Reviewer:** `gpt-6-astra` at high, with the A/B record and the screenshots.

## 6. Risks (ranked)

1. Deleting `HeatmapColumnWindow.hpp` or `HeatmapStreamService` before the moves breaks GPU walls and every frame.
2. Client overlay regressions: the footprint, TPO and volume profile slots share `DataProcessor` with the deleted paths.
3. Pattern-deleting `m_gpuHeatmap` together with `m_useGpuHeatmap`.
4. Removing `RecordingPage.cpp` turns the oracle tests red.
5. Legacy frames keep arriving until S8b; a parser that warns on each one floods the run log.
6. An A/B run without isolation could touch the owner's settings.
7. Agent API consumers outside the repo may read the renderer fields; keeping them constant in S8a limits this.
8. S8b stops the recorder writing `data/heatmap`, so its deploy needs the owner present.

## 7. Owner question (S8b only, not now)

When `HeatmapTwapStreamer` goes, the recorder stops writing `data/heatmap` (HMCL). The default is to keep the existing directory untouched; the alternative is to schedule its deletion with the owner present.

## 8. Conductor decisions (2026-10-06)

1. The S8a/S8b split is accepted. S8a runs now, during the R1 soak. S8b goes with slice E, after R2.
2. Defaults are accepted:
   - The Agent API renderer fields stay as the constant `"gpu"` in S8a.
   - `RecordingPage` stays as the test oracle until S8b.
   - `heatmap-ab.sh` retires after the final capture.
3. The A/B capture uses the GUI-host fallback: one hosted session per renderer, run one after the other. Each launches with `gui-shot.sh launch --renderer legacy|gpu --build <worktree>`, which is isolated by design. The `lt-claude/heatmap-ab-isolation` branch was not landed for this; the owner retired it on 2026-10-06 (tag `archive/lt-claude-heatmap-ab-isolation`).
4. `lt-sol/label-style` (locked) changes `HeatmapLabelRenderer` and `HeatmapOverlayRenderer`, both of which S8a deletes. That branch will no longer apply after S8a; the owner retired it on 2026-10-06 (tag `archive/lt-sol-label-style`).

5. (conductor, after the Astra r1 review) Accepted minor, not fixed in S8a: retired legacy frames are still JSON-parsed before the client drops them (`SentinelStreamClient.cpp:685,726,737`; about 20 frames/s at 11 KB, a cost inherited from before S8a). S8b removes the sender, which removes the cost; a pre-parse discriminator would be throwaway code.

## 9. A/B record (S8a writer, 2026-10-06)

Hosted GUI sessions (`gui-shot.sh launch --build <worktree> --fresh-profile`, one at a time, on the
running recorder; screenshots `target=heatmap` only; owner settings unchanged after every session).
Evidence under `screenshots/s8-final/` (gitignored): `before/` (`legacy-*`, `gpu-*`, unmodified branch
@ e258947), `after/` (gpu, branch @ fd9fcd5), `native/` (section 5 run), `repro/` (main vs S8a checks).
Steps: the S6d sequence (`2026-10-s6-plan.md` "Sequence and timings") plus 11b/11c labels; windows 05,
08b, 11, 11b and 11c are fixed absolute windows shared by all runs (`windows.env`). The legacy and
gpu "before" sessions ran 12 minutes apart, so live steps (01-04, 09, 10, 12, 13) show different data.

### Before (unmodified branch)

| Step | legacy op->rendered (ms) | gpu op->rendered (ms) | gpu settled after (ms) | legacy frameP95 | gpu frameP95 | gpu tick (units) | legacy vs gpu (3% fuzz) |
|---|---|---|---|---|---|---|---|
| 01 symbol BTC-USD | 392 | 33 | 16 | 1.1 | 4.6 | 100 | 62.4% (live) |
| 02 tf 1m | 25 | 30 | 29 | 0.8 | 1.0 | 100 | 62.9% (live) |
| 03 tf 5m | 27 | 43 | 22 | 0.8 | 1.1 | 500 | 66.4% (live) |
| 04 tf 1h | 29 | 49 | 1511 | 0.8 | 9.3 | 500 | 60.9% (live) |
| 05 viewport 36 h @1h | 31 | 28 | 27 | 0.5 | 0.8 | 500 | 40.4% (tick: legacy band vs gpu Auto $5) |
| 06 wheel x5 | 21-25 each | 20-24 each | 27 | 0.9 | 0.9 | 500 | **0.11%** |
| 07 price-axis drag | 29 | 29 | 27 | 0.7 | 0.8 | 1000 | 46.9% (tick) |
| 08 Manual $1 (gpu only) | - | 34 | 28 | 0.8 | 0.9 | 100 | n/a |
| 08b Manual $1, 72 h | 25 | 27 | 24 | 0.9 | 0.8 | 100 | n/a |
| 09 follow-live | 30 | 30 | 24 | 0.5 | 1.2 | 100 | live |
| 10 tf 1m (follow) | 37 | 48 | 25 | 0.5 | 0.8 | 100 | live |
| 11 viewport 3 h @1m | 30 | 39 | 22 | 0.6 | 0.7 | 100 | **5.9%** |
| 11b labels USD | 25 | 29 | 22 | 1.3 | 0.5 | 100 | 9.9% |
| 11c labels asset | 55 | 32 | 20 | 1.0 | 0.9 | 100 | 9.9% |
| 12 follow-live 1m | 35 | 25 | 24 | 1.0 | 2.3 | 100 | live |
| 13 after 60 s live | - | - | 36 | 0.8 | 1.5 | 100 | live |

10-minute live soak at 1m follow-live: legacy heatmap receive age p50 33 / p95 56 ms (legacy has no
draw-age metric), frameP95 p50 0.9 / p95 2.0 ms; gpu `liveAgeMs` p50 100 / p95 510 / max 615 ms (layer
cumulative p50 101 / p95 493 over 1297 drawn versions), frameP95 p50 1.6 / p95 2.9 ms. Cold start (host
launch to API, gpu settled): gpu 1354 ms; legacy 616 ms (no settle wait).

### After (S8a, gpu only)

| Step | op->rendered (ms) | settled after (ms) | frameP95 | tick (units) | vs before gpu (3% fuzz) |
|---|---|---|---|---|---|
| 01 symbol BTC-USD | 31 | 15 | 6.7 | 100 | live |
| 02 tf 1m | 28 | 18 | 1.0 | 100 | live |
| 03 tf 5m | 36 | 23 | 1.1 | 500 | live |
| 04 tf 1h | 27 | 21 | 1.0 | 500 | live |
| 05 viewport 36 h @1h | 26 | 22 | 0.5 | 500 | **0.00%** |
| 06 wheel x5 | 20-24 each | 30 | 0.6 | 500 | **0.00%** |
| 07 price-axis drag | 28 | 22 | 0.9 | 1000 | 5.4% (candles only, see below) |
| 08 Manual $1 | 33 | 19 | 0.7 | 100 | **0.00%** |
| 08b Manual $1, 72 h | 27 | 23 | 1.6 | 100 | **0.00%** |
| 09 follow-live | 25 | 22 | 0.8 | 100 | live |
| 10 tf 1m (follow) | 32 | 25 | 0.9 | 100 | live |
| 11 viewport 3 h @1m | 37 | 22 | 0.9 | 100 | **0.00%** |
| 11b labels USD | 27 | 27 | 1.1 | 100 | **0.00%** |
| 11c labels asset | 24 | 23 | 0.7 | 100 | **0.00%** |
| 12 follow-live 1m | 26 | 21 | 1.5 | 100 | live |
| 13 after 60 s live | - | 27 | 1.7 | 100 | live |

10-minute soak: `liveAgeMs` p50 115 / p95 2202 ms, but the high samples are the first 30 s only
(2.2-2.4 s): the Coinbase feed dropped at 21:01:13 (server log: EOF, reconnect, exchange clock offset
10 s, `backward=877`), while the client receive age stayed p95 501 ms. From 35 s on the soak matches
"before" (all samples <= 594 ms). frameP95 p50 1.8 / p95 2.6 ms. Cold start 1421 ms (901 ms in the native
run). Step 07: the heatmap is identical; the candles of the settled shot still showed the first of the
two drag steps (a stale overlay frame). Not reproduced in 6 + 6 two-step drags on main's build and on
S8a (`repro/`, candles off/on refresh shows no difference): a timing race, not an S8a change.

### Native run (section 5, `native/`)

BTC-USD 1m, 5m, 1h, wheel x5, price-axis drag, Manual $1, follow-live, ETH-USD and back: every step
`settled` true, renderer/activeRenderer "gpu"; `liveAgeP95Ms` 620 ms at 1m (2739 ms cumulative after the
1h steps, whose live versions are minutes old by construction). Trade overlays over 3 min with
footprint, TPO and VP on in turn: `lastReceivedAtMs.footprint/tpo/volumeProfile` advanced every 10 s
sample (18/18). `POST heatmap/settings {"renderer":"legacy"}` -> 422 `invalid_settings`. Run log: no W
line from the stream client. A direct `--agent-host` launch with `--heatmap-renderer legacy` logged one
warning, ran gpu, and the `stream.legacyHeatmap` probe counted 546 `heatmap_slice` frames dropped in
27 s (about 20/s, 11 KB each: S8b removes them at the source). Screenshots: `extra-heatmap.png`,
`extra-window.png`, `extra-settings-debug.png` (no renderer combo), `extra-toolbar.png`,
`extra-overlay-*.png`.

Found while collecting this (pre-existing on main, not S8a): the volume-profile layer draws a fan of
triangles from the chart's top-left corner (`repro/main-vp-*.png`, `repro/s8a-vp-*.png`).

### Clean soak re-run (after, a3d008f)

Requested by the conductor because the "after" soak overlapped the 21:01:13 Coinbase disconnect.
Same method (hosted GUI, `--build <worktree> --fresh-profile`, BTC-USD 1m follow-live, 10 minutes of
5 s samples), binary built 21:16:12 from a3d008f, 21:26:17-21:36:20, evidence in
`screenshots/s8-final/soak2/`. No disconnect in the window: the server's feed line stays
`conn=2 ... reconnects=1` (the 21:01 reconnect) throughout, with no EOF or invalidation; the GUI run log
has no W/E line beyond the usual Qt/QML and layout ones.

| Soak (1m, BTC-USD) | liveAgeMs p50 | p95 | max | layer cumulative p50 / p95 | receive age p95 | frameP95 p50 / p95 |
|---|---|---|---|---|---|---|
| Before (unmodified, gpu) | 100 | 510 | 615 | 101 / 493 (1297 versions) | 508 | 1.6 / 2.9 |
| After, clean re-run | 109 | 497 | 630 | 114 / 516 (1148 versions) | 504 | 1.1 / 1.9 |

The deletion leaves live data age unchanged within sampling noise.

6. (2026-10-07, from the owner audit; see docs/research/2026-10-07-audit-triage.md item 9) Before S8b: candles, footprint retention and the GUI toolbar still read server `heatmap.*` config keys. Move each still-used key to a neutral section and list every reader (`rg` each key) before deleting the section.
