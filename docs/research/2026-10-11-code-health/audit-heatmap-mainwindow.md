# Audit: GPU heatmap stack, MainWindowGpu, TopToolbar, HeatmapSettingsDialog

Repo: /home/claude/pattty847/sentinel, main @ 180d3b7. Read-only. All paths are relative to the repo root.
I did not compile anything. Where an include is called "likely unused", only a build can confirm it.

## 1. Responsibilities per file (and files doing more than one job)

| File | Lines | Jobs | Verdict |
|---|---|---|---|
| render/heatmap/HeatmapSourceController.cpp/.hpp | 1917/534 | (a) buildSpanSource, 83-128; (b) SpanSourceCache (LRU, build jobs, CPU ledger), 130-565; (c) live composition, 818-1193; (d) controller admission, CPU ceiling and reconcile, 1287-1916 | 3-4 jobs. SpanSourceCache is an independent class that should have its own file. |
| render/heatmap/HeatmapTileNode.cpp/.hpp | 1501/366 | residency/upload 291-417; bins 419-607; retire/report 622-714; draw 716-866; live edge 868-1154; prepare 1157-1447 | One job (the scene-graph node), but large. The live edge is separable. |
| render/heatmap/HeatmapGpuBinner.cpp/.hpp | 871/266 | (a) the active/spare single-source path with refusal and backoff, used only by the self-test, lab Bench and tests; (b) the resident pool, used by the TileNode | Two jobs. Path (a) is mostly test machinery. |
| render/heatmap/HeatmapGpuLayer.cpp/.hpp | 917/318 | controller lifecycle; tick policy 357-458; labels 483-672; telemetry state() 731-794 and metrics() 831-916 | 3 jobs. Telemetry is about 190 lines. |
| MainWindowGpu.cpp/.h | 1928/215 | construction and services; setupUI connects 332-503; Agent API server and widget grab 615-782; screener subprocess 784-899; symbol subscription state machine 912-1070; history requests 1110-1220; connectMarketDataSignals 1303-1551; Agent API snapshots and controls 1682-1910 | God object with about 7 jobs. |
| widgets/TopToolbar.cpp/.hpp | 1141/213 | widget construction; controls menu; overflow/responsive fit; WASD key navigation; tick selector | 4-5 jobs. The fit and overflow logic is separable. |
| widgets/HeatmapSettingsDialog.cpp/.hpp | 889/152 | settings dialog; it also hosts HeatmapGradientEditor and CandlePreview | Mostly one job. The gradient editor could have its own file. |

## 2. Dead code and legacy leftovers

### Dead, safe to delete (behavior-neutral)
- **HeatmapIntensityMaterial and HeatmapIntensityNode** (libs/gui/render/HeatmapIntensityNode.cpp) and **shaders/heatmap_intensity.vert/.frag**: nothing uses them. Only `HeatmapColumnTexture` from that file is live, used at FootprintOverlayRenderer.cpp:146. tests/render/test_heatmap_palette.cpp:71 still checks the intensity shader text.
- **HeatmapGpuBinner**:
  - unused accessors with zero external references: `binnedGrid()`, `binnedSourceId()`, `memoryCap()`
  - dead header includes and forward declarations (each name appears only once): QMatrix4x4, QRectF, QVector, QRhiGraphicsPipeline, QRhiRenderPassDescriptor, QRhiTexture, QRhiSampler
  - stale header comment "Drawing is the caller's"
  - `ComputeParams.paging[1]` (wordsPerEntry) is written at cpp:589, but heatmap_bin.comp never reads `paging.y`
- **heatmap_display.frag**:
  - `style.w` is unused
  - `dims.z` (kClampRows) is always 1 (TileNode 22, 822, 860), so the non-clamp branches are dead: centreCell 69-71 and the `else if` in the coverage loop
- **HeatmapTileNode**: subRect's `tfMs` parameter is always passed 1 (797, 853).
- **HeatmapGpuLayer**:
  - `setCaptureForTest` has zero users, so `capture_` is always null
  - `kMaxLabelRetryMs` has zero users
- **MainWindowGpu.h members** with zero uses in the cpp: `m_positionOverlayLabel`, `m_orderQtyInput`, `m_tradeInputManager`, plus the TradeInputManager.hpp include.
- **MainWindowGpu.cpp includes**:
  - duplicates: QMessageBox (6, 12) and QTabWidget (2, 82)
  - SessionManager.hpp (24) is unused, and it is a core servermodel header included from the GUI
  - likely unused, to be confirmed by a build: QElapsedTimer, QThread, QShortcut, QPushButton, QGroupBox, QVBoxLayout, QHBoxLayout, QSettings, QSGRendererInterface, QFile, QQmlContext
- **MainWindowGpu.cpp setupUI**:
  - `m_symbolInput` and `m_subscribeButton` are assigned twice (340-341 and 382-383)
  - there is an empty `if (m_qquickView) {}` at 379-380
- **TopToolbar**:
  - signal `indicatorsRequested` is never emitted or connected
  - `overflowMenu()` has no external users
  - QSlider and QSpinBox includes are possibly unused

### Test-only machinery in production classes
- HeatmapGpuBinner: setMemoryCap, refusedSourceId, retryBackoff, uploadStep, uploadPending, activeSource, pendingSource, readBackData, and the Refusal/backoff logic. All are used only by test_heatmap_gpu_binner.
- HeatmapCapacity::reportFree (SourceController.hpp:254) is used only by tests.
- HeatmapGpuLayer::setActive(false) is called only from test_MainWindowSymbolLifecycle.cpp:257. Production calls setActive(true) once (UGR.Init.cpp:132). The hpp:60 comment "Active (gpu renderer)" is a renderer-selection leftover.

### Legacy leftovers that need an owner decision (not behavior-neutral)
- **Agent API**: `{"renderer","gpu"},{"activeRenderer","gpu"}` at MainWindowGpu.cpp:1775-1776 ("the fields go in S8b"), and the "renderer" key in HeatmapSettingsStore.cpp:107 and 150-151. Removing them changes the API and the persisted format.
- **TopToolbar hidden adapters**: controlVisibility (888-898) never sets `palette`, `liquidity` or `labelsToggle`, so colorPresetCombo, liquidityModeCombo, labelsButton and their labels are always hidden; the comment at 891-892 confirms it. As a result, `colorPresetSelected` (connected in MainWindow 439-443) cannot be reached by the user. The palette is chosen in the gear menu (HeatmapChartControls.cpp:166-177). test_chart_ui.cpp:260-296 and 1261-1374 assert these widgets are hidden or synced.
- **Hidden settings**:
  - `showBandEdges` (Dialog 556-558): row hidden, lab only
  - `prefetchTiles` (Dialog 639-643: hidden, "not used yet"): round-tripped in HeatmapSettingsStore.cpp:71,99,119,174 and ConfigTypes.hpp:176, but never read by the planner
- **"legacy" naming that is still live**:
  - HeatmapPalette.hpp `legacyDefaultGradients` and `legacyRecordingColor`; the latter is also used by HeatmapLabelLayout.cpp:160
  - TileNode.hpp:196 "legacy default palette"
  - heatmap_display.frag:13 "legacy 512-texel image"
- **Live-tab text mismatch** (user-visible): the dialog note (682, 685) says the server publishes at 1 Hz, but HeatmapSourceController.hpp:359-361 says 2 Hz at the default 500 ms.

## 3. Duplicated logic

**HeatmapSourceController**
- The commonUnits LCM loop appears at 120-125 and 1095-1097, plus a min-finest variant in HeatmapGpuLayer at 345-347 and 884-889.
- `lagLimit` is computed twice, at 950 and 969.
- The sum sparseBytes+imageBytes+resolutionBytes appears at 860, 1100-1101 and 1108.
- The eviction-victim selection loops in reconcile, 1559-1576 and 1582-1599, are nearly identical.
- `gpuBytes()` (1334-1342) and `outstanding()` (1343-1352) are nearly identical.
- The liveWanted_ key commit appears at 1666-1670 and 1673-1676.
- The tier test `Visible || Fallback` appears at 922, 940, 1211, 1376, 1514 and 1702, although isRetained and isGuarded already exist (27-28) and TileNode has `guarded()` (hpp:42).
- steady_clock-to-ms lambdas appear at 580-583, 1172 and 1731.

**HeatmapTileNode**
- The static `fallback = makePalette(legacyDefaultGradients(), {})` appears twice, at 733 and 752.
- `listed = indexed_ && source.listedVersion == indexedVersion_` appears at 299, 327, 349 and 663.
- rowsCover (450-456) re-implements HeatmapBinGrid::gridCovers with different margins.

**HeatmapGpuBinner**
- The buffer spec list is built three times: setSource 225-239, uploadResident 691-703, beginRefill 759-771.
- The paging upload loop appears four times: 302-309, 716-727, 812-823, and refillStep (the same as uploadResident's tail).
- Grid validation is duplicated in bin() 512-514 and binSourceInto 615-618.
- The dispatch blocks at 539-543 and 666-670 are duplicates.
- The `(bytes+15)/16*16` rounding appears at 29, 240, 778 and TileNode 421.

**HeatmapGpuLayer**
- `resolution_ && resolution_->tfMs == tfMs_ ? *resolution_ : snapshot_->resolution` appears four times: 364, 434, 442, 448.
- The snapshot symbol/tf guard is repeated about 7 times.
- chooseTick and predictTickUnits overlap.
- The live-sample percentile code is repeated in state() 735-739 and metrics() 871-875.
- liveAnchorMs and oldestAvailableMs both walk availability.

**MainWindowGpu**
- The tab-select-by-name loop appears at 453-454, 481-482 and 775-776.
- Screenshot directory resolution appears at 513-516 and 640-649.
- The lsof kill command appears at 821-823 and 866-868.
- The stockChart quick-item grab appears in the drawQuick lambda (699-705) and the block at 733-747.
- Layer toggle states are set at 1333-1336, 1341-1344 and 1873-1874.
- The dock struct is built in setupMenuBar (1266-1275) and getDockWidgets (1667-1677).
- Five toggled connects repeat the renderer-null pattern (402-431).
- requestTpoHistoryForSymbol fetches the renderer twice (1148-1164).
- `out["tickUnits"]` at 1795 is redundant.
- The palette preset is synced at 1394-1397 and in HeatmapChartControls.cpp:400.

**Timeframe handling**
- MainWindow timeframeMsFromLabel (140-155) is the inverse of TopToolbar labelForTimeframeMs (27-38) and the combo arrays (174-175).
- The toolbar emits label text that MainWindow parses back into ms, although the combo already stores ms in UserRole.
- primaryField uses magic numbers 0-3 (MainWindow 1080-1081, TopToolbar 144-165).

**TopToolbar internals**
- Combo-to-menu mirroring is written four times: 385-393, 394-402, 403-415 and 554-577.
- The first-enabled-action loop appears at 697-702 and 756-761.
- setLayerToggleStates repeats four blocks (1056-1071).

**Lists duplicated across files**
- Palette presets, in 5 places: TopToolbar 224, HeatmapChartControls 169-170, HeatmapSettingsStore.cpp:11, Dialog:34, HeatmapPalette.cpp:65+.
- TPO session list: Dialog 714-716 and TopToolbar 235-237.
- TPO layout list: Dialog 720-721 and TopToolbar 248-249.
- Candle style list: Dialog 411 and TopToolbar 215.

**Palette shader logic** has 3 live copies plus 1 dead copy:
- HeatmapPalette.cpp:141 (CPU)
- heatmap_display.frag:42
- heatmap_intensity.frag:94 (dead)
- tests/render/shaders/heatmap_display_slice_a.frag, an intentional golden reference, so keep it

## 4. Comment gunk

Count: 39 comments in the 14 target files carry history tags, meaning a slice tag (S5b, S5c, S6a, S6b, S6c, S7b, S8a, S8b), a plan path, a date or "review N", where the comment should state the current fact.
- MainWindowGpu.cpp: 10
- SourceController.hpp: 6
- TileNode.hpp: 5
- GpuLayer.hpp: 5
- TopToolbar.cpp/.hpp: 3 each
- Dialog.hpp: 3
- one each in SourceController.cpp, Binner.cpp, MainWindowGpu.h and Dialog.cpp

On top of these, there are about 8 misplaced or stale comments, listed below. The heatmap .cpp files are otherwise mostly genuine "why" comments.

Worst 15:
1. MainWindowGpu.cpp:1408 says "Trade connection (simplified, assume UnifiedGridRenderer has a slot)". This is speculation left in the code.
2. MainWindowGpu.cpp:1775 says "renderer/activeRenderer: the constant "gpu" since S8a (the fields go in S8b)". It is a migration note on live API fields.
3. MainWindowGpu.cpp:120-125, the chartHealthProvider comment, reads as review narration.
4. HeatmapSourceController.cpp:669-671: the Windows threadCpuNs comment talks about "median of three", which is backoff logic and has nothing to do with this function.
5. HeatmapGpuLayer.cpp:796 says "Refreshes the controller's stats", but it sits above scanWalls, which does something else.
6. HeatmapGpuBinner.hpp says "Drawing is the caller's". The binner stopped drawing long ago.
7. HeatmapGpuLayer.hpp:60 says "Active (gpu renderer)". There is no renderer choice any more.
8. TopToolbar.cpp:296 says "GPU renderer: ... (replaces threshold/sensitivity)". This describes history.
9. HeatmapTileNode.hpp:196 says "nullptr draws the legacy default palette".
10. heatmap_display.frag:13 says "legacy 512-texel image".
11. heatmap_display.frag:36 says "exactly as heatmap_intensity.frag". It refers to a dead shader.
12. MainWindowGpu.cpp:1792 says "Live layer state replaces the S6a placeholders (plan section 5)".
13. HeatmapSourceController.hpp:3 says "(plan docs/research/2026-09-s5-plan.md, slice S5b)", and TileNode.hpp:3 and GpuLayer.hpp:3 have the same kind of plan-path header.
14. TopToolbar.hpp:59 says "(owner request 2026-10-02)".
15. HeatmapTileNode.hpp:92 says "(S6b review 3)", and GpuLayer.hpp:191 says "(S7b review)".

User-visible text with the same problem: Dialog 778 and 781 say "(GPU renderer)", and the Live-tab text at 682/685 gives the wrong rate (1 Hz vs 2 Hz).

## 5. STATUS perf smells in these files

| Item | Verdict | Evidence |
|---|---|---|
| Late cancelled span build can re-add an evictable CPU image (deferred) | **Confirmed**, in SpanSourceCache::request 497-501 | A cancelled build still inserts into the LRU when no key is uploaded. |
| Per-frame heap allocation in TileNode::prepare | **Refuted** for production | prepare reuses order_, covered_, pieces_ and segmentScratch_. Stats vectors use assign/push_back and keep their capacity. captureCells allocates `seen` every frame, but only in tests. |
| liveLatency growth | Minor | Front-erase at 4096 entries (TileNode 1148) is O(n), but runs only on a new live version. A ring buffer would remove it. |
| Transient GPU buffers on re-bin | Confirmed, low impact | binSourceInto creates QRhiBuffers per span bin pass when there is no cache. This happens only on re-bin, not per frame. |
| GpuLayer telemetry allocation | Confirmed, low impact | state() and metrics() allocate vectors at 4 Hz on the GUI thread. |
| HeatmapBinGrid planGrid/gridCovers/mappingFor dead | **Refuted** (STATUS already says false) | They are used by TileNode (mappingFor), lab Bench and tests. |
| Palette half-texel and span-cancel fixes | Landed | Not a current smell. |
| CandlestickBatched.cpp:498 and VolumeProfileRenderer | Out of scope | Not in the target files. |

## 6. Ordered behavior-neutral refactor slices

| # | Slice | Files | Rough delta | Risk | Guarding tests |
|---|---|---|---|---|---|
| 1 | Delete HeatmapIntensityMaterial/Node and heatmap_intensity.vert/.frag. Move HeatmapColumnTexture into its own file. Drop the intensity-shader check from test_heatmap_palette and fix the frag:36 comment. | HeatmapIntensityNode.*, shaders, qrc/CMake, test_heatmap_palette.cpp | about -300 | Low | test_heatmap_palette, FootprintOverlay tests, test_ChartRaster |
| 2 | MainWindow dead members and includes, the duplicate setupUI assignment, the empty if | MainWindowGpu.cpp/.h | about -25 | Trivial; needs a build to confirm includes | test_MainWindowSymbolLifecycle, test_AgentApiDocks |
| 3 | Binner dead includes and forward declarations, unused accessors, the paging.y write. TopToolbar indicatorsRequested and overflowMenu(). GpuLayer setCaptureForTest and kMaxLabelRetryMs. TileNode subRect tfMs parameter. | Binner.hpp/.cpp, TopToolbar.hpp, GpuLayer.*, TileNode.cpp | about -40 | Low | test_heatmap_gpu_binner, test_heatmap_tile_node, test_chart_ui |
| 4 | Shader: remove dims.z non-clamp branches and style.w | heatmap_display.frag, TileNode 22/822/860 | about -15 | Low-medium. The behavior is identical but the compiled output changes, so it needs a native Metal run. | test_heatmap_tile_node (pixel), test_ugr_gpu, slice_a golden |
| 5 | Binner dedupe: one buffer-spec builder, one paging-upload helper, one grid check, one dispatch helper, alignUp16 | HeatmapGpuBinner.cpp, TileNode 421 | about -80 | Medium | test_heatmap_gpu_binner, test_heatmap_tile_node, test_heatmap_live_node |
| 6 | Move SpanSourceCache into HeatmapSpanSourceCache.cpp/.hpp (move only), then extract live composition (818-1193) | HeatmapSourceController.*, CMake | net about 0 (moves about 450 + 375 lines) | Low (move); medium (live extraction) | test_heatmap_source_controller, test_heatmap_live_client, test_heatmap_plumbing |
| 7 | SourceController helpers: commonUnits(), sourceBytes(), a single victim picker, a single bytes-sum for gpuBytes/outstanding, use isRetained in place of the six inline tier tests, a single ms clock helper, compute lagLimit once | HeatmapSourceController.cpp | about -60 | Medium (reconcile ordering) | test_heatmap_source_controller |
| 8 | GpuLayer: move telemetry (state/metrics) into HeatmapGpuLayerTelemetry.cpp; add helpers currentResolution(), snapshotMatches() and liveSampleStats() | HeatmapGpuLayer.* | about -40, with about 190 lines moved | Low | test_heatmap_plumbing, test_heatmap_label_layout, test_AgentApiCodec |
| 9 | TileNode helpers: listed(source), one palette fallback, rowsCover via gridCovers (only if the margins can be matched; otherwise leave it) | HeatmapTileNode.cpp | about -20 | Low | test_heatmap_tile_node, test_heatmap_cell_query |
| 10 | MainWindow helpers: selectSettingsTab, screenshotDir, applyLayerStates, one dock-struct builder, one lsof helper, one quick-grab helper. Then move the screener into its own class (about 120 lines) and the Agent API handlers into MainWindowAgentApi.cpp (about 400 lines moved). | MainWindowGpu.*, new files | about -60, with about 500 lines moved | Medium (Agent API surface) | test_MainWindowSymbolLifecycle, test_AgentApiDocks, test_AgentApiCodec, test_chart_ui |
| 11 | TopToolbar: a combo-to-menu mirror helper, a first-enabled helper, and a loop in setLayerToggleStates. Share one timeframe table, and pass ms through UserRole so timeframeMsFromLabel can go. | TopToolbar.cpp, MainWindowGpu.cpp | about -70 | Medium. Changing the signal signature affects connects and tests. | test_chart_ui, test_heatmap_settings_ui |
| 12 | Shared option lists (palette presets, TPO session, TPO layout, candle style) in one header used by the toolbar, the dialog, the store and HeatmapChartControls | new ChartOptionLists.hpp, 4-5 files | about -30 | Low | test_heatmap_settings_ui, test_chart_ui, test_heatmap_palette |
| 13 | Comment sweep: rewrite the 39 history-tagged comments as current facts, and fix the misplaced comments (SC 669-671, GpuLayer 796, Binner.hpp, MW 1408 and 120-125) | all target files | about -20 | None | build only |
| 14 | **Owner decision (not neutral):** the hidden toolbar adapters and colorPresetSelected; the renderer/activeRenderer JSON fields and the store key (S8b); the prefetchTiles and showBandEdges settings; setActive and the test-only binner refusal path; the Live-tab 1 Hz vs 2 Hz text; renaming legacy* in HeatmapPalette | Toolbar, MW, Store, ConfigTypes, Dialog, Binner, Palette | about -200 | API and persisted format change | test_chart_ui, test_AgentApiCodec, test_heatmap_settings_ui, test_heatmap_gpu_binner |

Suggested order: 1-4 first (pure deletion, little review needed), then 5-9 (dedupe inside the heatmap), then 10-12 (GUI), with 13 alongside any of them and 14 kept separate.
