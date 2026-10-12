# Sentinel repo sweep: dead code, narration, duplicates, naming, size (read-only audit)

Tree: `/home/claude/pattty847/sentinel` @ `180d3b7`. Nothing edited, built or run. Every claim below comes from grep/read of the tree. "No callers" means a whole-word grep over `libs apps tests` (plus `.qml`, `CMakeLists.txt`, `scripts`, `config` where relevant) finds only the declaration/definition.

Excluded (other auditors): `libs/gui/UnifiedGridRenderer.*`, `libs/gui/render/heatmap/*`, `libs/gui/MainWindowGpu.*`, `TopToolbar`, `HeatmapSettingsDialog`. Where a finding has to edit one of these, I say so and leave the edit to that auditor.

**Collision set** (pending one-world step 4: D-b2b, slice E, S8b): `ServerDataModel.*`, `SentinelStreamServer.*`, `SentinelServerApp.*`, `ConfigLoader.*` (and in practice `ConfigTypes.hpp`, which D-b2b edits for `recording.live_feed`/`mdc`), `ShadowRoller.*`/`ShadowConfig.hpp`, plus `BookRecorder.cpp` (slice E removes `localClock` and the overflow invalidation path from it).

## (b) Removed by the deletion step (not audited)

- `libs/core/servermodel/HeatmapTwapStreamer.*` (1165 lines), `HeatmapColumnStore.*` (831), `IHeatmapDataSource.hpp`, `TickBinaryLogger.*`, the HMCL parts of `HmcolFormat.*`
- Server use of `MarketDataFeeds`/`MarketDataCoreEngine` (`SentinelServerApp.cpp`, `ServerDataModel.cpp`). Capture keeps both: `libs/core/capture/CaptureApp.cpp`.
- The legacy heatmap parts of `SentinelStreamServer.cpp` (`on_heatmap_slice` :1492, `buildHeatmapHistoryChunk` :1907, the recording view/history handlers) and of `RecordingLive.*` (`LiveBuilder`, `LiveView`, `Subscription`)
- The primary BookRecorder path in `ServerDataModel.cpp`, `RecorderStallMonitor.hpp` and `RecordingDir.hpp` (`RecordingDirChoice`)
- Their tests (`test_heatmap_column_store.cpp` 954, `test_book_recorder.cpp` primary cases, `test_recording_live.cpp` legacy parts, `test_heatmap_chunk_wire.cpp` legacy parts), `scripts/inspect_hmcol.py` and `scripts/dev/recording-history-probe.py` (S8 plan section 1)
- `libs/gui/render/RecordingBandPolicy.hpp` (test-only since S8a, its own comment at :2-4; only caller `tests/servermodel/test_recording_page.cpp:2`)
- Already scheduled in the S8 plan (`docs/research/2026-10-s8-legacy-deletion.md:96`): extracting `hmcol::crc32` out of `HmcolFormat` before the file goes. Callers that must survive: `Hmc2Store.cpp:119,144,548,746,906,1323` and `RawCapture.cpp:39`.

---

## 1. Dead code

### 1a. Whole files with no production users

| File(s) | Lines | Evidence |
|---|---|---|
| `libs/gui/qml/controls/ChartToolRail.qml` + `NavigationControls.qml`, `TimeframeSelector.qml`, `PriceResolutionSelector.qml`, `VolumeFilter.qml`, `GridResolutionSelector.qml` | 627 | `ChartToolRail` is used by nothing (grep over all `.qml/.cpp/.h`). The other five are used only inside `ChartToolRail.qml:99-111`. Listed in `libs/gui/CMakeLists.txt:278-283`. |
| `libs/gui/qml/CandleChartView.qml` | 81 | No `qrc:` load and no QML reference (`StockChartDock.cpp:116-119` loads `StockChartView.qml`; `QmlSceneController.cpp:59` loads `DepthChartView.qml`). |
| `libs/gui/qml/Theme.qml` | 12 | No `Theme.` or `Theme {` user. DepthChartView uses the context property `uiTheme`. |
| `libs/gui/render/LabTextItem.{hpp,cpp}` + shim `libs/gui/LabTextItem.hpp` | 297 | It declares `QML_ELEMENT` but is not in the `qt6_add_qml_module` SOURCES (`libs/gui/CMakeLists.txt:259-273`). No QML uses it. Only `tests/render/test_ugr_gpu.cpp:20,4472` and `tests/agentapi/test_MainWindowSymbolLifecycle.cpp:20,820` register it, and nothing then instantiates it. |
| `HeatmapIntensityNode` + `HeatmapIntensityMaterial` (in `libs/gui/render/HeatmapIntensityNode.{hpp,cpp}`), `render/shaders/heatmap_intensity.{vert,frag}`, `libs/gui/render/HeatmapRowGrouping.hpp` | ~390 + 116 + 83 | The legacy heatmap root, dead since S8a. The only include, `FootprintOverlayRenderer.cpp:2`, uses just `HeatmapColumnTexture` (:146). The shaders load only at `HeatmapIntensityNode.cpp:15-16`. `HeatmapRowGrouping` is used only by `HeatmapIntensityNode` (:311) and `tests/render/test_HeatmapRowGrouping.cpp`. **The S8 plan (finding 8, `:21`) says these are "used by footprint"; that is wrong.** Only `HeatmapColumnTexture` (hpp:19, cpp:115-) is shared. Keep it and move it next to `FootprintIntensityNode`. The CPU palette reference test that reads `heatmap_intensity.frag` (`tests/render/test_heatmap_palette.cpp:71`) goes with it. |
| `libs/core/marketdata/dispatch/MessageParser.hpp` | 53 | Listed in `libs/core/CMakeLists.txt:35`; never included. |
| `libs/core/marketdata/sinks/IMarketDataSink.hpp` | 10 | Never included. |
| `libs/gui/GPUTypes.h`, `libs/gui/ChartMode.h` | 35 + 11 | Never included (`ChartMode.h` is listed in `libs/gui/CMakeLists.txt:54`). |
| `libs/gui/CandlestickBatched.hpp` (include shim) | 6 | Every includer uses `render/CandlestickBatched.hpp` directly. |
| `tests/marketdata/fixtures/mock_authenticator.hpp` | 35 | Includes `marketdata/auth/IAuthenticator.hpp`, which no longer exists. Unbuilt and unincluded. |
| `tests/marketdata/generate_test_orderbook.py`, `generate_small_test_orderbook.py` | 373 | "Fractal zoom" generators for a removed feature. Not referenced anywhere. |
| `scripts/main.py` | 6 | uv "Hello from scripts!" boilerplate. |
| `scripts/ws_candle_history.py`, `scripts/ws_candle_stream.py` | 95 | Not referenced. They default to `ws://127.0.0.1:8080`, but the server is TLS-only, so they cannot connect as written. |
| `scripts/release/mac-package-release.sh` | 3 | A "compatibility wrapper" for `release_macos.sh`, which the no-backward-compatibility rule forbids. Only `scripts/release/README.md` mentions it. |
| `scripts/lizard_scan.ps1`; `scripts/setup/bootstrap-unix.sh`, `bootstrap-windows.ps1`, `setup-windows.ps1` | 138 + ~200 | Zero references anywhere. Owner call: keep them as manual tools or drop them. |
| `sentinel.yaml.template` (repo root, out of scope; listed for completeness) | — | No code reads `sentinel.yaml`. `ConfigLoader.hpp:3` still claims it does. |

### 1b. A subsystem with no consumer: the GUI screener child process

- `MainWindowGPU::startScreenerServer()` (`MainWindowGpu.cpp:784-~900`, called at :1246; excluded file) spawns `scripts/screener/screener_server.py` on port 17200. Before each start it runs `lsof -ti tcp:17200 | xargs kill -9` (:823, :868).
- **Nothing in the GUI connects to 17200.** There is no `QWebSocket`, no `ws://` and no port use in `libs/gui` (grep). `ScreenerDock` gets its data from the stream server instead: `ScreenerDock.cpp:282` calls `requestScreenerData`, the server's `screener_request` handler (`SentinelStreamServer.cpp:1328-1410`) runs `screener_fetch.py`, and the client receives `screener_update`.
- What goes with it:
  - `--no-screener` (`apps/sentinel_gui/main.cpp:112-115`)
  - `ClientGuiConfig::startScreener` (`ConfigTypes.hpp:197-199`)
  - `scripts/screener/screener_server.py` (185 lines) and `scripts/screener/test_screener.py` (178; it probes 17200)
  - `OWNER_PORTS = (17100, 17200)` in `scripts/dev/gui-host.py:66` and the related cases in `test_gui_host.py`
  - Keep `screener_core.py` (`screener_fetch.py` imports it).
- Side effect: `Qt6::WebSockets` is linked (`libs/gui/CMakeLists.txt:405`, root `CMakeLists.txt:99`) but no source uses it. The same holds for `Qt6::Charts` (`libs/gui/CMakeLists.txt:403`): no `QChart` anywhere.

### 1c. Dead status-bar telemetry chain

- `StatusBar::setCpuUsage/setGpuUsage/setFrameStats/setLatency/setCoinbaseLatency/setUploadBandwidth` are empty inline bodies (`libs/gui/widgets/StatusBar.hpp:16-22`, "Compatibility with existing hubs").
- Their only inputs are wired at `MainWindowGpu.cpp:246-254`. Everything feeding them is therefore dead:
  - `PerformanceMonitor` CPU/GPU/latency/upload-bandwidth signals, `updateCpu/Gpu/Latency`, the `m_cpuUpdateTimer`, and the Windows PDH counters (`PerformanceMonitor.cpp:28-32, 117-124, 129-270`)
  - `addUploadBytes` (`HeatmapIntensityNode.cpp:105,185`, `FootprintIntensityNode.cpp:105`)
  - Getters with no callers: `getCpuUsage/getGpuUsage/getLatency/getUploadBandwidthMBps` (`PerformanceMonitor.hpp:25-28`)
  - The wire message `coinbase_latency`, end to end: `SentinelServerApp.cpp:185`, then `SentinelStreamServer::broadcastCoinbaseLatency`/`registerLatencySender` (:845, :2340-2366), then `SentinelStreamClient::handleCoinbaseLatencyMessage` (:1518), which ends at a no-op slot.
- What stays: `frameStats()`/`frameStatsText()` (UGR :1732, `HeatmapChartControls.cpp:321`, `MainWindowGpu.cpp:1696`) and `FrameStatsWindow`.

### 1d. Functions and members with no callers (production and tests)

| Symbol | Location |
|---|---|
| `Cpp20Utils::formatTradeLog/OrderBookLog/ErrorLog/SuccessLog/PerformanceMetric/Throughput`, `fastStringToInt` (x2), `asciiToLower`/`equalsIgnoreCase` (internal only) | `libs/core/Cpp20Utils.hpp:50-132` (~75 of 231 lines) |
| `DataProcessor::setPriceResolution` (no-op body, but called from UGR :501), `getPriceResolution`, `addTimeframe`, `suggestTimeframe`, `getDisplayMode`, `isManualTimeframeSet`; `setTimeframe` only logs and stores unused state; `m_manualTimeframe*`, `m_currentTimeframe_ms` | `libs/gui/render/DataProcessor.cpp:276-310`, `.hpp:37-44,57-59`. Removing the UGR callers (:501, :640) belongs to the UGR auditor. |
| `CandlestickBatched::addCandle`, `loadDemoData` (Q_INVOKABLE, no QML user), signal `candleClicked` (no `onCandleClicked`) | `render/CandlestickBatched.hpp:93-94`, `.cpp:201` |
| `FootprintIntensityNode::setCellBorderAlpha/Frac`, `setCellDebug(Enabled)`, `setNeutralColor` | `render/FootprintIntensityNode.hpp/.cpp` |
| `FootprintStreamState::pendingUploadCount`, `updateRange` | `render/FootprintStreamState.*` |
| `VolumeProfileRenderer::setBarColor/setPocColor/setWidthFraction/setOverlay` (`setVaColor` is test-only) | `render/VolumeProfileRenderer.*` |
| `GridViewState::clearPanVisualOffset` | `render/GridViewState.cpp:226` |
| `TimeAuthority::observeEventTime` | `render/TimeAuthority.cpp:30` |
| `PaperTradeOverlayModel::setRiskState` | `render/PaperTradeOverlayModel.*` |
| `ThemeManager::availableThemes`, `LayoutManager::defaultLayoutName/resetToDefault`, `StatusBar::setReadyStatus/showVersion`, `PriceAxisModel::getDecimalPlaces`, `AxisModel::calculateNiceStep` (virtual, never called), `QmlSceneController::graphicsApiName`, `DockablePanel::panelId`, `SecApiClient::requestedTicker`, `LabItem::setUploadBudgetBytes` | various `libs/gui` headers |
| `ChunkCodec::chunkSourceForHmc2Layer` | `libs/core/heatmap/ChunkCodec.cpp:210`, `.hpp:24` |
| `ChunkLru::forEachMutable` | `libs/core/heatmap/HeatmapTiles.hpp:197` |
| `Trade::getProductId/setProductId` | `libs/core/marketdata/model/TradeData.h` |
| `TradingEngine::findOrder` | `libs/core/trading/TradingEngine.cpp:62` |
| `ServerDataModel::getLiveOrderBook`, `collectFootprintTrades` | `ServerDataModel.cpp:491,552` (collision) |
| Signals `orderUpdateBroadcast`, `positionUpdateBroadcast`, `riskOrderUpdateBroadcast`, `algoOrderEventBroadcast`, `pnlSnapshotBroadcast`: emitted, never connected | `SentinelStreamServer.hpp:58-62`, emitted at `.cpp:2418-2475` (collision) |
| `ConfigLoader::getLoadedFiles` + `s_loadedFiles` (test-only: `test_heatmap_plumbing.cpp:274`) | `ConfigLoader.cpp:13,346,366,375` (collision) |

Test-only APIs that are fine to keep (named `*ForTest`, or test seams): `Hmc2Store::*ForTest`, `RecordingLive::*ForTest`, `LabData::*ForTest`, `AgentHostMode::resetForTests`.

### 1e. Dead config keys and CLI flags

- **`gui.default_order_qty`.** Read at `ConfigLoader.cpp:309` into `ClientGuiConfig::defaultOrderQty` (`ConfigTypes.hpp:196`) and never used again. It also appears in `config/client_config.yaml:32`.
- **The `server:` block in `config/client_config.yaml:34-37` is silently ignored. This is a bug, not just dead config.** `parseClientConfig` reads `server` only under a `client:` wrapper (`ConfigLoader.cpp:322`: `if (clientNode && clientNode["server"])`), and the shipped file has no `client:` root. It works today only because the defaults in `ConfigTypes.hpp:209-214` equal the file's values. Editing `host` there does nothing. `apps/sentinel-lab/main.cpp:77-78` documents "default: config server.host". No test covers it. (ConfigLoader: collision.)
- **`--heatmap-renderer`.** Accepted and ignored (`apps/sentinel_gui/main.cpp:110,121-124`). No caller passes it (scripts grep). The same compatibility surface appears in:
  - the `heatmap.renderer` warning, `ConfigLoader.cpp:241-246`
  - the settings patch accepting `"renderer":"gpu"` as a no-op (`tests/render/test_heatmap_settings_ui.cpp:137`, `test_heatmap_plumbing.cpp:244`)
  - `AgentApiCodec.cpp:649` and `HeatmapChartControls.cpp:462` (both commented "goes in S8b")
  - `gui-host.py:283`
- **Legacy aliases in `ConfigLoader.cpp`:** `heatmap.recenter` (:107), `heatmap.timeframes` vs `timeframes_ms` (:125-134), the optional `server:` wrapper (:92-100), root-level MDC keys (`readMdcScalar` :26-33). Under the owner's no-compatibility rule all of them are candidates. Most die with D-b2b and S8b anyway.
- **Env overrides** `SENTINEL_HEATMAP_GAMMA/CONTRAST/SHADER_FLOOR` (`libs/gui/config/GuiConfigStore.cpp:17-47`): a second config path next to YAML. The keys are still live (UGR :1555). Owner call.

### 1f. Commented-out code

Essentially none. The only block is `libs/gui/models/TimeAxisModel.cpp:116-117` (a `//qDebug()`). There is no `#if 0` anywhere.

### 1g. Unused includes (heuristic; confirm by compiling)

- **`SentinelLogging.hpp` in `libs/core/marketdata/model/LiveOrderBook.cpp`.** No `sLog_` call in the file.
- **`ws/SubscriptionManager.hpp` in `MarketDataCoreEngine.hpp`.** It could move to the `.cpp`.
- **`ITimeAxisMappingProvider.hpp` in `render/AlgoOverlayRenderer.hpp`, and `ChartTextLayout.hpp` in `render/ChartTextRenderer.hpp`.** Neither header names anything from the include.
- **Dock headers in `mainwindow/MenuBuilder.cpp`, `LayoutOrchestrator.cpp` and `ShortcutBinder.cpp`.** Some may be needed for complete-type casts.
- **`../config/GuiConfigStore.hpp` in `datasources/RemoteGridDataSource.hpp`.**
- **`Cpp20Utils.hpp` drags `<format>`, `<charconv>`, `<chrono>` and `TradeData.h` into 6 TUs.** Its users need only `fastStringToDouble`, `fastSideDetection`, `parseISO8601` and `formatExchangeTimestamp`.

---

## 2. Chain-of-thought and agent-journal comments

Production code is mostly clean. There are no "Actually", "Wait,", "Let me" or "Hmm" lines in `libs/` or `apps/`; the one "Let's" is in `TickBinaryLogger.hpp:46` (deleted in step 4). The narration sits in `tests/`.

Counts (scope minus excluded files; comment lines):

| Dir | Review/fix-round narration | Slice/phase tags (S5c, S8a, A2, D-a, "slice X") | FM-/INV- refs | Dated/owner-decision refs |
|---|---|---|---|---|
| libs/core | 0 | 16 | 10 | 29 |
| libs/gui | 0 | 21 | 3 | 13 |
| apps | 0 | 1 | 0 | 0 |
| scripts | 0 | 1 | 0 | 3 |
| tests | **72** | **142** | 31 | 65 |

Narration by file: `test_ugr_gpu.cpp` 29, `test_heatmap_source_controller.cpp` 16, `test_shadow.cpp` 5, `test_heatmap_tile_node.cpp` 4, `test_heatmap_live_node.cpp` 3, `test_ChartRaster.cpp` 3, `shadow_mutations.py` 3, `test_book_recorder.cpp` 2, `test_heatmap_live_client.cpp` 2, others 1 each.

**FM/INV references.** Most libs uses name a guardrail the code relies on and are worth keeping: `CaptureMetrics.hpp:52` (INV-094), `RollCli.cpp:15` (INV-115), `ShadowRoller.cpp:53` (INV-114), `CaptureApp.cpp:251` and `CaptureSession.cpp:73` (FM-127), `SentinelStreamServer.cpp:2094,2168` (FM-154), `LocalChunkTransport.cpp:27` (FM-145). The weak ones are process references with no invariant behind them: `ProtocolValidation.hpp:2` and `tests/marketdata/test_protocol_validation.cpp` (INV-020/FM-014/015/017 as section titles). `_agent/` is gitignored, so an FM number alone is unresolvable for anyone without the scratchpad. Each reference should keep a one-clause statement of the guardrail; most already do.

Worst ~20 examples (rewrite each as the behaviour it checks, or delete):
1. `tests/render/test_ugr_gpu.cpp:2675` `// ---- A2 fix round 1`; likewise :2955 `A2 fix round 3` and :3754 `A2 fix round 2` (section headers that are review rounds, out of order)
2. `tests/render/test_ugr_gpu.cpp:792,820,844` `// Review round 2, fix 1/2/3: ...`
3. `tests/render/test_ugr_gpu.cpp:2087,2110,2140` `// Review round 3: ...`
4. `tests/render/test_ugr_gpu.cpp:2605` `// Acceptance 9 (slice A deferred finding, Astra's counterexample): ...` (an agent name in test prose)
5. `tests/render/test_ugr_gpu.cpp:4367` `// Review item 7 (pre-existing): ...`
6. `tests/render/test_ugr_gpu.cpp:4391-4394` "This executable used to link vcpkg's libskia.a ... skia is gone now" (history)
7. `tests/render/test_heatmap_source_controller.cpp:875-1216` `// Review fix 1..7: ...`
8. `tests/render/test_heatmap_source_controller.cpp:1306-1504` `// Re-review P1/P2: ...` (x5)
9. `tests/render/test_heatmap_source_controller.cpp:1566,1590,1605` `// Final round P1/P2: ...`
10. `tests/render/test_heatmap_tile_node.cpp:975,996,1026,1053` `// Review fix 1..4: ...`
11. `tests/render/test_heatmap_live_node.cpp:788,826,863` `// Review fix 1/3/4: ...`
12. `tests/render/test_ChartRaster.cpp:509,529` `// Fix round 1, finding 5/3: ...`
13. `tests/roller/test_shadow.cpp:1320,1370,1418,1459,1529` `// Review r1/r2 finding N: ...`
14. `tests/roller/test_roller.cpp:411` `// Review r1 finding 3: ...`
15. `tests/roller/deploy_marker_test.sh:2` `# A9 and review r1/r2 finding 2-3: ...`
16. `tests/roller/shadow_mutations.py:81,144,166,192` `# Slice D-a ...`, `# Review round 1/2/3.` (mutation groups named after review rounds)
17. `tests/servermodel/test_book_recorder.cpp:644,696` `// Fable review 2026-10-01: ...` (model name and date)
18. `tests/render/test_lab_item.cpp:283` `// S5L-c review fix 2: ...`
19. `tests/render/test_CandleDataSource.cpp:217` `// Review item 8 (pre-existing): ...`
20. `tests/trading/test_backtest_core.cpp:598` `// enable when D-b1 (lt-claude/roller-db1) lands` (a branch name in source; it resolves at step 1)

Production comments that describe history rather than behaviour (smaller set):
- `libs/core/roller/ShadowRoller.cpp:50`: "Option C live path (owner 2026-10-06)"
- `libs/core/capture/CaptureFanout.hpp:8`: "Slice C merge seam"
- `libs/core/capture/RawCapture.hpp:96`: "2026-10-02: metadata naming ... is refused"
- `libs/core/capture/CaptureRouting.hpp:10`: "historic contract of the 2026-09-30..10-02 archive; the writer is removed"
- `libs/core/servermodel/BookRecorder.cpp:796`: "(2026-09-30: ..."
- `libs/gui/datasources/RemoteGridDataSource.cpp:204`: "(it seeded the chart ~$600 off, 2026-10-02)"
- `libs/gui/render/DataProcessor.cpp:4-8`: "nothing here touches it since S8a"
- `libs/core/protocol/SentinelStreamClient.cpp:816-819` and `tests/marketdata/test_sentinel_stream_client_write.cpp:272`: "until S8b removes it". These go with S8b.
- `apps/sentinel_gui/main.cpp:110,124`; `libs/core/ConfigLoader.cpp:241`; `gui-host.py:29,283`: "(S8a)"

Plan and spec links (`docs/research/...` in `LabItem.hpp:3,7`, `S5Bench.hpp:7-8`, `HeatmapSpanPlanner.hpp:3`, `HeatmapResolution.hpp:2`, `ChartRaster.hpp:1`, `ConfigTypes.hpp:96`) are acceptable why-pointers. The research files are frozen after landing, though, so "owner decision 1" with no statement of the decision is weak. `HeatmapTiles.hpp:132` and `HeatmapSpanPlanner.hpp:7,121` do state the rule; those can stay.

---

## 3. Duplicated helpers across libs

| Helper | Copies | Suggested home |
|---|---|---|
| CRC-32 | One implementation (`HmcolFormat.cpp:10-35`), but it lives in a format the S8b deletion removes. `RawCapture.cpp:38` wraps it. | A small `libs/core/Crc32.hpp`, before S8b (already in the S8 plan :96). |
| Default capture socket `~/Sentinel-runtime/run/capture.sock` | 4: `ShadowRoller.cpp:372`, `CaptureApp.cpp:59`, `CaptureFanout.cpp:53`, `CaptureFanout.cpp:166` | One `capture::defaultFanoutSocket()` in `CaptureFanout.hpp` |
| Directory/file fsync | 3: `PersistenceIo.hpp:23,35,165` (canonical), `RawCapture.cpp:57-77` (`syncFd`/`syncDirectory`, partly reusing PersistenceIo), `Hmc2Store.cpp:388` (thin wrapper) | Make `RawCapture` call `persistence::syncDirectory`. |
| Little-endian put/get | 4: `RawCapture.cpp:41-52`, `CaptureFanout.cpp:39-43`, `ShadowRoller.cpp:35-40` (`little()`, which parses the same fanout framing as `CaptureFanout`), `Hmc2Store.cpp:68-95` | `libs/core/ByteCodec.hpp`. The fanout packet encoder and decoder should live together in `CaptureFanout`, and `ShadowRoller::LiveSource` should reuse them. |
| Varint/zigzag | 1 (`RecordingCodec.hpp:63-102`) | Fine. |
| Day/minute ms constants | 7+: `heatmap::kDayMs` (`SparseColumns.hpp:8`, canonical), `SessionManager.hpp:44`, `Roller.cpp:14`, `ShadowRoller.cpp:18`, `MarketEventSource.cpp:349` **and** `:412` (twice in one file), `TpoProfileModel.cpp:54`; raw `86'400'000` in `Hmc2Store.cpp:966,1019`, `TradeOverlayPublisher.cpp:17,106,279-280`, `RecordingPage.cpp:363`, `RecordingHistoryWire.hpp:204` | One `libs/core/TimeUnits.hpp` |
| UTC day string `YYYY-MM-DD` | 4: `Hmc2Store.cpp:404` (`dayName`, std::chrono), `CaptureVerifier.cpp:180` (`utcDay`, QDateTime), `Roller.cpp:104`, `MarketEventSource.cpp:496` | One helper |
| UTC CLI time parsing | 2 near-identical: `roller::parseTime` (`Roller.cpp:50`) and `parseUtc` in `apps/sentinel-backtest/main.cpp:35` (stricter). `Cpp20Utils::parseISO8601` (:133) is a hand-rolled Coinbase timestamp parser, a different job. | Backtest should call `roller::parseTime`, or the reverse. |
| Wall-clock ms | 4 in core: `sentinel::logging::nowMs` (`SentinelLogging.cpp:86`), `ShadowRoller.cpp:20` `nowMs`, `ServerDataModel.cpp:14` `localNowMs` (slice E removes it), `BookRecorder.cpp:89` `systemNow` | Reuse `logging::nowMs` or a TimeUnits helper. |
| Bounded subprocess runner | 2: `runBoundedProcess`/`terminateProcessTree` in `SentinelStreamServer.cpp:65-125` (a screener concern inside the WS server); the GUI's `widgets/ResearchProcess.*` (async) | Move the server one to its own file when S8b slims the server. |
| Price-to-tick rounding | Small, local; acceptable (`AgentApiSnapshots.cpp:10`, `BinCell.cpp:30,52`). | — |
| JSON helpers, logging wrappers | No duplicates found. Logging goes through `sLog_*`. Stray `std::cout` and `fprintf` appear only in CLI/bench code (`CaptureApp.cpp:72`, `RhiBackend.cpp:63,81`, lab benches). | — |

---

## 4. Naming and stale docs or comments

**Names that no longer match:**
- **`ShadowRoller` / `ShadowConfig` / `roller_shadow:` YAML key / `compareShadow` / `sentinel_roller_shadow_*` metrics.** Since R2 (2026-10-08) the roller is the only recorder and serves clients. The header contradicts the code:
  - `ShadowRoller.hpp:9-10` says "No callback into primary recording/streaming".
  - Yet `ShadowConfig.hpp:39-55` installs `publisher`, `retractLive` and `ensureLiveFinal`, which feed the live service.
  - The name spreads across `ShadowRoller.cpp` (34 uses), `SentinelServerApp.cpp` (13), `ConfigLoader.cpp` (9), `ServerDataModel.cpp` (6), `Diff.cpp` (5), metric names (`ShadowRoller.cpp:593-635`) and `ops/monitoring/README.md:258-280` plus dashboards and alerts.
  - Renaming touches deployed config and metric history, so it should happen after step 5 (high risk; it collides).
- **`config/server_config.yaml:52`: "Shadow pipeline: primary recording/ and client stream remain authoritative."** False since `recording.source: roller`.
- **`recording.source` default `"primary"`** (`ConfigTypes.hpp:111-115`) while the deployed value is `roller` and D-b2b refuses `primary`.
- **`DataProcessor`.** It now processes only trade overlays (`DataProcessor.hpp:1`). `TradeOverlayProcessor` would say what it does; it is a hot path, so rename only.
- **`SentinelStreamClientTransport`.** It is the heatmap `ChunkTransport` adapter over the stream client, not a transport (`.cpp:8-150`).
- **`HeatmapIntensityNode.{hpp,cpp}`** would end up housing only `HeatmapColumnTexture`, which serves the footprint (see 1a).
- **`MarketDataCoreEngine`/`MarketDataFeeds`** stay in `libs/core/marketdata` but are capture-only after D-b2b.
- **`StatusBar.hpp:16`** "Compatibility with existing hubs" (see 1c).

**Stale comments and docs that contradict code:**
- `libs/core/ConfigLoader.hpp:3`: "Load sentinel.yaml". It loads `config/{server,client}_config.yaml`.
- `libs/core/config/ConfigTypes.hpp:91`: "scripts/certs/gen-certs.ps1". The scripts are in `certs/`.
- `libs/core/config/ConfigTypes.hpp:34-38` and `HeatmapColumnStore.hpp:7`: cite `docs/private/plans/F1_HEATMAP_PERSISTENCE.md`, which does not exist. The text says "Default OFF until validated", but `server_config.yaml:13` has it on. Goes with the deletion step.
- `libs/core/config/ConfigTypes.hpp:96-97`: "one set of values for now (BTC-USD). Per-asset ticks come later". The roller derives per-product grids (`roller/Grid.hpp:10-13`). Only the primary format is BTC-only, and slice E removes it.
- `libs/gui/widgets/HeatmapTelemetryDock.hpp:6`: "nullopt while the chart draws with the legacy renderer". There is no legacy renderer.
- `libs/gui/render/GridViewState.hpp:43` and `GridViewState.cpp:9`: "the legacy renderer sets none" / "(legacy renderer)".
- `libs/core/heatmap/HeatmapResolution.hpp:27`: "The legacy page path (RecordingBandPolicy) uses this". That path is test-only now.
- `docs/research/2026-10-s8-legacy-deletion.md:21`, finding 8: "HeatmapIntensityNode ... used by footprint". Wrong (see 1a). The file is frozen, but the S8b writer should be told.
- `scripts/dev/budget.sh`, `gui-host.py:4-9`, `build-queue.sh:10`, `bootstrap-cloud.sh:2`: name Codex and Claude. These are tooling docs, so this is acceptable.

---

## 5. Files over 800 lines (excluded set and (b) left out)

| File | Lines | Verdict |
|---|---|---|
| `libs/core/protocol/SentinelStreamServer.cpp` | 2485 | **Mixes jobs.** It holds TLS/WS session lifecycle; `Session::handle_message` (:910-1422, one ~510-line if-chain over every message type); legacy heatmap (b); chunk and live service glue; screener subprocess plumbing (:65-125, :1328-1410); trading/algo routing (:2369-2475); latency fan-out; metrics. S8b and D-b2b remove a large share. After that, split message dispatch into per-family handlers and move the screener runner out. (Collides.) |
| `libs/core/protocol/SentinelStreamClient.cpp` | 1577 | Mostly cohesive (one client), but the per-message JSON parsers (footprint :1049-1218, TPO :1268-1438, VP :1439-1517, ~550 lines) belong in the existing `SentinelStreamClientParseHelpers.*` (190 lines). It also carries legacy S8b drop logic (:816). |
| `libs/core/servermodel/Hmc2Store.cpp` | 1402 | Cohesive (the HMC2 format) but holds three jobs: codec (:23-400), `Hmc2Reader` (:621-1108), writer `Hmc2Store` (:1110-1370). A reader/writer split is optional. Low priority. |
| `libs/core/capture/CaptureVerifier.cpp` | 1176 | Cohesive: offline journal verification (file index and sort, trade audit, replay, report). OK. |
| `libs/core/servermodel/BookRecorder.cpp` | 1053 | Cohesive (the minute recorder the roller uses: `Roller.cpp:94,135`, `ShadowRoller.cpp:64-67`), but it carries primary-only paths (`localClock`, overflow invalidation) that slice E removes. Collides. |
| `libs/core/roller/ShadowRoller.cpp` | 1017 | **Mixes jobs.** `LiveLead` (:57-226, the live provisional publisher), `LiveSource` (:227-537, fanout socket client plus journal resume), the supervisor `Impl` (:548-1000), and the hourly self-check comparison (:895-960; whether it stays is an open owner question in STATUS). Split into `LiveLead`, `FanoutClient` (sharing framing with `CaptureFanout`), `RollerSupervisor` and `SelfCheck` when renaming. Collides. |
| `libs/gui/widgets/PaperTradingDock.cpp` | 979 | **Mixes jobs.** Manual orders, the algo tab, a full backtest runner UI (`buildBacktestTab` :365-606, ~240 lines with its own subprocess handling), the PnL panel and market health. Extract a `BacktestPanel`. No collision. |
| `libs/gui/qml/DepthChartView.qml` | 1019 | Chart host plus ~600 lines of inline paper-trade order and position pills with TP/SL launchers (:148-~740). Extract `PaperTradeOverlayLayer.qml`. No collision (QML only). |
| `libs/core/servermodel/ServerDataModel.cpp` | 900 | Slice E and D-b2b shrink it. Collides. |
| Tests: `test_ugr_gpu.cpp` 4480, `test_shadow.cpp` 1970, `test_raw_capture.cpp` 1952, `test_chart_ui.cpp` 1850, `test_heatmap_source_controller.cpp` 1773, `test_heatmap_settings_ui.cpp` 1719, `test_hmc2_store.cpp` 1403, `test_heatmap_tile_node.cpp` 1348, `test_heatmap_live_client.cpp` 1290, `test_heatmap_live_node.cpp` 1208, `test_book_recorder.cpp` 1176, `test_heatmap_chunk_wire.cpp` 1155, `test_backtest_core.cpp` 1136, `test_recording_live.cpp` 1042, `test_heatmap_gpu_binner.cpp` 1004, `test_recording_server_stop.cpp` 901, `test_chunk_fetcher.cpp` 888, `test_MainWindowSymbolLifecycle.cpp` 836, `test_TradeOverlay.cpp` 814 | | `test_ugr_gpu.cpp` is the outlier: one binary covering zoom, raster, autoscale, symbol switch, QML input and link guards, sectioned by review round. Split it by behaviour family (zoom/glide, price fit, symbol lifecycle, QML input) when its narration is rewritten. The others are long but single-subject. |

---

## 6. Ranked cleanup list

Line deltas are net removals, approximate. "Wait" means it touches the collision set and has to land after (or inside) step 4.

| # | Item | Files | ~Lines | Risk | Collides? |
|---|---|---|---|---|---|
| 1 | Delete the dead QML: `ChartToolRail` + 5 controls, `CandleChartView`, `Theme` | `libs/gui/qml/**`, `libs/gui/CMakeLists.txt:276-283` | -720 | low | no |
| 2 | Delete the legacy heatmap node: `HeatmapIntensityNode`/`Material`, `heatmap_intensity.*`, `HeatmapRowGrouping.hpp` + its test and palette-shader parity entry. Move `HeatmapColumnTexture` beside `FootprintIntensityNode`. | `libs/gui/render/HeatmapIntensityNode.*`, `render/shaders/heatmap_intensity.*`, `HeatmapRowGrouping.hpp`, `tests/render/test_HeatmapRowGrouping.cpp`, `tests/render/test_heatmap_palette.cpp:71`, `tests/render/CMakeLists.txt:68-76`, `FootprintOverlayRenderer.*` | -600 | low-med (footprint upload path; GPU smoke) | no. It contradicts the S8 plan's "keep" note; tell S8b. |
| 3 | Remove the unused GUI screener child, `--no-screener`, `startScreener`, `screener_server.py`, `test_screener.py`, the 17200 handling in gui-host | `MainWindowGpu.cpp:784-900,1246` (MainWindowGpu auditor), `apps/sentinel_gui/main.cpp`, `ConfigTypes.hpp:197-199`, `scripts/screener/*`, `scripts/dev/gui-host.py`, `test_gui_host.py`, `test_MainWindowSymbolLifecycle.cpp` | -550 | low-med (also removes an `lsof \| xargs kill -9` at GUI start) | `ConfigTypes.hpp`: 3 lines, easy rebase |
| 4 | Delete small dead files: `MessageParser.hpp`, `IMarketDataSink.hpp`, `GPUTypes.h`, `ChartMode.h`, `LabTextItem.*` + shim + test registrations, `CandlestickBatched.hpp` shim, `mock_authenticator.hpp`, `generate_*orderbook.py`, `scripts/main.py`, `ws_candle_*.py`, `mac-package-release.sh` | as listed in 1a | -1000 | low | no |
| 5 | Remove the dead status telemetry: StatusBar no-op slots, the PerformanceMonitor CPU/GPU/latency/bandwidth path and PDH counters, `addUploadBytes`, client `coinbaseLatencyReceived` | `StatusBar.hpp`, `PerformanceMonitor.*`, `FootprintIntensityNode.cpp:105`, `SentinelStreamClient.*:185,1518`, `MainWindowGpu.cpp:246-254` | -200 | low | GUI and client side: no. The server half (`broadcastCoinbaseLatency`, `SentinelServerApp.cpp:185`): wait. |
| 6 | Trim dead functions (table 1d, non-collision rows), `Cpp20Utils` dead block, DataProcessor stubs (needs the UGR call sites at :501, :640) | see 1d | -250 | low (DataProcessor is a hot path, but this only deletes code) | no, except the `ServerDataModel` and `SentinelStreamServer` rows (wait) |
| 7 | Drop the unused link deps `Qt6::Charts` and `Qt6::WebSockets` (after 3) | `CMakeLists.txt:99`, `libs/gui/CMakeLists.txt:403,405`, maybe `vcpkg.json` | -3 | low (build only; check the macOS deploy bundle step) | no |
| 8 | Extract the duplicated helpers: socket path, `syncDirectory`, LE codec, `TimeUnits`, UTC day format, CLI UTC parse | `capture/*`, `servermodel/Hmc2Store.cpp`, `roller/Roller.cpp`, `trading/MarketEventSource.cpp`, `apps/sentinel-backtest/main.cpp`, `SessionManager.hpp` | -80 | low-med (byte formats: tests must pin bytes) | Mostly no. Leave `ShadowRoller.cpp`'s copies until after step 4. |
| 9 | Extract CRC-32 out of `HmcolFormat` (already a planned S8b sub-step) | `HmcolFormat.*`, new `Crc32.hpp`, `Hmc2Store.cpp`, `RawCapture.cpp` | ±0 | low | It is a precondition for S8b; it can land now because it touches neither `ServerDataModel` nor the server. |
| 10 | Fix stale comments (section 4 list, non-collision ones) | `HeatmapTelemetryDock.hpp:6`, `GridViewState.*`, `HeatmapResolution.hpp:27`, `StatusBar.hpp:16` | ±0 | low | no |
| 11 | Rewrite test narration: "Review fix N", "fix round", "Fable review", agent names and branch names become behaviour statements; split `test_ugr_gpu.cpp` by family | `tests/render/*`, `tests/roller/*`, `tests/servermodel/test_book_recorder.cpp` | ±0 (about 210 comment lines edited) | low, but heavy churn | Do the `tests/render` part now. Leave `tests/roller` and `tests/servermodel` until after step 4, because the deletion wave edits those files. |
| 12 | Split `PaperTradingDock` (backtest panel) and `DepthChartView.qml` (paper-trade layer) | `libs/gui/widgets/PaperTradingDock.*`, `libs/gui/qml/*` | ±0 | med (UI behaviour; needs GUI-host screenshots) | no |
| 13 | Config cleanup: fix or remove the ignored client `server:` block (**bug**), drop `gui.default_order_qty`, the legacy aliases and the `heatmap.renderer` warning, `--heatmap-renderer`, the settings/API `"renderer"` no-op | `ConfigLoader.cpp`, `ConfigTypes.hpp`, `config/client_config.yaml`, `apps/sentinel_gui/main.cpp`, `AgentApiCodec.cpp:649`, `HeatmapChartControls.cpp:462`, settings model, related tests | -60 | low-med (`client_config.yaml` is user-facing) | **wait** (ConfigLoader); fold into D-b2b/S8b. The `main.cpp` and AgentApi/settings parts could go now. |
| 14 | Split `SentinelStreamServer.cpp` (message dispatch per family, screener runner out) and `SentinelStreamClient.cpp` parsers into `ParseHelpers` | `libs/core/protocol/*` | ±0 | med | server: **wait** (after S8b). Client parsers: no. |
| 15 | Rename Shadow to the roller's real role (`ShadowRoller`, `ShadowConfig`, `roller_shadow:`, metrics, `compareShadow`, header contract comment, `server_config.yaml:52`), and split `ShadowRoller.cpp` into LiveLead / FanoutClient / Supervisor / SelfCheck | `libs/core/roller/*`, `ConfigLoader.cpp`, `SentinelServerApp.*`, `ServerDataModel.cpp`, `config/server_config.yaml`, `ops/monitoring/*`, dashboards and alerts, tests | ±0 (+ split) | **high** (deployed config key, metric continuity, recorder service) | **wait**: after step 5, with owner sign-off on the metric and config rename. Fix the false header comment (`ShadowRoller.hpp:9-10`) and the YAML comment as part of step 4. |

Totals: about 3,500 lines are removable now without touching the collision set (items 1-7, 9 and part of 11). About 400 more go with or after step 4 (items 13-15 and the colliding rows of 5 and 6).
