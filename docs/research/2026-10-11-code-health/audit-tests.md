# Sentinel test-suite audit (read-only, main @ 180d3b7)

Scope: `tests/**` (100 .cpp files, ~51.8k lines including fixtures/benches, 1,551 `TEST*` cases, 96 ctest entries from ~96 executables). Nothing was built or run; every timing figure below is an estimate from the code. Line numbers are `file:line` in the tree as read.

## 0. Headline findings

1. **The landing gate runs ctest serially.** `scripts/dev/agent-worktree.sh:134` runs `ctest --test-dir build/mac-clang --output-on-failure --timeout 600` with no `-j`/`--parallel`, and `CMakePresets.json` test presets set no `execution.jobs`. Each of the 96 entries is a whole gtest binary (no `gtest_discover_tests`). So the test phase costs the *sum* of every binary's time. This is the biggest single lever (section 7).
2. **About 154 GPU cases in 8 files skip and still report "Passed"** wherever `lab::gpuUnavailableReason()` is non-empty (Linux/CI/sandboxed Codex). Nothing turns a skip into a failure: there is no `SKIP_RETURN_CODE`, `FAIL_REGULAR_EXPRESSION` or "require GPU" switch anywhere in `tests/` or `cmake/`.
3. **Roughly 3.3k test lines exist only for code the pending deletions remove** (HeatmapColumnStore/hmcol, HeatmapTwapStreamer, legacy recording view wire/LiveBuilder, engine-to-server binding, primary BookRecorder path). Section 3 lists them.
4. **A handful of tests cannot fail, or only grep source text.** Section 1.
5. **Process gunk is concentrated in a few files:** `test_ugr_gpu.cpp` has 32 "Review fix/round/blocker/major/minor" comments; `test_ChartRaster.cpp` has 16 and `test_backtest_core.cpp` 6 test names carrying acceptance tags (`A3_…`, `B4…`).

---

## 1. Tests that cannot fail or test nothing meaningful

### 1a. No assertion, or assertions that cannot fail
| Test | Evidence | Verdict |
|---|---|---|
| `AlgoEngineTest.TickOnWrongSymbolDoesNothing` | `tests/trading/test_algo_trading.cpp:372-388`. It counts callbacks but never asserts; the body ends in `SUCCEED()` with "We just verify it doesn't crash". | Cannot fail unless it crashes. Assert `callbackCount`/no order events for ETH-USD, or delete it. |
| `HeatmapModelTiming.SyntheticMonthOfDeepHoursReportsComposeMilliseconds` | `tests/servermodel/test_heatmap_model.cpp:420-446`: "A single assert-free timing sample", builds 720×4096×2 ≈ 5.9M entries and prints `(informational)`. | No assertion, and it costs real time on every run. Move it to a bench target or `DISABLED_`. |
| `ShadowTest.HandoffLatencyBenchmark` | `tests/roller/test_shadow.cpp:970-1010`: 200 submissions paced by `sleep_until(begin + (i+1)*10ms)` (≥2 s), using the production 1 s block interval. | A benchmark in the default suite. Make it opt-in (env or `DISABLED_`). |
| `MessageDispatcher…HandleMissingChannel` | `tests/marketdata/test_message_dispatcher.cpp:268`. Its only assertion is `EXPECT_NO_THROW`. | Weak: also check the result (dropped and counted). |
| `SourceController…` teardown case ending in `SUCCEED()` | `tests/render/test_heatmap_source_controller.cpp:1303` | Useful only under ASan/Guard Malloc. Label it `sanitizer`, or keep it and note that. |

The other 30 assertion-free bodies found by scan delegate to asserting helpers (`expectBoundary`, `expectGrid`, `checkCap()`, `CorruptFraming` one-liners, and so on). They are fine.

### 1b. Source-text tests (grep a .cpp/.qml/.frag/.yaml instead of running code)
| Test | Evidence | Verdict |
|---|---|---|
| `ServerFeedAdmissionSource.AppChecksUpstreamResultBeforeAcknowledgement` | `tests/servermodel/test_server_feed_admission.cpp:381-396` reads `SentinelServerApp.cpp` and `find`s `"m_marketDataCore->add(symbol)"`, `"AddResult::CapacityExceeded"` and others. | Pins the exact spelling of code D-b2 deletes. **Removed with the deletion step.** |
| `ServerFeedAlerts.A1bSelectsEachPinnedProductForFiveMinutes` | `…feed_admission.cpp:466-475` substring-matches `rules.yaml` for alert A1b. | A1b is deleted in D-b2. **Removed with the deletion step.** |
| `AgentHostModeSources.DockRouteUsesHostAwarePersistencePolicy` | `tests/agentapi/test_AgentHostMode.cpp:140-145`: `source.contains("AgentHostMode::dockChangesPersist(body.persistDocks)")`. | A pure string pin; the behaviour test right above it (`:130`) already covers the policy. Delete it. |
| `AgentHostModeSources.NoNativeFormatQSettingsInTheGui` | `test_AgentHostMode.cpp:218-242` | A lint rule (AGENTS.md:125 says "a test enforces it"). It is valid as a guard but belongs in a `lint` label or a script, not the unit suite. |
| `AgentHostModeSources.EveryOutboundRequestInTheDataSourceIsGuarded` | `test_AgentHostMode.cpp:247-314`: regex over `RemoteGridDataSource.cpp` plus a tree walk of `libs/gui`. | A security guard, so keep it, but under a `lint` label. It walks the whole GUI tree on every run. |
| `HeatmapPalette.BothShaderMappingsUseTheCpuTexelCentres` | `tests/render/test_heatmap_palette.cpp:64-78` | Self-described source-text match of shader lines. Replace it with a GPU readback parity case that already exists in the tile-node suite, or keep it as `lint`. |
| `TradeBubbleControls.LayerOrderUsesRealQmlBindingAndPersistedChartApiSetting` | `tests/render/test_heatmap_settings_ui.cpp:1241-1252` slices `DepthChartView.qml` text at `"    TradeBubbleOverlayItem {"` and `"\n    }"`. | Brittle to indentation. Better: load the real QML (as `test_chart_ui.cpp:922` does). |
| Grafana rule pins | `tests/capture/test_capture_fanout.cpp:482-494`, `tests/roller/test_corrupt_blocks.cpp:234-246` (YAML-parsed, stronger than substring) | Config checks inside C++ unit binaries. Move them to one `ops` lint test. |

### 1c. Absence and backward-compat tests that pass when the feature is gone
- `test_heatmap_settings_ui.cpp:818-821` (`EXPECT_FALSE(findChild("renderer"/"makeDefault"/"savedRenderer"))`) and `test_chart_ui.cpp:502` ("the legacy threshold went in S8a"). These assert the absence of deleted UI. They are permanent "still deleted" checks and can go.
- `HeatmapPlumbing.RetiredRendererAndSourceYamlKeysAreIgnored`, `tests/render/test_heatmap_plumbing.cpp:189-201`. This is a backward-compat test, which AGENTS.md:41 says to delete. It would pass even if the loader had never known the keys.
- `HeatmapChartUi.TickSelectorIsDisabledWithoutAChartWithAReason`, `test_heatmap_settings_ui.cpp:1517-1531`. Only the `{"renderer","legacy"}` refusal line is legacy; trim that line.
- Legacy-format readers kept under test: capture RAWL2 v2 (`tests/capture/CMakeLists.txt:1-5`, `test_raw_capture.cpp:713,1724`, `test_capture_app.cpp:267`), hmc2 schemas 1-3 (`test_hmc2_store.cpp:819,1097`, `Hmc2LegacyFixture.hpp`), `LogFormat::LEGACY_VERSION` (`test_backtest_core.cpp:292`). These are owner decisions: they go when the old data is archived (AGENTS.md:41, 4b).

### 1d. Log-line-only assertions
- `ServerStartup.MissingTlsLeavesTheRequiredRecorderRunning`, `tests/servermodel/test_server_startup.cpp:89-112`. Seven of its nine checks are `log.contains(...)`, including `"mdcHost=127.0.0.1"` and `"Server default subscribe: count=1"`, which go stale with D-b2. It overlaps `StreamServerStartFailure.MissingTlsFilesReturnFalseAndCanRetry` (`test_recording_server_stop.cpp:701`).
- The other log checks (`test_recording_live.cpp:937,994`, `test_shadow.cpp:1347+`, `test_capture_app.cpp:163`) back up behavioural assertions. That is acceptable.

### 1e. Golden-hash change detectors
- `RollerLiveFixture.RecordedInputPreservesLiveBytes`, `tests/roller/test_live_fixture.cpp:51-62`, compares a SHA-256 of BookRecorder output against a constant. Any harmless encoder change turns it red. Keep it only while the bytes are a contract.

---

## 2. GPU cases that skip and report Passed

The gate is `lab::gpuUnavailableReason()` (`libs/gui/lab/RhiBackend.cpp:231`) or `gpu.rhi == nullptr`. Counts come from a scan of fixture `SetUp()` skips plus per-test skips:

| File | Cases | GPU-skippable | How |
|---|---:|---:|---|
| `tests/render/test_ugr_gpu.cpp` | 100 | **76** | fixtures `UgrGpu` (`:118`) and `UgrGlide : UgrGpu` |
| `tests/render/test_heatmap_tile_node.cpp` | 25 | **25** (all) | 20 per-test skips plus fixture `NodeWithController` (`:1096`) |
| `tests/render/test_heatmap_gpu_binner.cpp` | 23 | **16-17** | `if (!gpu.rhi) GTEST_SKIP()` (`:369…974`) |
| `tests/render/test_heatmap_live_node.cpp` | 12 | **12** (all) | fixtures `LiveNode` (`:253`), `LiveNodeWithController` (`:1042`) |
| `tests/render/test_heatmap_settings_ui.cpp` | 43 | **9** | fixture `HeatmapChartUi` (`:1453`) plus `OffscreenQuickTest` (`:1171`) |
| `tests/render/test_chart_ui.cpp` | 42 | **9** | fixture `ChartLabels` (`:1460`) |
| `tests/render/test_lab_item.cpp` | 7 | **5** | fixture `LabItemTest` (`:92`) |
| `tests/render/test_qsg_compute_spike.cpp` | 1 | **1** (all) | `:148` |
| **Total** | | **~154** | |

That is three whole ctest entries (`HeatmapTileNodeTests`, `HeatmapLiveNodeTests`, `QsgComputeSpikeTests`) that are 100 % skip-and-pass off-GPU, plus three Guard Malloc re-runs (`tests/render/CMakeLists.txt:193-228`, Apple only).

Other environment and opt-in skips are intentional, but they are also silent passes: `test_capture_app.cpp` 5 (non-POSIX), `test_hmc2_store.cpp` 6 (root/permissions/debug), `test_lab_output_guard.cpp` 5 (links), `test_research_ui.cpp` 6, `test_fonts.cpp` 3 / `test_market_health.cpp` 1 / `test_WatchRailScreener.cpp` 2 / `test_chart_ui.cpp` 2 (opt-in native screenshots), the env-gated `ChunkBench`, `HeatmapModel` real parity, and `CorruptFraming` with a real hour. In addition, 4 `DISABLED_` cases exist (`test_backtest_core.cpp:599`, `test_dom.cpp:693`, `test_recording_page.cpp:517,557`).

**Structural point:** many `UgrGpu` cases test camera and viewport logic (fits, follow-live, timeframe switches, autoscale; `test_ugr_gpu.cpp:645-2211`) and need a GPU only because the fixture builds an RHI scene. The CPU twins already exist as a pattern (`UgrZoomCpu`/`UgrWheelCpu`/`UgrPlacementCpu`, `:3191-3730`). Moving the pure-geometry assertions onto a CPU fixture would make them run everywhere and much faster.

---

## 3. Duplicates and tests removed with the deletion step

### 3a. Near-duplicates
| Scenario | Copies |
|---|---|
| QRhi loss recovery | `LabItemTest.ComesBackAfterTheQRhiIsRecreated` (`test_lab_item.cpp:157`) vs `UgrGpu.ComesBackAfterTheQRhiIsRecreated` (`test_ugr_gpu.cpp:1278`); the same FM-104 comment appears in both. |
| Shift+wheel price zoom and clamps | `LabItemTest.ShiftWheelScalesPriceOnlyWithinTheClamps` (`test_lab_item.cpp:201`) vs `UgrGpu.ShiftWheelScalesPriceOnlyWithinTheClamps` (`test_ugr_gpu.cpp:1353`). |
| Identical return-to-live | `UgrPlacementCpu.AnIdenticalReturnToLiveInvalidatesKeptPadding` (`:3191`) and `UgrGpu.…` (`:3196`) run the same helper, CPU and GPU. Keep the CPU one; the GPU one adds only `frames(2)`. |
| Whole-pixel raster rules A1-A8 | CPU math in `test_ChartRaster.cpp:48-330`, re-asserted end-to-end in `UgrGpu.APanMovesEveryLayerByTheSameWholePixels`/`AZoomInsideTheStepBandNeverOscillates`/`TheCursorPriceStaysWithinARowAcrossAnIntegerStep` (`test_ugr_gpu.cpp:566-732`) and `PixelAxisGeometry`/`TpoPixelGeometry` (`test_heatmap_settings_ui.cpp:462-570`). Layered rather than identical; keep one integration case per rule. |
| Missing TLS at start | `StreamServerStartFailure.MissingTlsFilesReturnFalseAndCanRetry` (`test_recording_server_stop.cpp:701`) and `ServerStartup.MissingTlsLeavesTheRequiredRecorderRunning` (`test_server_startup.cpp:89`). |
| Bind address | `StreamServerBind.DefaultIsLoopback`/`InvalidOrUnassignedAddressFailsStartup` (`test_recording_server_stop.cpp:587,625`), `ServerFeedConfig.BindAddressDefaultsFlatWrappedAndOverride`/`CheckedInConfigDefaultsToLoopback` (`test_server_feed_admission.cpp:398,423`), and `ServerStartup.InvalidBindAddressExitsBeforeRecordingOrFeeds` (`test_server_startup.cpp:69`, which spawns a real server 3×). |
| Grafana alert pins | Three files, three styles (1b). |
| Misfiled suites | `CandleHistoryPaging` and `StreamServerBind`/`StartFailure` live in `test_recording_server_stop.cpp`; `ServerFeedConfig` in `test_server_feed_admission.cpp`; `LocalChunkTransportFaults` in `test_ugr_gpu.cpp:4396`. |

The whole of `test_lab_item.cpp` (310 lines) drives the standalone lab item through the same scenarios the production chart suite covers. Unless the lab item is itself a product, delete the five GPU cases and keep `LabDataServer`/`LabDataService`.

### 3b. Removed with the deletion step
| Pending deletion | Tests that go with it |
|---|---|
| **HeatmapColumnStore / hmcol** | `tests/servermodel/test_heatmap_column_store.cpp` (954 lines, ctest `HeatmapColumnStoreTests`); `test_hmcol_format.cpp` (181, `HmcolFormatTests`; first move its CRC32 cases, because `Hmc2Store.cpp:119,144,548,746` still uses `hmcol::crc32`); `hmcol_seed.cpp` (81); `ServerFeedAdmissionTest.ReleasedGuiFeedStopsLegacyColumnStore` (`test_server_feed_admission.cpp:631`). |
| **HeatmapTwapStreamer** | `test_heatmap_twap_streamer.cpp` (232, `HeatmapTwapStreamerTests`); `test_heatmap_history_fixture.cpp` (200) plus `HeatmapHistoryFixture.hpp` (189, `HeatmapHistoryFixtureTests`); `checkRecorderRelease()` at `test_server_feed_admission.cpp:478-…`, which swaps in `HeatmapTwapStreamer`. |
| **Legacy recording view wire / LiveBuilder** | `tests/marketdata/test_recording_history_wire.cpp` (195, `RecordingHistoryWireTests`; `parseView`/`buildLive` used at `SentinelStreamServer.cpp:959-985`); the `RecordingLive.*` LiveBuilder cases in `test_recording_live.cpp:28-570,998-1042` (about 23 of 36 cases; keep `RecordingRawTail.*`); `LiveClient.RecorderReplayMatchesLiveBuilderAndAnalyticalTwapAtOneAndFiveMinutes` (`test_heatmap_live_client.cpp:1243`); the LiveBuilder check at `test_book_recorder.cpp:609`; `recording_live_bench.cpp` (205, built and never run). |
| **TickBinaryLogger** (server use) | `ShadowTest.StalledAndFailingShadowDoesNotDelayServerPrimary` (`test_shadow.cpp:616-…`, builds a full `SentinelServerApp` with the primary recorder and has a 1.5 s stall sleep). Per the D-b packet §0.11, `BacktestCore.TickLoggerWritesAggressorBasisInVersionedFile`/`TickBinaryTradeEventSourceParsesTradeFiles` (`test_backtest_core.cpp:288,337`) stay while the class stays. |
| **MarketDataCoreEngine use in the server** | `ServerFeedAdmissionSource.*` (`:381`), `ServerFeedAlerts.A1b…` (`:466`), the `ServerFeedAdmissionTest` cap, metrics, GUI-switch and refusal cases (`:218-379`; the packet says "rewrite on roller admission"), `ServerFeedConfig.DefaultOverrideAndInvalidCap`/`RootKeysSurvivePartialMdcOverride` (`:432,445`, `mdc.max_connections`), `RecordingServerStop.SelfInvalidationIsHandedToTheMainThread` (`test_recording_server_stop.cpp:356`), the `ServerMetricsTests` `sentinel_mdc_connected` assertions, and the `ServerStartup` `mdcHost=` log check. The engine itself stays (capture uses it), so `MarketDataEngine{Reconnect,ConnectTimeout}Tests` stay. |
| **Primary BookRecorder path** | `FeedsTest.RecorderSelfInvalidationResnapshotsAndRecordingResumes` and `RecorderInvalidationKeepsPeerFullyObserved` (`test_market_data_engine_reconnect.cpp:307-415`, the `RecorderFeed` wiring engine to recorder); `ReleasedGuiFeedFinalizesPartialAndReacquiresFresh` and `MetadataGateRecordsNativeSnapshot…` (`test_server_feed_admission.cpp:563,596`); `RollerLiveFixture.ControlledTickScheduleStaysWithinBands` (`test_live_fixture.cpp:64`; "legacy" live recorder vs roller parity, now moot); the `ShadowTest` primary-recorder case above. `BookRecorderTests` itself stays (the roller writes through `BookRecorder`). |
| **Disposable storage probe** (`CMakeLists.txt:197` "Disposable storage research harness") | `storage_probe/test_encoders.cpp`, `test_cli.cpp` (`StorageProbeTests`, `StorageProbeCliTests`; the CLI test spawns the probe and polls up to 15 s), about 900 lines in total. |

Total is about 3.3-3.6k lines and 9-10 ctest entries.

---

## 4. Runtime hogs

### 4a. Mechanisms
- **Serial ctest** (section 0.1). Each binary is one entry, so even `-j` parallelism is coarse.
- **Real servers and processes per test:** `SentinelStreamServer` is constructed in `test_recording_server_stop.cpp` (6 sites, including start/stop loops under client churn, `:768`), `test_heatmap_chunk_wire.cpp` (fixture-wide, with restarts at `:1042,1098`), `test_server_feed_admission.cpp` (fixture-wide) and `test_shadow.cpp`. `QProcess` children: `test_capture_app.cpp` (24 refs, real capture with SIGTERM), `test_backtest_core.cpp` (10, CLI), `test_server_startup.cpp` (real `sentinel_server`, 4 launches), `test_lab_output_guard.cpp` (3 `sentinel_lab --synthetic 20000` runs, each allowed `waitForFinished(90'000)`, `:98-112`), `test_research_ui.cpp`, and `storage_probe/test_cli.cpp`.
- **Fixed sleeps** (approximate sums of literal waits): `test_shadow.cpp` about 7 s plus ≥2 s benchmark (`:504-1642`, including 1500 ms at `:628` and `:1432`); `test_heatmap_settings_ui.cpp` about 2.5 s (`qWait(1100)` `:1359`, 600 ms ×2); `test_heatmap_chunk_wire.cpp` about 2.2 s (`:562` 1500 ms, `:1093` 650 ms); `test_sentinel_stream_client_write.cpp` about 1.9 s (`:335` 700 ms, `:342` 1200 ms); `test_market_health.cpp` about 1.6 s (`:310` 1100 ms); `test_dom.cpp` about 1.3 s; `test_chart_ui.cpp` 42 `qWait`s of 10-100 ms (about 1-2 s); `test_heatmap_gpu_binner.cpp` 490 ms (`:785,792`) plus 10 ms polls up to 5 s; `test_research_ui.cpp` about 0.8 s. Fixed sleeps add up to roughly 25-30 s in total. That is real, but it is not the dominant cost.
- **Polling caps:** `eventually()` (8 s cap, 5 ms poll) is called 105× in `test_shadow.cpp:59`. The roller runs with a production 1 s block interval, so many waits really take about 1 s each.
- **Large fixtures:** `CaptureTest.InventoryOver100000FilesHasBoundedBuffersAtEveryScope` (`test_raw_capture.cpp:1100-1125`, 100k hard links, its own ctest entry with TIMEOUT 180); `HeatmapModelTiming` with 5.9M entries; gpu_binner 200k/100k loops (`:282,303`); the UgrGpu fixture loads a 4 h synthetic history per case, and `pump(30'000, …)` waits up to 30 s per settle; three `UgrInput` cases load the full `DepthChartView.qml` (`:4154,4305,4375`) without a GPU gate.

### 4b. ctest properties (CMakeLists)
No `RUN_SERIAL`, `RESOURCE_LOCK`, `PROCESSORS` or `COST` anywhere. TIMEOUTs: 300 s for `UgrGpuTests`, `UgrGpuServiceTeardownGuardMalloc`, `HeatmapSettingsUiTests`, `ChartUiTests`; 180 s for `CaptureVerifierScaleTests`, `CaptureApplicationTests`, `HeatmapChunkWireTests`; 120 s for `RollerTests`, `ShadowRollerTests`, `RollerCorruptBlockTests`, `RecordingServerStopTests`; 60 s for `RawCaptureTests`, `CaptureFanoutTests`, `MarketDataEngineConnectTimeoutTests`, `RecordingLiveTests`, `ServerMetricsTests`, `ServerFeedAdmissionTests`, `DomTests`, `WatchRailScreenerTests`; 45 s for `ServerStartupTests`, `StorageProbeCliTests`, `HeatmapSourceControllerTests`, `FontWidgetTests`, `MarketHealthTests`; 30 s/15 s for the rest. The servermodel comment (`CMakeLists.txt:157-158`) says these timeouts are about 10× normal runtime, so they hint at relative cost. Unlabelled entries: `RollerLiveFixtureTests`, `PriceLadderTests`. Existing labels are by area (`render`, `heatmap`, `ui`, `servermodel`, …), none by cost or kind.

### 4c. Slowest-looking 15 (estimated, descending)
1. **UgrGpuTests**: 76 RHI-scene cases, 4 h fixture each, settles up to 30 s, 3 full QML loads; TIMEOUT 300. Likely the single biggest test cost on the Mac.
2. **ChartUiTests**: 42 widget/QML cases, about 42 qWaits, 9 GPU cases; TIMEOUT 300.
3. **HeatmapSettingsUiTests**: 43 cases, about 2.5 s fixed waits, 9 GPU cases; TIMEOUT 300.
4. **ShadowRollerTests**: 41 cases, real fan-out and writers, 1 s block interval, about 9 s fixed sleeps, links `SentinelServerApp.cpp`; TIMEOUT 120.
5. **CaptureApplicationTests**: real capture child processes, SIGTERM, reconnect; TIMEOUT 180.
6. **HeatmapChunkWireTests**: real server, restarts, 2.2 s sleeps; TIMEOUT 180.
7. **CaptureVerifierScaleTests**: 100k-file inventory; TIMEOUT 180.
8. **RecordingServerStopTests**: 6 real-server lifecycles, churn loops; TIMEOUT 120.
9. **LabOutputGuardTests**: 3 `sentinel_lab --synthetic 20000` child runs, no TIMEOUT.
10. **RollerTests / RollerCorruptBlockTests**: TIMEOUT 120 each.
11. **RawCaptureTests**: 91 cases, 100k/150k loops, RSS sampling; TIMEOUT 60.
12. **HeatmapTileNodeTests + HeatmapGpuBinnerTests** (+3 Guard Malloc re-runs on Apple; Guard Malloc is 10-100× slower per allocation).
13. **BacktestCoreTests**: 10 CLI child processes.
14. **ServerStartupTests**: real `sentinel_server` launched 4×, plus **StorageProbeCliTests** with up to 15 s.
15. **DomTests / MarketHealthTests / SentinelStreamClientWriteTests**: second-scale fixed sleeps.

**How to confirm in one minute:** every land leaves `build/mac-clang/Testing/Temporary/CTestCostData.txt` and `LastTest.log` in the worktree, plus `.land-ctest.log` with per-test seconds. Reading the last land's file replaces this estimate with measured times. ctest also uses that cost file to schedule `-j` runs.

Build side: about 96 test executables, many linking `sentinel_gui_lib`+`sentinel_lab_lib`+Qt Quick, and `test_shadow_roller` recompiles `SentinelServerApp.cpp`. At `-j 6` on 16 GB the link steps are a large share of a land. Deleting about 10 entries (section 3) and merging tiny core/servermodel binaries both cut link work.

---

## 5. Process gunk (counts per file)

"Review" means comments of the form `Review fix N`, `Review round N`, `review blocker/major/minor`, `finding N`, `rN`. "Slice" means `S5c`/`S6d`/`S8a`/`D-b1`/`slice A` in comments. "Names" means test names that carry acceptance or slice tags.

| File | Review | Slice | Agent names | Tagged test names |
|---|---:|---:|---:|---:|
| `render/test_ugr_gpu.cpp` | 32 | 10 | 4 ("Astra's case/counterexample", `:2605,2916,2972,3135`) | 0 |
| `render/test_heatmap_source_controller.cpp` | 7 | 6 | 0 | 0 |
| `roller/test_shadow.cpp` | 6 | ~12 (all tags) | 0 | 0 |
| `render/test_ChartRaster.cpp` | 5 | ~12 | 1 ("Astra's case", `:589`) | **16** (`A1_…A8_`, `A3…`) |
| `render/test_heatmap_tile_node.cpp` | 4 | 6 | 0 | 0 |
| `render/test_heatmap_live_node.cpp` | 3 | 4 | 0 | 0 |
| `render/test_chart_ui.cpp` | 0 | 6 | 0 | 0 |
| `render/test_heatmap_settings_ui.cpp` | 0 | 5 | 0 | 0 |
| `render/test_heatmap_plumbing.cpp` | 1 | 5 | 0 | 0 |
| `trading/test_backtest_core.cpp` | 0 | 1 | 1 (`:598` "`lt-claude/roller-db1`") | **6** (`B1…B7`) |
| `servermodel/test_book_recorder.cpp` | 0 | 0 | 2 ("Fable review 2026-10-01", `:644,696`) | 0 |
| `render/test_lab_item.cpp`, `test_CandleDataSource.cpp`, `roller/test_roller.cpp`, `agentapi/test_AgentApiInput.cpp` | 1 each | 1-2 | 0 | 0 |
| `render/test_lab_output_guard.cpp` | 0 | 0 | 0 | 1 (`S5Bench…`) |
| `render/CMakeLists.txt` | 1 ("Review blocker 1", `:192`) | ~15 | 0 | 0 |
| `marketdata/test_protocol_validation.cpp` | 0 | 0 | 0 | 0 (11 FM/INV refs; these are fine) |
| `tests/README.md` | | | 1 ("thanks Codex!", `:48`); stale, still says "3 test suites" | |
| `roller/check_mutations.py` | | | 1 (`lt-astra/roller-a`) | |

Totals: about 60 review-round comments, about 135 slice tags, about 10 agent-name mentions, and 23 tag-prefixed test names. FM-/INV- references (33) are durable IDs, so keep them. Fix: rewrite the comment as the rule it protects (for example "Review round 2, fix 3: a tick proposal…" becomes "A tick proposal…"), and rename `A3_TheStepBand…` to `TheStepBand…`.

---

## 6. Test files over 1,500 lines

| File | Lines | Cases | Split? |
|---|---:|---:|---|
| `render/test_ugr_gpu.cpp` | 4,480 | 100 | **Yes.** It is already 7 suites. Split into `test_ugr_camera_cpu.cpp` (UgrZoomCpu, UgrWheelCpu, UgrPlacementCpu, UgrCandleCpu, about 24 cases plus any camera cases moved off the GPU fixture), `test_ugr_gpu.cpp` (render-path UgrGpu only), `test_ugr_glide.cpp` (UgrGlide, 9), `test_ugr_input_qml.cpp` (UgrInput, 3 full QML loads), and move `LocalChunkTransportFaults` to `test_chunk_fetcher.cpp`. Separate binaries also let ctest run the CPU parts in parallel and off-GPU. |
| `roller/test_shadow.cpp` | 1,970 | 41 | Yes: `ShadowConfig`/`ShadowStorage` (pure, fast) vs `ShadowTest` (threads, fan-out), and the `SentinelServerApp` cases into their own binary so only that one recompiles the app file. |
| `capture/test_raw_capture.cpp` | 1,952 | 91 | Moderate: writer/reader/verifier vs `CaptureQueuePool`/`TradeWindow`/`DecimalGrid` (pure). The scale case already runs separately. |
| `render/test_chart_ui.cpp` | 1,850 | 42 | Yes: `ChartToolbar` (16, widgets only) vs `ChartControls`/`ChartLabels` (QML/GPU). |
| `render/test_heatmap_source_controller.cpp` | 1,773 | 54 | Single cohesive fixture. Splitting buys little; trim review comments instead. |
| `render/test_heatmap_settings_ui.cpp` | 1,719 | 43 | Yes: it holds 9 unrelated suites. Move `CandleStyleTest`, `PixelAxisGeometry`, `TpoPixelGeometry` (pure) and `HeatmapSettingsModelTest` into a fast binary; keep the dialog and `HeatmapChartUi` (GPU) separate. |

`servermodel/test_hmc2_store.cpp` (1,403) and `render/test_heatmap_tile_node.cpp` (1,348) are close to the threshold; leave them.

---

## 7. Recommendations

### 7a. Make the landing gate fast (largest win, no test deleted)
1. **Run ctest in parallel at land:** `ctest --test-dir build/mac-clang --output-on-failure --timeout 600 -j 4` (or a `testPresets` entry with `"execution": {"jobs": 4, "timeout": 600}`, used by `agent-worktree.sh`). To keep the Mac safe:
   - `set_tests_properties(<all GPU/QML entries> PROPERTIES RESOURCE_LOCK gpu)`, so only one Metal/QQuick test runs at a time;
   - `RESOURCE_LOCK server` or `RUN_SERIAL` for the real-process entries (CaptureApplication, ServerStartup, LabOutputGuard, RecordingServerStop, HeatmapChunkWire), if port or CPU contention appears;
   - `PROCESSORS 2` on the thread-heavy roller and capture entries.

   Expected saving: if the test phase is serial today, `-j 4` with one GPU lane typically cuts it to the GPU lane's length plus slack, about **40-60 % off the ctest phase**. Measure with `CTestCostData.txt`.
2. **Use measured costs:** keep `CTestCostData.txt` between lands (a shared `Testing/` directory or copy-forward), so ctest starts the long entries first.
3. **Fail GPU skips on the Mac gate:** add `SENTINEL_REQUIRE_GPU=1` in the land environment and make `gpuUnavailableReason()` callers `FAIL()` when it is set, or set `SKIP_REGULAR_EXPRESSION "GPU case skipped"` so ctest reports them as *Skipped*, not *Passed*. This makes the AGENTS.md:86 rule ("read the output for `GPU case skipped`") mechanical.

### 7b. Labels for a fast agent loop
Add one cost label and one kind label to every entry (only the two unlabelled entries lack any label today):
- `fast`: pure logic with no process, server, QML or GPU, expected under 5 s (about 55 of 96 entries: core, marketdata parse/dispatcher/protocol, servermodel codecs/model/tiles/span/tick, render math such as AxisLayout/PriceAxis/TimeAxis/ChartRaster/CandleSeries/TpoProfile/RowGrouping, trading core/algo, agentapi codec).
- `slow`: anything with a TIMEOUT of 60 s or more (the 19 listed in 4b).
- `gpu`: the 8 files in section 2 plus the Guard Malloc entries.
- `integration`: real server or child process (section 4a list).
- `lint`: the source-text and config guards from 1b, folded into one binary.

Dev loop for agents: `ctest -L fast -j 6` (seconds to about a minute), plus `-R <targeted suite>`. Writers already stop at targeted tests (AGENTS.md:77). Conductor land: `ctest -j 4` full, with `-L gpu` serialized through the resource lock. Optionally run `-L slow|integration` nightly instead of on every land, which needs owner approval since it changes the land gate.

### 7c. Delete (about 4k lines, about 12 ctest entries, no coverage loss)
- With the deletion slices (section 3b): `test_heatmap_column_store.cpp`, `test_hmcol_format.cpp` (after moving CRC32), `hmcol_seed.cpp`, `test_heatmap_twap_streamer.cpp`, `test_heatmap_history_fixture.cpp` + `HeatmapHistoryFixture.hpp`, `test_recording_history_wire.cpp`, the LiveBuilder half of `test_recording_live.cpp`, `recording_live_bench.cpp`, the engine-bound `ServerFeedAdmission*` cases, and the two `FeedsTest.Recorder*` cases.
- Now: `AgentHostModeSources.DockRouteUsesHostAwarePersistencePolicy`, `HeatmapPlumbing.RetiredRendererAndSourceYamlKeysAreIgnored`, the S8a absence assertions, `AlgoEngineTest.TickOnWrongSymbolDoesNothing` (or give it a real assertion), the 5 GPU cases in `test_lab_item.cpp` that duplicate UgrGpu, and `StorageProbe*` together with the disposable harness.
- Make opt-in: `HeatmapModelTiming.*`, `ShadowTest.HandoffLatencyBenchmark` (≥2 s each run), and `CaptureVerifierScaleTests` (move to `slow`).

### 7d. Merge or speed up
- Move camera and viewport assertions off the `UgrGpu` fixture onto the existing CPU fixtures. That turns as many as about 40 skip-prone GPU cases into fast CPU cases that also run on Linux and Codex.
- Pick one owner per server-startup scenario (TLS and bind: keep the in-process `StreamServer*` cases and keep `ServerStartup` to a single smoke launch).
- Replace the fixed sleeps in `test_shadow.cpp`, `test_heatmap_settings_ui.cpp:1359-1432`, `test_market_health.cpp:310`, `test_sentinel_stream_client_write.cpp:335-342` and `test_heatmap_chunk_wire.cpp:562,1093` with injected clocks or `QTRY_*`/`eventually`. Give the roller's block interval a test override instead of the production 1 s. Expected saving: about 20-30 s of fixed waiting, plus less flakiness.
- Lower `LabOutputGuardTests`' child runs (`--synthetic 20000`) to the smallest input that reaches the guard. The guard refuses before rendering, so `--synthetic 1` should do.
- Merge the many tiny single-file core and servermodel test executables per directory (for example one `test_servermodel_fast` binary). This cuts about 20-30 link steps per build at `-j 6`. Keep slow/GPU/integration as separate binaries so labels and parallelism still work.
- Strip review-round, slice and agent text from comments and test names (section 5) during the file splits in section 6.

### 7e. Expected overall effect (estimates, to be confirmed from `CTestCostData.txt`)
| Change | Effect |
|---|---|
| `ctest -j 4` plus GPU resource lock | ctest phase about 40-60 % shorter |
| Agent loop on `-L fast` | about 1 min instead of the full suite for writers |
| Section 3b and 7c deletions | about 4k test lines, about 12 ctest entries, fewer link steps |
| Sleeps, benchmarks and scale case out of the default run | about 30-60 s serial time |
| UgrGpu camera cases to CPU, split binaries | GPU lane shorter; about 40 cases stop skipping off-Mac |
