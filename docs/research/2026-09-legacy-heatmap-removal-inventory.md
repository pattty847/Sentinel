# Legacy heatmap removal inventory

Branch: `lt-astra/remove-legacy-heatmap`. Status: phase 1 implemented; phase 2 remains a follow-up.

## Phase 1 checklist

- [x] Extract footprint/TPO/VP builder from legacy heatmap publication.
- [x] Independent bounded session timer, fair live/history scheduling, two-worker
  admission, one in-flight job per session, bounded tape/output, stale-result checks.
- [x] Independent `trade_overlays` defaults and optional explicit request grid.
- [x] Keep retained trade aggregation; no scans on network/recording callbacks.
- [x] Carry immutable footprint/TPO grids; map all three overlays against the
  common viewport, independently of heatmap bands and dimensions.
- [x] Preserve neutral footprint time gaps and reset only on its own grid changes.
- [x] Tests for cadence, half-open bucket boundaries, actual client wire parsing,
  grid reset/gaps, recording-independent mapping and bounded worker shutdown.
- [x] Review fixes: budget only requested trades; cap TPO at session close;
  restore worker-side minute-candle history before the retained tape.
- [x] Regressions for an over-cap retained deque with a small requested window,
  closed-session history/live, partial-minute candle coverage and restart history.
- [x] Second review: finite total REST deadline, bounded DNS isolation, and
  overlay page cancellation on shutdown/selection/unsubscribe; local TLS stall
  tests and temporary working directory for the trade-window model fixture.
- [x] Third review: join candle-history and screener jobs before owner teardown;
  bound/cancel screener subprocesses; cache/share DNS lookups with capacity waits;
  test stalled-fetch destruction, resolver recovery, and deadline-test stability.
- [ ] Orchestrator live continuity checks below.

The original blocker was that `on_heatmap_slice` also produced all three live
trade overlays. That coupling and the footprint-history HMCL grid lookup are now
removed. TPO retains REST minute-candle high/low history before the retained tape,
fetched on the worker in bounded pages. This phase does not implement persisted
trade history.

The new publisher retains a fixed per-subscription trade grid, anchored to the
latest trade (or candle close for restart history) unless an explicit grid was
requested. It rejects oversized requested trade windows
rather than truncating a profile. See MARKETDATA.md and CONFIG.md for bounds and
request semantics. Source references below describe the original inventory;
line numbers moved during extraction.

## Consumer dispositions

| Consumer | Evidence | Required disposition |
|---|---|---|
| Footprint live | SentinelStreamServer.cpp:1805; DataProcessor.cpp:322 | Phase 1 done: independent publisher and grid; trade quantities retained. |
| Footprint history | streamFootprintHistory:762 calls resolveTpoGridAndRange:409, which reads HMCL/RAM history at :431 | Phase 1 done: no legacy grid lookup. |
| TPO live | SentinelStreamServer.cpp:1837 | Phase 1 done: independent publisher, session/letter/trade calculation retained. |
| TPO history | SentinelStreamServer.cpp:843 | Phase 1 done: own grid, retained trades and restored REST minute-candle ranges before tape coverage; bounded worker fetches. |
| Volume profile live | SentinelStreamServer.cpp:1886 | Phase 1 done: independent publisher; session aggregation and POC/value area retained. |
| Liquidity labels | HeatmapLabelRenderer.cpp; UnifiedGridRenderer.Render.cpp | Keep recording absolute quantity, validity, and sensitivity-aware coloring; remove only normalized-intensity/grouping branches. |
| Liquidity threshold | UnifiedGridRenderer.Render.cpp:146; HeatmapStreamService.cpp:420; MainWindowGpu.cpp:302 | Applies to recording uploads as well; keep unless separately retiring this user feature. |
| Recording display tick | UnifiedGridRenderer.cpp:823; DataProcessor.cpp:667,698; RecordingBandPolicy.hpp:31 | Keep target_row_px/minRowPx behavior. It is not exclusively legacy. Remove cell_aspect and client shader row grouping. |
| Candles/gating | ServerDataModel.cpp:109,121; SentinelStreamServer.cpp:1933; ServerCandleGateConfig | Separate trade-driven aggregator/timer and candle wire updates; retain candle gates and configured timeframes. No direct legacy-slice dependency found. |
| Agent API heatmap freshness | MainWindowGpu.cpp:1068,1280 | Switch lastReceivedAtMs.heatmap from legacy slice reception to accepted recording live reception. |
| Agent API book/trades | MainWindowGpu.cpp:1088,1388 | Keep independent L2 book and trade streams; heatmap deletion must not remove them. |
| Agent API state capability fields | AgentApiCodec.cpp:368 | Keep wire-presence/null semantics; update served timeframe/grid reporting to recording truth. |
| Agent API walls | GuiApiServer.cpp:249; HeatmapColumnWindow.cpp captureWalls | Keep recording source-column scan, validity, ranking and bounds; remove only legacy 409 branch. |
| CopeNet | CopenetFeedDock.cpp | In-tree dock is a text feed, no direct legacy data dependency found. External Agent API consumers can observe changed freshness/capabilities; no external repository was audited. |
| HMC2 CRC | Hmc2Store.cpp:2,119,144,554,912,1232 | Extract shared CRC32 before deleting HmcolFormat. Recording persistence directly uses hmcol::crc32. |
| HMC2 legacy-schema fixtures | Hmc2LegacyFixture.hpp; test_hmc2_store.cpp | Preserve recording-format fixture coverage. These are HMC2 schemas, not HMCL heatmap-stream tests. |

## Phase 2 removal checklist (not started)

- Server implementation: HeatmapTwapStreamer.{hpp,cpp},
  HeatmapColumnStore.{hpp,cpp}, IHeatmapDataSource.hpp after severing remaining
  inheritance, HMCL parts of HmcolFormat.{hpp,cpp} after extracting CRC32.
- Server integration: ServerDataModel streamer construction/start/bootstrap,
  history/floor methods and signals; SentinelStreamServer legacy history builder,
  source dispatch, live heatmap signal connection and heatmap_slice encoder.
  The three live overlay publications have already been split out in phase 1.
- Wire/client: HeatmapSlice.hpp; HeatmapSlice enum/string routing and schema
  constants; SentinelStreamClient legacy history requests, live/history parsers,
  signals and metatypes; IGridDataSource and RemoteGridDataSource legacy DTOs,
  forwarding, requests; MainWindowGpu legacy connections/history request routing.
  Recording still uses heatmap_history_request/chunk names: do not remove those
  message families wholesale. Keep RecordingHistoryWire request correlation,
  bounded worker handling, live view and live messages.
- Client processing: DataProcessor legacy live/history handlers and fetch state;
  HeatmapColumnWindow legacy ingestion/resampling/rollup branches;
  HeatmapStreamState default LegacyIntensity metadata; HeatmapStreamService legacy
  render-tick and mode branches. Preserve recording cache, window slots, staged
  re-bands, validity, repair, retry and disconnect behavior.
- Renderer: LegacyIntensity enum/value-mode plumbing, params3 legacy shader
  branch, HeatmapRowGrouping.hpp, renderer grouping/phase computations and legacy
  label color decoding. Preserve shared heatmap texture classes used by footprint.
- Config: heatmap.source; cell_aspect; legacy persistence_enabled/dir/fsync/
  retention; legacy normalization intensity_mode/max_mode/max_decay/log_scale/
  power/floor; recenter_delta and band_fast/medium/slow once overlay grid ownership
  is resolved; debug_slice_log. Keep ring dimensions and shared timeframes,
  recording configuration, target_row_px and recording sensitivity. tick_size and
  active timeframe are currently shared with trade-overlay grid/cadence and must
  be resolved during extraction. Remove matching ConfigLoader/wire parsing/docs.
- Deleted-code tests/tools: test_heatmap_column_store.cpp,
  test_heatmap_twap_streamer.cpp, HeatmapHistoryFixture.hpp,
  test_heatmap_history_fixture.cpp, hmcol_seed.cpp, HMCL-format-specific tests,
  test_HeatmapRowGrouping.cpp, scripts/inspect_hmcol.py and CMake entries.
- Mixed tests: retain recording sections of test_HeatmapColumnWindow.cpp and
  remove/replace legacy-only fixtures; split WallsRejectLegacyAndExcessiveScans
  so bounded-scan coverage remains. Update protocol validation/parser fixtures.
- Recording suites to retain: recording codec/page/live/server-stop/wire,
  HMC2 store/recovery, book recorder, RecordingBandPolicy, RecordingDataProcessor,
  recording window/walls tests and applicable Agent API tests. Add tests for
  three overlay streams surviving legacy removal, freshness from recording, no
  legacy wire publication, recording-only startup, and CRC extraction equivalence.
- Canonical docs: MARKETDATA.md, CONFIG.md, ARCHITECTURE.md, AGENT_API.md,
  SENTINEL_STREAM_CLIENT.md. Research/initiative documents contain historical
  legacy references; mark their assumptions superseded rather than describing
  them as current behavior. Retire shared memory invariants INV-041/042/043/050
  only when their implementation is actually removed.

## Validation and live handoff

Phase 1 changes server/client source, overlay configuration, canonical docs and
tests. It deletes no source files and touches no recorded data. Phase 2 has not
started: legacy heatmap production, HMCL persistence, wire ingestion and source
switch still exist. CRC extraction and the other removal items above remain open.

Targeted TradeOverlayTests and transport bounds tests cover phase 1; full build/ctest results belong
in the handoff report. Live/visual checks remain unverified in this worktree.

Once implemented: rebase onto main, full optimized mac-clang build, then ctest
from build/mac-clang, with exact summaries in the READY report. Never merge own
branch. The orchestrator must run server and GUI with
SENTINEL_PROBES=zoom,heatmap.recording; inspect both run-log headers for the new
binary, startup/error logs, first recording paint, zoom/re-band stability, ongoing
live columns and walls ordering/validity. Also toggle footprint, TPO and volume
profile together and confirm each continues updating and mapping correctly after
zoom/timeframe changes. No live/visual check was performed for this inventory.
