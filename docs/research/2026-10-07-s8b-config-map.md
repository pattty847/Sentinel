# S8b prep: server heatmap.* config readers and TickBinaryLogger readers (2026-10-07)

Read-only Codex gpt-6.1-sol investigation for S8b and D-b2, against main @ eff1388. Verbatim packet below.

**S8b cannot delete the entire server `heatmap` section safely.** Three stored keys still control live behavior: the timeframe list, active timeframe, and grid width. Two wire availability lists also derive from them. Read-only investigation on `main`; no edits, builds, tests, or services run.

**Evidence notation.** References below use these file abbreviations; numbers are current source lines.

- **D** = [libs/core/config/ConfigTypes.hpp](../../libs/core/config/ConfigTypes.hpp:13)
- **L** = [libs/core/ConfigLoader.cpp](../../libs/core/ConfigLoader.cpp:93)
- **T** = [libs/core/servermodel/HeatmapTwapStreamer.cpp](../../libs/core/servermodel/HeatmapTwapStreamer.cpp:98)
- **M** = [libs/core/servermodel/ServerDataModel.cpp](../../libs/core/servermodel/ServerDataModel.cpp:98)
- **S** = [libs/core/protocol/SentinelStreamServer.cpp](../../libs/core/protocol/SentinelStreamServer.cpp:133)
- **P** = [libs/core/protocol/SentinelStreamClientParseHelpers.cpp](../../libs/core/protocol/SentinelStreamClientParseHelpers.cpp:27)
- **R** = [libs/core/protocol/RecordingHistoryWire.hpp](../../libs/core/protocol/RecordingHistoryWire.hpp:17)

**Complete server-key inventory.** Keys are relative to `heatmap`; defaults are C++ defaults from D:14–44. Every row has its loader location. **Legacy** means the functional reader disappears in S8b; **live** means migration is required. Wire serialization/parsing is listed separately from functional use.

| YAML key → member; default | Loader | Functional server readers | Wire writer → client parser |
|---|---|---|---|
| `timeframes` / `timeframes_ms` → `timeframesMs`; `[1000,60000,300000,900000,3600000,14400000,86400000]` | L:127–135 | Legacy T:106; **live M:102,119** candles/retention; **live S:1198–1200** candle-history allowlist; **live R:19–21** retained recording capability | S:136,139,143–144 → P:32–39; also S:186 → P:92,101–105 |
| `timeframe` → `activeTimeframeMs`; `0` | L:108 | Legacy T:124–125; **live M:118** retention | S:137–138,167–168; derived availability S:140–145 → P:53–54,69–75 |
| `grid_width` → `gridWidth`; `5120` | L:105 | Legacy T:111–112; **live M:124** retention | S:152 → P:52,55 |
| `grid_height` → `gridHeight`; `2048` | L:106 | Legacy T:114–115 | S:153 → P:53,56 |
| `tick_size` → `tickSize`; `0.0` | L:107 | Legacy T:117–118 | S:154 → P:57 |
| `recenter_delta` / fallback `recenter` → `recenterDelta`; `0.01` | L:109–110 | Legacy T:130–131 | S:155 → P:58 |
| `band_fast` → `bandFast`; `0.15` | L:112 | Legacy T:133–134 | S:156 → P:59 |
| `band_medium` → `bandMedium`; `0.25` | L:113 | Legacy T:136–137 | S:157 → P:60 |
| `band_slow` → `bandSlow`; `0.35` | L:114 | Legacy T:139–140 | S:158 → P:61 |
| `intensity_mode` → `intensityMode`; `"log"` | L:115 | Legacy T:178 | S:159 → P:62 |
| `intensity_max_mode` → `intensityMaxMode`; `"running"` | L:116 | Legacy T:188 | S:160 → P:63 |
| `intensity_max_decay` → `intensityMaxDecay`; `0.995` | L:117 | Legacy T:192–193 | S:161 → P:64 |
| `intensity_log_scale` → `intensityLogScale`; `1000.0` | L:118 | Legacy T:195–196 | S:162 → P:65 |
| `intensity_power` → `intensityPower`; `0.4` | L:119 | Legacy T:198–199 | S:163 → P:66 |
| `intensity_floor` → `intensityFloor`; `0.001` | L:120 | Legacy T:201–202 | S:164 → P:67 |
| `debug_slice_log` → `debugSliceLog`; `false` | L:121 | **No functional reader**, including in T | S:165 → P:68 |
| `persistence_enabled` → `persistenceEnabled`; `false` | L:122 | Legacy T:148 | None |
| `persistence_dir` → `persistenceDir`; `"data/heatmap"` | L:123 | Legacy T:152,154,171,317, including diagnostics | None |
| `persistence_fsync_every_n_records` → `persistenceFsyncEveryNRecords`; `1` | L:124 | Legacy T:150 → column-store config | None |
| `persistence_fsync_every_ms` → `persistenceFsyncEveryMs`; `1000` | L:125 | Legacy T:151 → column-store config | None |
| `persistence_retention_days` → `persistenceRetentionDays`; `0` | L:126 | Legacy T:158,161–165 → column-store retention/diagnostics | None |
| **Derived, not a YAML key:** `servedTimeframesMs`; empty | No loader; D:20–21 | S:140–145 calculates a local list; does **not** read `cfg.heatmap.servedTimeframesMs` | S:151 → P:54,70–75 |

L:98–101 accepts both `server.heatmap` and root `heatmap`. L:73–89 filters positive timeframes, sorts and deduplicates; `timeframes` takes precedence over `timeframes_ms`. Current checked-in [server_config.yaml](../../config/server_config.yaml:4) has the section at **lines 4–15**, with active timeframe `60000`, width `8192`, tick `5`, and persistence enabled.

**Live dependency chains and client readers.**

- **Candles:** M:102 constructs `TimeframeAggregator` from the list; M:588–589 feeds trades. S:1198–1200 independently checks the list for REST candle history. Preserve both readers.
- **Footprint/TPO/VP:** M:118–125 computes `clamp(2 × max(1000, activeTf, configuredTfs) × max(1024, width), 300000, 86400000)`. M:598–600 prunes retained trades using that result.
- **TradeOverlayPublisher is indirect:** S:545–564 collects those retained trades, obtains `retainedFromMs`, and calls the publisher. Its grid comes from `tradeOverlays` at S:411, not `heatmap`; [TradeOverlayPublisher.cpp:75](../../libs/core/servermodel/TradeOverlayPublisher.cpp:75) defines footprint/TPO/VP trade windows and :87 uses retention for candle fallback.
- **Configuration delivery:** [SentinelStreamClient.cpp:861](../../libs/core/protocol/SentinelStreamClient.cpp:861) validates/parses/emits; [RemoteGridDataSource.cpp:420](../../libs/gui/datasources/RemoteGridDataSource.cpp:420) copies the DTO into `GuiConfigStore`.
- **Timeframe list:** [MainWindowGpu.cpp:1320](../../libs/gui/MainWindowGpu.cpp:1320) uses its first element as fallback; [UnifiedGridRenderer.cpp:1315](../../libs/gui/UnifiedGridRenderer.cpp:1315) does likewise; [GuiConfigStore.cpp:59](../../libs/gui/config/GuiConfigStore.cpp:59) logs its size; [AgentApiCodec.cpp:507](../../libs/gui/mainwindow/AgentApiCodec.cpp:507) exposes it when advertised.
- **Active timeframe:** MainWindowGpu.cpp:1319; UnifiedGridRenderer.cpp:1314–1318; [UnifiedGridRenderer.Init.cpp:20](../../libs/gui/UnifiedGridRenderer.Init.cpp:20); GuiConfigStore.cpp:58; AgentApiCodec.cpp:517. These remain live.
- **Served timeframe list:** MainWindowGpu.cpp:214,1327 → [TopToolbar.cpp:1025](../../libs/gui/widgets/TopToolbar.cpp:1025), which enables/disables choices and may select the first available; AgentApiCodec.cpp:512–514 exposes it. **Empty means all toolbar choices enabled** at TopToolbar.cpp:1034–1035.
- **Grid dimensions:** GuiConfigStore.cpp:56–57 logs them; AgentApiCodec.cpp:518–519 exposes them when advertised. Live diagnostics, with no remaining renderer use of these server dimensions.
- **Other transmitted keys:** P:57–68 parses tick/recenter/bands/intensity/debug, but there are **no downstream client functional readers**. Persistence keys are not transmitted.
- **Recording capability:** R:19–24 derives `recording.timeframes_ms` from the configured list; P:92,101–105 parses it. No downstream read of `ServerConfig.recording.timeframesMs` was found.
- **Chunk/roller:** no direct `ServerHeatmapConfig` reader found. Chunk setup uses the model’s recording root at S:1855–1859; [SentinelStreamClientTransport.cpp:79](../../libs/core/protocol/SentinelStreamClientTransport.cpp:79) consumes separate chunk availability. [RollCli.cpp:47](../../libs/core/roller/RollCli.cpp:47) loads shared config but reads recording/roller paths, not heatmap keys.

**Minimal proposed migration — three stored keys, unchanged behavior.**

| Old key | Neutral destination | Preserve |
|---|---|---|
| `heatmap.timeframes[_ms]` | `chart.timeframes_ms` / `ServerChartConfig::timeframesMs` | Exact default list; loader normalization; candles, history allowlist, recording-capability filter |
| `heatmap.timeframe` | `chart.default_timeframe_ms` / `defaultTimeframeMs` | Default `0`, checked-in `60000`, first-configured fallback, retention contribution |
| `heatmap.grid_width` | `trade_overlays.retention_columns` / `retentionColumns` | Default `5120`, checked-in `8192`, exact M:118–125 retention formula |

Do **not** substitute `trade_overlays.grid_width` (`512` default): that controls overlay geometry and would change retention for custom configurations. Do not replace the formula with a fixed one-day retention merely because current defaults reach its upper clamp.

Wire: advertise neutral `chart.timeframes_ms`, `chart.default_timeframe_ms`, and derived `chart.served_timeframes_ms`; preserve S:140–145’s existing derivation during this migration. Keep `recording.timeframes_ms` and R’s current filter. Retention columns need transmission only if preserving their diagnostic visibility; `grid_height` and the remaining legacy fields can disappear.

GUI: migrate P’s DTO/presence markers, MainWindowGpu, renderer initialization/application, toolbar input, logging and Agent API reporting together. Preserve absent-capability handling. Final removal of old wire fields should coordinate server/client versions: [SentinelStreamProtocol.hpp:10](../../libs/core/protocol/SentinelStreamProtocol.hpp:10) currently declares schema `1`, and the client checks it at :861–865. Temporary dual fields/fallbacks can support a staged migration. Replacing legacy served-timeframe derivation with chunk availability is a separate behavior decision.

**Existing tests to carry into the packet — FM-201.**

- `SentinelStreamClientParseHelperTests`: `ParseServerConfigMapsFields`, [test_sentinel_stream_client_parse_helpers.cpp:4](../../tests/marketdata/test_sentinel_stream_client_parse_helpers.cpp:4); `MissingServedTimeframesMeansUnknown`, :39.
- `AgentApiCodecTests`: `EnvelopeAndUnknowns`, [test_AgentApiCodec.cpp:219](../../tests/agentapi/test_AgentApiCodec.cpp:219), assertions :235–237; `AdvertisedCapabilitiesOnly`, :311–323.
- `RecordingHistoryWireTests.StatusErrorsAndCapabilities`: [test_recording_history_wire.cpp:90](../../tests/marketdata/test_recording_history_wire.cpp:90), list/filter assertions :110–119. **Relocate the retained capability test before deleting the legacy suite.**
- `RecordingServerStopTests`: `CandleHistoryPaging.OneSecondPagesReachOlderRetainedBars`, [test_recording_server_stop.cpp:532](../../tests/servermodel/test_recording_server_stop.cpp:532), config :544; `StalledCandleFetchIsJoinedBeforeServerDestruction`, :449, config :456.
- `TradeOverlayTests.AdvertisedDefaultsAreIndependentOfHeatmapConfig`: [test_TradeOverlay.cpp:151](../../tests/render/test_TradeOverlay.cpp:151). Retained-trade snapshot coverage begins :162.
- `ChartUiTests`: toolbar controls at [test_chart_ui.cpp:307](../../tests/render/test_chart_ui.cpp:307); served-list fixture :806,829 is **opt-in**, not an ordinary automatic config assertion.
- Legacy pins: `HeatmapTwapStreamerTests` [test_heatmap_twap_streamer.cpp:67](../../tests/servermodel/test_heatmap_twap_streamer.cpp:67); `HeatmapHistoryFixtureTests` [test_heatmap_history_fixture.cpp:73](../../tests/servermodel/test_heatmap_history_fixture.cpp:73); `ServerFeedAdmissionTests` legacy setup [test_server_feed_admission.cpp:483](../../tests/servermodel/test_server_feed_admission.cpp:483), release test :631.
- Compile/fixture references requiring cleanup: `LiveBookTickTests` :21; `ServerMetricsTests` :49; `HeatmapChunkWireTests` :295; `ShadowRollerTests` :637,1830; `ServerStartupTests` YAML :42, plus the persistence assignments throughout `test_recording_server_stop.cpp`.
- Shared-loader coverage also includes `test_server_feed_admission.cpp:398,423,432,445`, `test_shadow.cpp:1017,1933`, and [test_book_recorder.cpp:552](../../tests/servermodel/test_book_recorder.cpp:552). These cover other sections, not server heatmap-key mapping.
- Gap: no direct YAML test found pinning every server heatmap key or the retention formula. [candle_history_benchmark.py:158](../../tests/servermodel/candle_history_benchmark.py:158) also embeds the old timeframe section.

**TickBinaryLogger / D-b2.**

- Actual file reader: [MarketEventSource.cpp:69](../../libs/core/trading/MarketEventSource.cpp:69), `TickBinaryTradeEventSource`; records :82–127, header/version validation :145–153, recursive `.bin` discovery/sorting :171–190. Accepts versions **1 and 2**, emits timestamp/symbol/price/quantity, skips book records :129–132 and trailing trade-ID bytes :107–110; does not expose side.
- Production consumer: [apps/sentinel-backtest/main.cpp:74](../../apps/sentinel-backtest/main.cpp:74), choosing the binary reader at :85 for non-CSV input.
- GUI consumer: [PaperTradingDock.cpp:372](../../libs/gui/widgets/PaperTradingDock.cpp:372) offers binary-file/directory input and launches that executable at :517.
- Test readers: `BacktestCore.TickBinaryTradeEventSourceParsesTradeFiles`, [test_backtest_core.cpp:279](../../tests/trading/test_backtest_core.cpp:279), checks both versions; `TickLoggerWritesAggressorBasisInVersionedFile`, :325, creates logger :337 and reads raw structs :347–355. No other file decoder found in searched code/scripts.
- Current producer: M:101 creates the logger; M:580–581 writes trades. **No call to `logBookUpdate` exists**; its definition remains [TickBinaryLogger.cpp:60](../../libs/core/servermodel/TickBinaryLogger.cpp:60). The D-b packet’s “every trade and book message” statement is stale.
- D-b2 must either retain logging on the surviving journal-fed trade path, or add journal trades as an `IMarketEventSource` and wire it through CLI/GUI. Keeping only the class/test leaves future backtests without fresh input. Preserve existing `.bin` files, their reader, and `LogFormat` definitions at [TickBinaryLogger.hpp:11](../../libs/core/servermodel/TickBinaryLogger.hpp:11), even if the writer is retired or the format declarations move.

WORKFLOW: Read-only symbol and wire tracing; found three live stored keys, two derived availability lists, uncovered loader/retention coverage gaps, and corrected the stale claim that the model logs book updates.