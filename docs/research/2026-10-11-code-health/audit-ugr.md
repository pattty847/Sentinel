# UnifiedGridRenderer (UGR) audit and split plan

Read-only audit of `main @ 180d3b7`. Scope: `libs/gui/UnifiedGridRenderer.h` (688), `.cpp` (1844), `.Init.cpp` (192), `.Render.cpp` (307), `.Zoom.cpp` (599); 3,630 lines in all. 35 commits touched these files since 2026-10-01 (`git log --since=2026-10-01 -- libs/gui/UnifiedGridRenderer*`), 14 of them titled "review" or "fix round".

Short version: the code is not vibe gunk in the "Actually... wait" sense. There are **no** chain-of-thought comments. The real problems are:
1. one class doing about 14 jobs;
2. a layer of dead public surface left from the pre-GPU "grid" era (resolution presets, volume filter, debug stubs, 6 dead QML files);
3. copy-paste scaffolding that each fix round added again instead of factoring out (the raster-camera setup appears 7 times, zoom span clamps 4 times, layer exclusion 5 times);
4. about 30 comments that cite process (slice and plan names, "review r3", "frozen A2", spec-rule numbers) instead of explaining the code.

One correction to the brief: **coverage blending is not dead.** The rejected approach (STATUS.md:113, "coverage blending rejected and archived") was exact-area coverage *at rest*. The A2 glide uses coverage *during transitions* (`SyncFrame::coverageGamma`, h:270; set at Render.cpp:130; consumed by `HeatmapGpuLayer::prepareFrame`, HeatmapGpuLayer.cpp:461-469; documented in ARCHITECTURE.md:287). It is live and must stay.

---

## 1. Responsibilities map

| # | Job | Where (approx. lines) |
|---|---|---|
| 1 | **Lifecycle and window binding**: ctor, dtor (DataProcessor thread teardown), `bindWindow` (beforeSynchronizing, afterRendering frame ack, screenChanged) | cpp:63-155; Init.cpp:18-136 |
| 2 | **Data-thread wiring**: DataProcessor create/move/start, footprint/TPO/VP column intake | Init.cpp:81-95, 122-125, 138-192 |
| 3 | **QML property surface**: 43 `Q_PROPERTY`, 58 `Q_INVOKABLE`, 32 signals, trivial getters and setters | h:64-112, 322-395, 548-583; cpp:262-325, 1637-1668 |
| 4 | **Layer visibility and mode**: heatmap/footprint/TPO/VP exclusion rules, `primaryField` | cpp:471-494, 702-806 |
| 5 | **Look and config bridges**: TPO config and style, heatmap tone, candle appearance, GPU layer settings, client/server config | cpp:395-469, 648-700, 818-866, 1554-1576 |
| 6 | **Raster camera (whole-pixel mapping)**: `rasterInputs`, `camerasFor`, `rasterCameraNow`, `restCameraNow`, `intendedEndNow`, `drawnTimeWindow`, `rasterPanShift`, frame mapping and publication | Render.cpp:16-41, 95-159; Zoom.cpp:36-150; cpp:889-914, 1179-1202, 1487-1499, 1661-1668 |
| 7 | **Zoom ladder, glide and gesture state machine** | Zoom.cpp:152-517; state h:182-262 |
| 8 | **Input routing**: mouse pan, wheel, native pinch, axis wheel/drag/click entry points, keyboard pan | cpp:1591-1635, 1774-1844; Zoom.cpp:519-599 |
| 9 | **Live placement**: follow-live, return to live, seed from book/trade, cold bootstrap, initial span | cpp:157-177, 916-998, 1035-1098 |
| 10 | **Timeframe switch placement**: Now-column keep, follow-padding solve | cpp:517-646 (one 130-line function) |
| 11 | **Symbol switch and price carry** | cpp:343-393, 1000-1033 |
| 12 | **Auto price scale and fits**: candle-buffer hook, fit time/price windows, `autoPriceFit`, `refitAutoPrice`, `fitView`, `resetView` | cpp:1100-1393, 1455-1485 |
| 13 | **Agent API**: `applyViewportRequest`, control-revision handshake | cpp:1395-1453; h:311-318, 430-441 |
| 14 | **Render-thread paint**: root node, overlays, S7b labels, axis text, trade-bubble frame, profiler | Render.cpp:44-93, 161-307 |
| 15 | **Text atlas and debug strings**: MSDF atlas build (env knobs, /tmp dump), overlay debug text | cpp:33-61, 1501-1552, 1578-1589, 1670-1734 |

---

## 2. Dead code (each item checked with grep across libs/, apps/, tests/ and libs/gui/qml; no generic `QMetaProperty` introspection exists in libs/gui or apps)

### 2a. Declared but never defined or emitted
- `calculateOptimalResolution` and `struct GridResolution` (h:455-459): declared and never defined; no callers.
- Signals `gridResolutionChanged` (h:549) and `priceResolutionChanged` (h:552) are never emitted (0 hits in UGR .cpp files). `currentPriceResolution` (h:67) NOTIFYs a signal that never fires.

### 2b. No callers anywhere
- `getCurrentTimeResolution` (h:452, cpp:1637-1639)
- `setGridResolution` (h:454, cpp:1769-1771; it ignores `timeResMs`)
- `getGridDebugInfo` and `getDetailedGridDebug` (h:461-462, cpp:1670-1676)
- `togglePerformanceOverlay` (empty body, cpp:1772)
- `getCacheHitRate` (returns 0.0, cpp:1734)
- `lastTimeAxisMapping` (h:471)
- `getScreenWidth` and `getScreenHeight` (cpp:1643-1644)
- `currentTimeframe()` (h:322). `getCurrentTimeframe` (h:375) is the one in use, at MainWindowGpu.cpp:215, 1130, 1213, 1329 and 1745.
- `worldToScreen` and `screenToWorld` (h:531-532, cpp:1487-1499): no QML or C++ caller. UI_ARCHITECTURE.md:41 and ARCHITECTURE.md:285 say QML hit tests use them; those docs are stale.
- `autoScrollSmoothEnabled`: property, member, setter and signal (h:69, 158, 326, 554, 679; cpp:278-283). Nothing reads the member.
- `autoScrollPaddingFrac` property and setter (h:68, 325, 553, 678; cpp:270-276): nothing writes it. The member is used (cpp:940, 977, 1040, 1076, 1151), so it is in effect the constant 0.08.
- `m_manualTimeframeSet` and `m_manualTimeframeTimer` (h:130-131): only written (cpp:530-531), never read. This is a copy of DataProcessor's own flag.
- Q_PROPERTYs with no QML or `property()` reader (the C++ getters stay where C++ uses them): `viewState` (h:104), `visibleTimeStart`/`visibleTimeEnd`/`minPrice`/`maxPrice` (h:88-91; QML calls the `get*()` invokables instead, DepthChartView.qml:874-877), `panVisualOffset` (h:97), `effectiveAxisLabelPx` (h:107).

### 2c. Write-only plumbing
- `m_candleStreamGeneration` (h:300) is never incremented. It is loaded at Render.cpp:213 and published as `candleGeneration` (Render.cpp:36), so `CandlestickOverlayItem.cpp:373-388` compares a constant 0.
- `m_footprintStreamGeneration` (h:299) is incremented (Init.cpp:154, cpp:339) and published as `footprintGeneration` (Render.cpp:35), but nothing reads `streamGenerations.footprint` or `footprintGeneration`.
- `m_gpuLabelSerial` (h:281; written at Render.cpp:167) exists only for the test accessor `gpuLabelSerial()` (tests only).

### 2d. Reachable only from dead QML
`ChartToolRail.qml` is never instantiated: its only reference is `libs/gui/CMakeLists.txt:283`. It is the only user of `VolumeFilter.qml`, `PriceResolutionSelector.qml`, `GridResolutionSelector.qml`, `NavigationControls.qml` and `TimeframeSelector.qml` (ChartToolRail.qml:99-111). That is 627 dead QML lines. Through it, these are dead too:
- `minVolumeFilter` property, member, setter and signal (h:66, 115, 323, 551, 671; cpp:262-268). The value affects nothing.
- `setPriceResolution` (cpp:496-506), which forwards to `DataProcessor::setPriceResolution`, a no-op (DataProcessor.cpp:276-278).
- `setGridResolutionPreset` (cpp:508-515). It would set timeframes of 50/100/250 ms, a leftover of the legacy grid.
- `getCurrentPriceResolution` (returns 1.0, cpp:1640-1642).
- Out of UGR scope but the same family: the DataProcessor stubs `getPriceResolution`, `addTimeframe`, `suggestTimeframe` and `getDisplayMode` (DataProcessor.cpp:280-297) have no callers.

### 2e. Live menu items that do nothing (owner decision, not neutral)
`showRenderStrategyOverlay`, `showMemoryCacheOverlay` and `showModeFlagsOverlay` (h:83, 85, 86) are toggled from the chart menu (HeatmapChartControls.cpp:249, 251, 252; MenuBuilder.cpp:154, 178), but no QML reads them (DepthChartView.qml reads only GpuStats, DataPipeline and ViewportMath at :844, :896, :941, :973). The user can toggle three menu items that have no effect.

### 2f. S8a (legacy heatmap) leftovers
- Unused includes:
  - QSG geometry: cpp:20-22 (`QSGFlatColorMaterial`, `QSGGeometry`, `QSGVertexColorMaterial`); h:3-4 (`QSGGeometryNode`, `QSGVertexColorMaterial`).
  - `QtEndian`: cpp:28, Init.cpp:10.
  - Duplicate `<algorithm>`/`<cmath>`: cpp:17-18 and 29-30.
  - Unused forward declaration `class QScreen` (h:56).
- No legacy heatmap logic remains. The tone setters (`heatmapGamma`, etc.) feed the GPU palette (`syncGpuTone`, cpp:854-859) and are live.

### 2g. Defensive noise
`m_viewState` is created in `init()` (Init.cpp:30), which the ctor calls (cpp:72), and is never reset before member destruction. The ~78 `m_viewState` null checks across the five files are therefore dead branches. The same holds for `m_axisTextService` (13 checks; created at Init.cpp:73, before any caller). The `m_gpuLayer` checks (41) are **not** all dead: the dtor resets it (cpp:133) while queued lambdas may still run, so keep those.

### 2h. Rule violation, not dead code
`SENTINEL_DUMP_GLYPH_ATLAS` writes `/tmp/sentinel_msdf_atlas.png` (cpp:1534-1548). AGENTS.md section 4a forbids ad-hoc env gates and side files in /tmp. Deleting it is a candidate (owner).

---

## 3. Duplicated logic

| # | What | Sites | Note |
|---|---|---|---|
| D1 | Snapshot `m_rasterStep` under `m_frameContextMutex` | cpp:591-595, 894-898, 1193-1197; Zoom.cpp:95-99, 109-113, 131-135; Render.cpp:120-124 (7) | One `rasterStepNow()` helper |
| D2 | viewportSnapshot, then dpr, then `rasterInputs`, then `computeRaster` | Zoom.cpp:89-101 and 103-115 (identical except the last line), 117-137; cpp:596-604 (`restAt` lambda), 889-914, 1179-1202 | One `cameraFor(viewport, tick, includeDrag)` |
| D3 | `window() ? window()->effectiveDevicePixelRatio() : 1.0` | cpp:590, 863, 893, 1192, 1267, 1807; Zoom.cpp:93, 107, 129 (9) | `dpr()` helper |
| D4 | Live bucket end `floorDiv(x + tf - 1, tf) * tf` | cpp:552, 606, 608, 930, 1039, 1146 | **Inconsistent twin:** seed and bootstrap use `floorDiv(x, tf) * tf + tf` (cpp:976, 1075). Exactly on a bucket boundary that is one tf later. Keep both forms in a neutral slice and flag. |
| D5 | Follow padding `max(tf, span * m_autoScrollPaddingFrac)` | cpp:939-940, 977, 1040, 1076 (gpuFitTimeWindow cpp:1158 is a variant) | `livePaddingMs(span)` |
| D6 | Candle timeframe seconds `max(1, (tf + 500) / 1000)` | cpp:363, 1118, 1145, 1219 | MainWindowGpu.cpp:1213 uses `(tf + 999) / 1000` and DepthChartView.qml:103 uses `Math.round`. They differ only for sub-second timeframes. Flag. |
| D7 | Zoom anchor fraction clamp | Zoom.cpp:299-300, 347-348, 413-414, 489-490 | `zoomFrac(x, y)` |
| D8 | Column rung limits (minCol/maxCol) | Zoom.cpp:353-357 == 491-495 | `columnLimits(W, tf)` |
| D9 | Span clamp to min/max limits | Zoom.cpp:304-306 == 447-449 (time), 313-315 == 456-458 (price); directional variant 378-380 | `clampSpan` |
| D10 | Post-zoom tail: `if (price) emit priceInteracted(); if (autoscroll) enableAutoScroll(false)` | Zoom.cpp:327-328, 395-396, 465-466 | |
| D11 | Raster anchor for moved axes | cpp:1387-1388, 1444-1447, 1758-1764; Zoom.cpp:292-293, 324-325, 462-463 | Three distinct rules (centre-if-moved, cursor, live edge). Factor only the "keep unchanged axis" part. |
| D12 | "Land zoom": `endZoomGlide(); m_zoomGesture = false;` | cpp:350-351, 522-523, 1355-1356, 1397-1398, 1456-1457, 1741-1742 | |
| D13 | "Price known": `m_gpuPriceKnown = true; m_gpuReseedPrice = false;` (+ `m_priceCarry.reset()`) | cpp:1327-1329, 1382-1383, 1428-1429, 1436-1437, 1474-1475, 1746-1748 | |
| D14 | Glide end shifting (both ends by a delta) | cpp:948-953, 1813-1817; Zoom.cpp:65 | |
| D15 | Layer exclusion and its 5-line log | cpp:471-494, 702-734, 736-756, 758-779, 781-806 (~130 lines) | Rules differ per layer (heatmap+footprint may coexist). A log/emit helper is neutral; a table rewrite is not. |
| D16 | Six identical debug-overlay setters | cpp:285-325 | |
| D17 | TPO layout/theme setters | cpp:441-461 | |
| D18 | Mouse pan vs invokable pan | cpp:1591-1610 vs 1782-1799 | `mousePressEvent` does not call `update()`, `beginPanAt` does: preserve that |
| D19 | Three active-symbol compares per trade, one allocating | cpp:158, 164-165, 168 (`QString::fromStdString`) | Contradicts the comment at cpp:163 ("no allocation per trade"). Removing the allocation is neutral. |
| D20 | Two price row-fit rules | Auto: `solveAutoPriceFit`/`fitPriceToRows` (cpp:1290-1304; ChartRaster.cpp:398-454). Manual / auto-off "frozen A2" rounded-centre: inline at cpp:1306-1314 | Intentional per ARCHITECTURE.md:287. Move the inline rule into ChartRaster as a named function so it is unit-testable (neutral). |
| D21 | Second pixel-to-world path | prepareSyncFrame's linear drag offset at Render.cpp:223-226 (bounds-relative), beside `chart_raster::continuousRaster` | Intentional (Auto judges the continuous view) but a parallel mapping. Do not touch in neutral slices; candidate for the "heatmap/candle drift" item (STATUS.md:112). |
| D22 | Bin-view fields copied field by field | `Cameras` (h:257-262) vs `SyncFrame` (h:266-274); copy at Render.cpp:130-135 | One `BinView` struct |
| D23 | Split signal wiring | ctor connects `snapshotChanged`/`liveChanged` to `refitAutoPrice` (cpp:78-81) and `autoPriceScaleChanged` (cpp:73-75); `init()` connects the same three signals again (Init.cpp:35-46, 104-108) | Order matters (init's slots run first). Merge in that order. |
| D24 | Tone sync three times | Init.cpp:23-25 + 33-34 (inline `setTone`), then `applyClientConfig` at Init.cpp:85 calls three setters that each `syncGpuTone`, then `syncGpuTone` again (cpp:1555-1558); `onViewportChanged()` already calls `update()` before cpp:1567 | |
| D25 | Same-member accessors | `currentTimeframe`/`getCurrentTimeframe`/`getCurrentTimeResolution` (h:322, 375; cpp:1637); `viewState()`/`getViewState()` (h:355-356); `lastTimeAxisMapping`/`currentTimeAxisMapping` (h:471-473); `addTrade` -> `onTradeReceived` (cpp:1736, used only by a test) | |

---

## 4. Comment gunk

About 484 comment lines in the five files. **30 lines** cite process instead of code: slice, plan or spec IDs, review rounds, frozen-plan paths, history. **0** chain-of-thought lines ("Actually", "Wait"): grepped, none. Most comments are dense but genuine why-comments.

Worst 15:
1. cpp:586: "(whole device pixels; **review r3**)"
2. cpp:533-539: history narration: "S6d: keeping the time span gave four 1h columns after 1m, and 1,600 hairline 1m columns after 1h"
3. cpp:1306: "**Frozen A2 path** for Manual tick..." (names a review state, not the rule)
4. Render.cpp:306: "// the one root path (**S8a**)"
5. h:136 and Init.cpp:31: "S6b GPU heatmap (the only heatmap renderer **since S8a**)", written twice
6. Init.cpp:1 and Render.cpp:1: "... **split from main TU**"
7. h:381: "(S6b, **docs/research/2026-10-s6-plan.md**)". AGENTS.md section 8 says research files are frozen once landed and nobody maintains them.
8. h:397-424: a 28-line behaviour spec that duplicates ARCHITECTURE.md, headed by a frozen research path
9. Render.cpp:198, 215, 228, 239: "Steps 1-3 (**whole-pixel plan section 1.9**)", "1-2.", "3.", "4." (numbering from an external plan)
10. cpp:962: "Cold start (**plan section 2**)"
11. cpp:868: "**Spec rules 1, 2 and 9**: ..."
12. cpp:532 and cpp:1624: "**Spec rule 1**", "(**spec rule 2**, as the lab)"
13. cpp:1253: "(**owner decision 2026-10-02**)". Keep the why, drop the date.
14. cpp:1271-1272 and 1287-1288: review-response tone ("This is expected; only a failed solve ... warrants a warning"; "Every Auto price fit uses the same raw basis")
15. h:600-602: **misplaced**. It describes `ensureGpuRootNode`'s root layout but sits above `publishTradeBubbleFrame`. cpp:163 is **false** (see D19).

Keep (good why-comments): Zoom.cpp:521-526 (Cocoa wheel quirk), cpp:128-129 (dtor unbind order), cpp:82-83, cpp:380-381 (reseed before setSymbol), cpp:1008-1013 (carry confirmation), cpp:1213-1214, cpp:1248-1249, Zoom.cpp:218-221 (one retarget rule), Render.cpp:260-261 (thread rule).

Related stale docs (fix in the same slice as the code): UI_ARCHITECTURE.md:26 names `m_vpMutex`/`m_footprintPendingMutex`, which no longer exist (0 grep hits in libs/gui). UI_ARCHITECTURE.md:41 and ARCHITECTURE.md:285 claim QML hit tests use `worldToScreen`.

---

## 5. Deferred A3 minors (STATUS.md:61, 102)

- **destroyController autoPriceTick_ reset.** `HeatmapGpuLayer::destroyController` (HeatmapGpuLayer.cpp:119-136) resets tick state but not `autoPriceTick_`. `setSymbol`/`setTimeframeMs` do reset it (:140, :153). `forgetService` (:70-81) has the same gap. This is a one-line behaviour fix in HeatmapGpuLayer, not UGR, and not neutral: land it as its own tiny slice with a test.
- **Test rename** (likely target, inferred). `UgrZoomCpu.SymbolSwitchBeforeFirstSnapshotUsesPlainFitSilently` (test_ugr_gpu.cpp:3530) was named in dc88645 ("silent plain fit before the first snapshot"). 2d0194b then changed the code to return **raw** bounds (cpp:1273), and the test asserts raw `*1.12` bounds (:3543-3545). Suggested name: `...PublishesRawBoundsSilently`. Confirm with the conductor; STATUS does not name the test.
- **Refit cost.** `refitAutoPrice` runs a full `setViewport`, then the price fit, then `drawnTimeWindow` (computeRaster), `getVisibleSlice` and `solveAutoPriceFit` (a `std::function` candidate search). It is triggered on `liveChanged` (cpp:80-81), `snapshotChanged`, `tickChanged`, `limitsChanged`, the surface, settings and `candlesDirty`. While following live, `liveChanged` also runs `followGpuLive`, whose `setViewport` fits again (GridViewState.cpp:34-39): two fits per live tick. Perf-pass item, not neutral. Gate the `liveChanged` refit on "live bucket inside the drawn window" or drop it when `followGpuLiveTo` already fitted.
- Also listed in STATUS for this refactor (render perf pass, STATUS.md:113): `VolumeProfileRenderer` rebuilds all geometry every frame, and `CandlestickBatched.cpp:498` allocates per frame. Both are outside UGR's files.

---

## 6. Split plan

### Size target and why
Aim for **UnifiedGridRenderer.cpp at about 450-600 lines** (lifecycle, window binding, viewport plumbing, the property facade) and **each extracted unit at 200-450 lines, one job per file**. The reasons are practical, not a number to hit:
- `UnifiedGridRenderer.cpp` is a declared hot file (AGENTS.md section 10): one branch at a time. Per-job files let zoom work and fit work proceed in parallel.
- AGENTS.md Fast Start asks agents to read about 200 lines before acting. A 300-line single-job file can be read whole; a 1,844-line file invites the patch-on-patch rounds seen in the git log.
- The header cannot shrink much below about 350-400 lines while QML needs about 40 properties and about 50 invokables. Prune the dead surface first, then stop.

### Phase 1: neutral slices (move or delete only; byte-identical rendering; tests unchanged)

Guarding tests, all existing:
- `test_ugr_gpu`: 100 tests. `UgrGpu` (67) and `UgrGlide` (9) **skip without a GPU** (test_ugr_gpu.cpp:118). `UgrZoomCpu` (13), `UgrWheelCpu` (5), `UgrInput` (3), `UgrPlacementCpu` and `UgrCandleCpu` run on CPU.
- `test_chart_ui`: 42 tests (ChartControls, ChartLabels, ChartToolbar).
- `test_heatmap_settings_ui`: 43 tests (HeatmapChartUi, TradeBubbleControls with the `TradeBubbleRendererTest` friend, CandleStyle, TPO/axis pixel geometry).
- `test_MainWindowSymbolLifecycle`: 16 tests (loads the full QML scene).
- `test_heatmap_label_layout`: 10. `test_ChartRaster`: 23. `test_GridViewState`: 8.

**Byte-identical proof:** the suite has image comparisons but no goldens. Before slice 1, build an out-of-tree harness (scratch, not committed, so tests stay unchanged): render about 10 canned scenes (rest, mid-glide, drag, follow-live step, auto-fit, Manual tick, TPO, VP, labels) and hash the images. Rerun it after every slice on **native Metal** (llvmpipe is not GPU verification, AGENTS.md section 4). After property removals, run one GUI-host smoke and grep the run log for QML `ReferenceError|TypeError|Cannot assign`.

| Slice | Content | Files | Delta (approx.) | Risk | Guards |
|---|---|---|---|---|---|
| **N1 Dead C++ surface** | Delete 2a, 2b and the unused includes from 2f. Turn `autoScrollPaddingFrac` into a `static constexpr`. Delete the write-only manual-timeframe pair. Fix the stale docs (UI_ARCHITECTURE.md:26, :41; ARCHITECTURE.md:285) | UGR h/cpp, 2 docs | -150 | Low (compile, plus QML runtime lookup: verified no users) | build all, full ctest, QML smoke |
| **N2 Dead QML chain** | Delete the 6 `qml/controls/*.qml` files and their CMake entries, then UGR `minVolumeFilter`, `setPriceResolution`, `setGridResolutionPreset`, `getCurrentPriceResolution`/`currentPriceResolution`. Optionally the DataProcessor stubs | qml/controls, libs/gui/CMakeLists.txt, UGR, DataProcessor | -627 QML, -60 C++ | Low. Owner sign-off advisable (deletes UI files, though none is instantiated) | QML smoke, test_MainWindowSymbolLifecycle |
| **N3 Write-only generations** | Delete `m_candleStreamGeneration`/`candleGeneration` (always 0) and the unread footprint generation | UGR, FrameContext.hpp, FrameContextBuilder.cpp, ITimeAxisMappingProvider.hpp, CandlestickOverlayItem.cpp | -25 | Low-medium (sibling files) | test_ugr_gpu candle cases, test_heatmap_settings_ui |
| **N4 Comment pass** | Rewrite or remove the 30 process-ref lines, fix the misplaced (h:600-602) and false (cpp:163) comments, trim h:397-424 to a pointer to ARCHITECTURE.md. Code untouched (verify the diff is comment-only) | 5 UGR files | -40 | None | build |
| **N5 Helpers (dedupe)** | D1, D2, D3, D5, D6, D7, D8, D9, D10, D12, D13, D14, D16, D17, D19 (drop the allocating compare), D22, D23 (merge in init's order), D24 (drop the redundant extra tone call). Keep each expression's evaluation order and operands bit-identical. **Do not** unify D4's two live-end forms or D11's three anchor rules | UGR files | -150 | **Medium**: floating-point identity and slot order | CPU fixtures first (UgrZoomCpu, UgrWheelCpu, UgrInput), then native GPU ctest and the golden harness |
| **N6 TU split** (member functions stay on UGR; pure moves) | `UnifiedGridRenderer.Camera.cpp` (job 6: Zoom.cpp:36-150, Render.cpp:95-112, cpp:889-914, 1179-1202, 1487-1499) | 4 UGR files | 0 net | Low | full ctest |
| | `UnifiedGridRenderer.Fit.cpp` (jobs 12+13: cpp:1100-1485) | | | | |
| | `UnifiedGridRenderer.Live.cpp` (jobs 9-11: cpp:157-177, 343-393, 517-646, 916-1098) | | | | |
| | `UnifiedGridRenderer.Layers.cpp` (jobs 4+5: cpp:395-515, 648-866, 1554-1576) | | | | |
| | `UnifiedGridRenderer.Input.cpp` (job 8: cpp:1591-1635, 1774-1844 plus Zoom.cpp:519-599); Zoom.cpp keeps job 7 only | | | | |
| | `UnifiedGridRenderer.Debug.cpp` (job 15: cpp:33-61, 1501-1552, 1578-1589, 1670-1734) | | | | |
| | Result: cpp is about 450 lines | | | | |
| **N7 Header regroup** | Order: properties, then public API by job, then a clearly fenced "Test hooks" block (keep them public: a friend struct would change tests), then private state grouped by job. Move the private structs (`AxisGlide`, `FollowPadding`, `Cameras`, `SyncFrame`, `RasterSurface`, `PriceCarry`) into `render/UgrTypes.hpp` | h, new hpp | ±0 | Low | build |
| **N8 Pure-function extraction** (optional) | Move the frozen-A2 rounded-centre row fit (cpp:1306-1314) into ChartRaster. Extract `fitTimeWindow` math (cpp:1139-1177) and the timeframe-switch follow-padding solve (cpp:584-619) into free functions with plain inputs, as `chart_raster::*` already is. Gains unit-testability without Qt | ChartRaster.hpp/.cpp, UGR | +40 / -60 | Medium | test_ChartRaster (no new tests in-slice; add them in a later test-only slice) |

Class extraction (`UgrZoomController`, `UgrAutoScale`, `UgrLivePlacement` as objects) is **not recommended in phase 1**. Every candidate needs `GridViewState`, `HeatmapGpuLayer`, `m_rasterStep` with its mutex, the item size and the DPR. Making a class of each means a host interface with about 10 callbacks, and the gain over N6 plus N8 is small. Revisit after N8 if `Zoom.cpp` (about 370 lines after N6) keeps growing.

### Phase 2: behaviour changes (not neutral; each needs its own review)
1. destroyController/forgetService `autoPriceTick_` reset (HeatmapGpuLayer.cpp:70-81, 119-136), with a test.
2. Test rename (test_ugr_gpu.cpp:3530) once the conductor confirms the target.
3. Refit cost: double fit per live tick (section 5).
4. Owner decisions: the three no-op debug menu toggles (2e); the `SENTINEL_DUMP_GLYPH_ATLAS` /tmp dump (2h).
5. Latent inconsistencies to decide on and test: the D4 live-end forms at bucket boundaries (seed/bootstrap vs `gpuLiveEndMs`); D6 candle-seconds rounding (UGR vs MainWindowGpu.cpp:1213 vs DepthChartView.qml:103).
6. Remove the ~78 `m_viewState` and 13 `m_axisTextService` dead null checks (2g). Neutral in theory but noisy and spread everywhere; do it after N6 so each per-job file is touched once.

Order: N4 then N1, N2, N3 (deletions on clean comments), then N5, N6, N7, then N8. One hot-file branch at a time; each slice lands before the next starts.
