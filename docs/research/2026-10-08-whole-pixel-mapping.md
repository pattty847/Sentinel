# Task packet: whole-pixel chart mapping (raster camera), 2026-10-08

Planner: Claude Fable (read-only; nothing edited, built or run). Repo `main` = `62f1c6a` (docs-only on top of `4913fc1`). Owner decision 2026-10-08 (final) and `_agent/DECISIONS.md` 2026-10-08 entry apply; the "why" is not re-litigated here.

## 0. Base, branches, worktrees, ports

- **Base:** `main` AFTER `lt-sol/palette-texel` lands. That branch (uncommitted in `/Volumes/T7/sentinel-worktrees/lt-sol-palette-texel`) edits `tests/render/test_heatmap_tile_node.cpp`, `tests/render/CMakeLists.txt`, `HeatmapPalette.cpp`, both frag shaders, and adds `tests/render/test_heatmap_palette.cpp`. This slice extends `test_heatmap_tile_node.cpp` (the `DisplayReadback` harness) and must not race it. No other in-flight branch (`lt-claude/roller-db1`, `lt-astra/anchors-b`, `lt-astra/journal-anchors`) touches `libs/gui` or `tests/render` (`git diff --name-only main...<branch>` checked).
- **Slice A** `lt-claude/pixel-raster` (raster camera, heatmap rows+columns, axes, hit-testing, GridViewState anchor/pan commit, Agent API `drawn`). **Slice B** `lt-sol/pixel-geometry` (candle bodies, VP/TPO edge snapping; after A lands). See section 9.
- GUI runs: own `--api-port 17112` and own scratch dir via `scripts/dev/gui-shot.sh launch --build <worktree>`; never the owner's GUI (AGENTS 4b). Builds: `scripts/dev/build-queue.sh --label pixel-raster -- cmake --build --preset mac-clang -j 2`.
- Risk class: **high** (GPU heatmap core, hot files `UnifiedGridRenderer.cpp` and possibly `MainWindowGpu.cpp`).
- Hand-off: AGENTS section 10 step 3 (`READY: <branch>` + tip, changes, checks with summary lines, unverified items, `WORKFLOW:` last line).

## 1. Design

### 1.1 Two cameras, one function, one frame struct

Today `UnifiedGridRenderer::computeGpuFrameMapping` (`libs/gui/UnifiedGridRenderer.Render.cpp:94-140`) bakes the drag offset into the continuous viewport (`UgrFrameMath::applyDragPan`, `libs/gui/render/UgrFrameMath.cpp:6-24`) and fills `TimeAxisMapping` with fractional `cellW/cellH` (`:137-138`). It runs twice per frame (`:183` with tick 0, `:188` with the chosen tick) around `HeatmapGpuLayer::prepareFrame` (`:186`), which chooses the tick (`libs/gui/render/heatmap/HeatmapGpuLayer.cpp:348-363`) and posts labels for the view it is given (`:427-488`). Candles, bubbles, labels, VP, TPO, footprint, paper/algo overlays all read `frame.mapping` or the published `MappingFrameContext` (`UnifiedGridRenderer.Render.cpp:15-39`, `UnifiedGridRenderer.cpp:1407-1414`).

New:
- **Continuous camera** = what `GridViewState` stores (`m_visibleTimeStart_ms/End`, `m_minPrice/m_maxPrice`, `m_panVisualOffset`; `libs/gui/render/GridViewState.hpp:98-117`). Exact, never rounded back from pixels (no accumulation of frame-to-frame deltas). Unchanged semantics; `setViewport()` and `viewportVersion` unchanged (INV-003).
- **Raster camera** = new `libs/gui/render/ChartRaster.hpp/.cpp`, a pure function:
  ```
  struct RasterInputs { int64 timeStart, timeEnd; double minPrice, maxPrice; QPointF dragLogicalPx;
                        double tfMs, tick; double itemWidthLogical, itemHeightLogical, dpr;
                        double anchorFracX, anchorFracY; };
  struct RasterStep   { int rowPx = 0, colPx = 0; };            // last frame's integers (0: none)
  struct RasterCamera { bool valid; int rowPx /*P*/, colPx /*C*/; double dpr; int widthDev, heightDev;
                        int64 topRowIndex /*n*/, leftColIndex /*m*/; double tfMs, tick;
                        double drawnStartMs, drawnEndMs, drawnMinPrice, drawnMaxPrice;
                        double xDev(double timeMs) const { return timeMs * colPx / tfMs - double(leftColIndex); }
                        double yDev(double price)  const { return double(topRowIndex) - price * rowPx / tick; }
                        double timeAtXDev(double x) const; double priceAtYDev(double y) const; };
  RasterCamera computeRaster(const RasterInputs&, RasterStep previous);   // pure, any thread
  TimeAxisMapping toMapping(const RasterCamera&);                          // fills the existing struct
  ```
  Device pixels, item-relative (every price/time layer is `anchors.fill: unifiedGridRenderer`, `libs/gui/qml/DepthChartView.qml:95-144`, so item-relative alignment locks them to each other; absolute alignment additionally needs the item origin on a device pixel, checked in the lab, section 6).
- **Computed once per frame** in `computeGpuFrameMapping` (render thread, GUI blocked), stored as `m_raster` next to `m_lastTimeAxisMapping` (`UnifiedGridRenderer.h:167`), published in `MappingFrameContext` (`libs/gui/render/ITimeAxisMappingProvider.hpp:10-34`, add `RasterCamera raster;`). The `RasterStep` state (`m_rasterStep`) is written there and read by GUI-thread callers afterwards (the render thread writes only while the GUI thread is blocked in sync; keep it under `m_frameContextMutex` anyway).
- **GUI-thread evaluation** `UnifiedGridRenderer::rasterCameraNow()` builds `RasterInputs` from the current `GridViewState`, `m_gpuLayer->tickPrice()`, `width()/height()`, `window()->effectiveDevicePixelRatio()`, the anchor, and `m_rasterStep`. Same pure function, same inputs the next frame will see (except a tick change in that frame, which crossfades anyway), so axis positions and the frame agree. Hit-testing uses the published frame's inverse (what is on screen).

### 1.2 Integer pixels per row and per column

- `rRow = heightDev * tick / (maxPrice - minPrice)`, `rCol = widthDev * tf / (timeEnd - timeStart)`, with `widthDev = floor(itemWidthLogical * dpr)`, `heightDev = floor(itemHeightLogical * dpr)`.
- `step(r, prev)`: keep `prev` while `|r - prev| <= 0.5 + kStepBandPx` (`kStepBandPx = 0.1`, device px), else `max(1, lround(r))`. So 2 -> 3 at r > 2.6, 3 -> 2 at r < 2.4; a tick change (r jumps by the preset ratio) re-rounds. Hard constant, no setting (owner: no switch).
- **Auto tick** stays exactly `autoTickUnits` on the continuous camera (`HeatmapGpuLayer::chooseTick`, `HeatmapResolution.hpp:108-123`, `minRowPx = 2`, `h = 0.25`). With defaults r never drops below 1.5 before Auto coarsens, and `lround(1.5..2.49) = 2`, so drawn rows are never thinner than 2 px in Auto; the drawn height steps 2, 3, 4, 5 as you zoom in, then the tick steps finer. `{1,2,2.5,5}x10^k` presets unchanged. **Manual** tick: P from the same rule; the 1 row/px clamp (`maxManualPriceSpan`, `HeatmapResolution.hpp:151`; `GridViewState.cpp:49-52, 224`) gives r >= 1 so P >= 1.
- **Columns:** C from `rCol` with the same step rule. The 1 column/px zoom-out clamp (`HeatmapGpuLayer::maxTimeSpanMs` `:229-231` = `widthPx_*dpr_*tf`) gives rCol >= 1, so C >= 1; at rCol in [1, 1.4] with prev 2 it stays 2 until 1.4 (drawn shows W/2 columns), then C = 1 (one column per device pixel; `floor()` in the shader picks one bucket per pixel as today). **Very wide columns** (zoom-in floor `kMinZoomColumns = 4`, `HeatmapGpuLayer.hpp:95` -> C ~ W/4 >= 160): 1 px steps are invisible, nothing special. **Timeframe switch** (`UnifiedGridRenderer::setTimeframe` `:481-547`) scales the span by the tf ratio, so rCol and C are unchanged; the anchor is the Now column (section 1.4). **Follow-live** (`followGpuLive` `:785-801`) shifts the committed viewport by `shift` ms; m moves by `lround(shift*C/tf)`: whole-pixel steps, the picture between steps is byte-identical. The live bin's draw clip is at bucket edges (`HeatmapTileNode::subRect` `:709-722`), hence on pixel edges.
- **Tick 0** (before a tick is drawn, today "rows one pixel tall" `Render.cpp:109-110`): tick = `priceSpan / heightDev`, so r = 1, P = 1; the mapping stays valid as today.

### 1.3 Edges on pixel edges (the snap)

Row b's edges land on device pixels iff `drawnMaxPrice = n * tick / P` (n integer); column k's edges iff `drawnStartMs = m * tf / C`. Then in the shader (`heatmap_display.frag:19-20`) `fy = priceOffset + (j+0.5)/P` with `priceOffset*P` integer: `floor(fy)` changes exactly every P pixels and never at a pixel centre. The block quads in `HeatmapTileNode::addBinDraw` (`:799-811`, `y0/y1` at `:803-804`) and the time sub-rects (`subRect` `:717-718`) are linear in `frame_.view`/`frame_.rect`; given the snapped view they land on integer device pixels. **`HeatmapTileNode.cpp`, `HeatmapBinGrid.hpp::mappingFor` (`:75-83`) and both shaders are untouched.** 4x MSAA becomes irrelevant to row boundaries (no partial coverage).

Anchor-preserving choice of n and m (section 1.4):
```
anchorTime  = timeStart + fracX * (timeEnd - timeStart);  anchorPrice = maxPrice - fracY * (maxPrice - minPrice)
m = llround(anchorTime  * C / tf  - fracX * widthDev)  - llround(drag.x * dpr)
n = llround(fracY * heightDev + anchorPrice * P / tick) + llround(drag.y * dpr)
drawnStartMs = m * tf / C; drawnEndMs = drawnStartMs + widthDev * tf / C
drawnMaxPrice = n * tick / P; drawnMinPrice = drawnMaxPrice - heightDev * tick / P
```
Signs follow `applyDragPan` (`UgrFrameMath.cpp:17-18`: drag right = earlier start, drag down = higher top). Precision: `anchorTime*C/tf` <= ~3e10, exact in double; BTC `price*P/tick` ~3e5; PEPE 1e-5/1e-8 ~1e3.

`toMapping`: `drawRect = QRectF(0,0,widthDev/dpr,heightDev/dpr)`, `view* = drawn window`, `dataStartMs = floor(drawnStart/tf)*tf`, `srcRect = ((drawnStart-dataStart)/tf, 0, widthDev/C, heightDev/P)`, `cellW = C/dpr`, `cellH = P/dpr`, `viewportColumns = true`, `timeOffset = 0`; add fields `dpr, rowPxDev, colPxDev` to `TimeAxisMapping` (`libs/gui/render/TimeAxisMapping.hpp`) for consumers that snap their own geometry (slice B). `FrameContextBuilder.cpp:21` sets `surfaceBounds` to the same device-integer rect (one place; identical to `boundingRect()` on the owner's 2x Mac with integer layout; differs by < 1 px only at fractional DPR with odd sizes), so VP/TPO/footprint, which interpolate `view*` over `surfaceBounds`, stay on the same slopes.

### 1.4 Zoom anchor invariance

`GridViewState` gets `struct RasterAnchor { double fracX = 0.5, fracY = 0.5; }` + `setRasterAnchor(fracX, fracY)` + getter. Set by:
- wheel (`handleZoomWithViewport`, `GridViewState.cpp:174-180` already computes `centerTimeRatio/centerPriceRatio`: store them; `fracY = 1 - centerPriceRatio`), time-axis drag/wheel (`handleTimeZoomWithSensitivity` `:313-314`, x only), price-axis drag/wheel (`handlePriceZoomWithSensitivity` `:350-351`, y only), keyboard zoom (centre; `UnifiedGridRenderer::zoomIn/Out` `:1509-1522` pass the centre already).
- pans: unchanged (the anchor's screen position persists; the drag is applied as `llround(drag*dpr)` whole device pixels, 1:1 with the mouse).
- `UnifiedGridRenderer` sets it for its own moves: `setTimeframe` -> `fracX = 1 - *rightFrac` when known (`:512-517`), else 1.0; `applyViewportRequest` (`:1134-1182`) and `UnifiedGridRenderer::setViewport` (`:1485-1503`) -> (0.5, 0.5); fits/`resetView`/`returnGpuToLive`/`seedGpuViewport`/`bootstrapGpuTimeView` -> `fracX = 1.0` (view end), `fracY = 0.5`. While auto-scroll is on, `RasterInputs.anchorFracX` is forced to 1.0 (the live edge stays one padding inside; no GridViewState change needed).
- The anchor keeps the anchored price/time within 0.5 device px of where the continuous camera puts it, for any P/C, including the frame where P or C steps: the content under the cursor does not jump.

**Pan release without a jump.** Today `handlePanEnd` (`GridViewState.cpp:259-289`) commits `pan * span/size` (continuous scale). With P != r the recomputed n would differ from the dragged n by up to 1 px at release. Add a hook like `PriceFit` (`GridViewState.hpp:52-53`): `using PanScale = std::function<bool(double& pricePerLogicalPx, double& msPerLogicalPx)>; setPanScale(...)`, set by UGR to return `tick/P*dpr` and `tf/C*dpr` from `rasterCameraNow()`; `handlePanEnd`, `dragShiftMs` (`:106-109`) and `displayedTimeWindow` (`:111-114`) use it when it returns true, else the continuous scale (tests without the hook unchanged). Then `n_committed == n_dragged` exactly (adding an integer to the rounded quantity).

**Fits land on integer-pixel spans.** `gpuFitPriceWindow` (`UnifiedGridRenderer.cpp:1013`) and `autoPriceFit` (`:1068`) widen `[lo, hi]` (after `kFitPriceMargin`) to `heightDev * tick / P_fit`, `P_fit = max(1, floor(heightDev*tick/(hi-lo)))`, tick = `m_gpuLayer->tickPrice()` (skip when 0), inside min/max spans. Then r = P_fit exactly, drawn == stored, and the fitted candles are never cropped by the snap (without this, P = lround(r) could crop up to 20% of a fit). Time fits: `gpuInitialSpanMs` (`:924-928`) is `initial_column_px` (8 logical = 16 device px at 2x) per column: already integer. Wheel and axis zooms keep the continuous span; the drawn window deviates by r/P (<= 25% at P = 2), which is the owner's accepted stepped zoom.

### 1.5 Hysteresis
Two independent bands, both on the continuous camera: the existing Auto tick band (`minRowPx`, `h`, untouched) and the integer step band (`kStepBandPx = 0.1` beyond the half-integer). A zoom that oscillates inside either band flips nothing (acceptance A3/B5).

### 1.6 Dragging
Per frame: `RasterInputs.dragLogicalPx = frame.viewport.panVisualOffset` while `dragging`; the camera applies `llround(drag * dpr)` device pixels to m/n. Every layer reads the one mapping, so everything moves by the same integer. `UgrFrameMath::applyDragPan` loses its only caller (`Render.cpp:105`): delete it, its header and `tests/render/test_UgrFrameMath.cpp` + CMake target (`tests/render/CMakeLists.txt:30-42`) in the same push (AGENTS 1, no backcompat). Axis models today add the raw drag (`PriceAxisModel.cpp:155-161, 171-177`, `TimeAxisModel.cpp:131-137, 147-153`); they switch to `rasterCameraNow()` (section 2).

### 1.7 DPR changes
`FrameContextBuilder.cpp:22` reads `effectiveDevicePixelRatio()` every frame, so the camera follows a monitor move at once: `heightDev` doubles, r doubles, `|r - prev| > 0.6` re-rounds P in that frame (one re-snap, expected). `HeatmapGpuLayer::setSurface` (the clamps) is refreshed only from `bindWindow` and `geometryChange` (`UnifiedGridRenderer.cpp:83, 202`); add `connect(w, &QQuickWindow::screenChanged, this, [this]{ syncGpuSurface(); })` in `bindWindow` (`:79-103`) so the 1 column/px clamp follows the DPR (`AxisTextService` already does this for label px, `AxisTextService.cpp:130`). UNVERIFIED: whether Qt emits `geometryChange` on a DPR-only change; the connection makes it moot.

### 1.8 Stored versus drawn
- Stored (`GridViewState`, `setViewport()`, `viewportVersion`, `GET /api/v1/viewport` bounds, `POST /viewport` semantics, all clamps): continuous, exact, unchanged. INV-003 holds; no viewport change is introduced per frame.
- Drawn (`MappingFrameContext.raster`, `frame.mapping.view*`): the snapped window, up to r/P off the stored one in each axis after a wheel/axis zoom, equal after fits. Published read-only. `GET /api/v1/viewport` gains `drawn: {startMs, endMs, priceMin, priceMax, rowPx, colPx, dpr}` from the last published frame (encode next to `zoom` in `libs/gui/mainwindow/AgentApiCodec.cpp:587-590`; struct in `AgentApiTypes.hpp`; the producer passed to `GuiApiServer` (`GuiApiServer.cpp:52, 234`) is UNVERIFIED to live in `MainWindowGpu.cpp` ~`:1073`; if so, that is the only hot-file touch there, about 10 lines). Document in `docs/AGENT_API.md` (`:8`, `:20`).
- The controller is asked for the **drawn** window: `syncGpuView` (`UnifiedGridRenderer.cpp:775-781`) posts `rasterCameraNow()`'s drawn bounds (a superset of the screen where P < r), and `prepareFrame` gets the drawn view so `postLabels` (`HeatmapGpuLayer.cpp:443-468`) covers what is on screen.
- `HeatmapGpuLayer::metrics()["rowPx"]` (`:758`) stays the continuous r; add `rowPxDrawn/colPxDrawn` via a `noteRaster(P, C)` call from the frame (atomics; telemetry dock reads on the GUI thread).

### 1.9 Frame order in `updateGpuPaintNode` (`Render.cpp:179-221`)
1. continuous view (committed + drag, for Auto eligibility over the columns in view, as today);
2. `tick = m_gpuLayer->chooseTickForView(continuousView)` (split out of `prepareFrame` `:365-385`: snapshot refresh, `chooseTick`, `tickChanged`, `setTickRequest`);
3. `camera = computeRaster(inputs, m_rasterStep)`; `m_rasterStep = {P, C}`; `frame.mapping = toMapping(camera)`; `frame.raster = camera`;
4. `prepared = m_gpuLayer->prepareFrame(tileFrame, frame.surfaceBounds, drawnView)` (fills the node frame, posts labels);
5. the rest unchanged (`publishFrameContext`, bubbles, overlays, axis text, labels). One mapping computation per frame instead of two.

## 2. Every consumer, and how it changes

Automatic (reads `frame.mapping`/`currentFrameContext()`; no code change, verify in B4):
- Heatmap node: `HeatmapTileNode::Frame::view/rect` (`HeatmapTileNode.hpp:174-193`) receives the drawn view; `subRect :709-722`, `addBinDraw :784-824`, `rowsCover :443-448`, `binRows :452-458` unchanged.
- Liquidity labels: `HeatmapLabelLayout::layout` (`libs/gui/render/heatmap/HeatmapLabelLayout.cpp:95-126`, `cellW/cellH :109-110`, `columnX_/rowY_ :124-126`; box snap `:8, :151`).
- Trade bubbles: `TradeBubbleData.cpp:85-95` (centres), `TradeBubbleNode.cpp:61-123` (fast-path translation becomes whole pixels). Bubble centres stay fractional by design (circles).
- Footprint: `FootprintOverlayRenderer::render` (`:43-197`, `mapTradeOverlay :176-177`) from `frame.mapping.view*` + `surfaceBounds` (`Render.cpp:48-53`).
- VP: `VolumeProfileRenderer::render/rebuildGeometry` (`:162-177, :239-365`, `priceToY :84-91`) from `frame.mapping.viewMinPrice/MaxPrice` + `surfaceBounds` (`Render.cpp:59-65`).
- TPO: `TpoOverlayRenderer::render` (`:423-578`, `pxPerMs/pxPerPrice :487-488`, `anchorY :501`, `startX :535`) from `frame.mapping.view*` + `surfaceBounds` (`Render.cpp:68-75`, INV-037).
- Candles: positions via `mapping.timeToScreenX/priceToScreenY` (`CandlestickOverlayItem.cpp:577-592`); wicks/dojis already device-snapped (`CandlePixelGeometry.hpp:25-35`, INV-109).
- Paper trading: `PaperTradeOverlayModel::priceFromScreenY` (`:10-19`, hit-test inverse), `PaperTradeOverlayRenderer.cpp:62-66, 228-302`; algo: `AlgoOverlayRenderer.cpp:117-193`.

Must change (would otherwise silently stay fractional or desync):
1. `UnifiedGridRenderer.Render.cpp:94-140, 179-221`: the camera (section 1.9).
2. `libs/gui/render/FrameContextBuilder.cpp:21`: device-integer `surfaceBounds`.
3. `GridViewState.hpp/.cpp`: `RasterAnchor`, `PanScale` hook in `handlePanEnd :259-289`, `dragShiftMs :106-109`, `displayedTimeWindow :111-114`; anchor capture in the three zoom handlers; delete dead `calculateViewportTransform` (`:130-152`, no callers).
4. `UnifiedGridRenderer.cpp`: anchors at `setTimeframe :523-537`, `applyViewportRequest :1177`, `setViewport :1501`, fits/reset/return/seed/bootstrap; `syncGpuView :775-781` posts the drawn window; `gpuFitPriceWindow :1013` / `autoPriceFit :1068` integer-pixel spans; `bindWindow :79-103` `screenChanged`; `rasterCameraNow()`; `worldToScreen/screenToWorld :1214-1242` reimplemented through the published mapping (`timeToScreenX/priceToScreenY`, `screenXToTime/screenYToPrice`); then `libs/gui/CoordinateSystem.{h,cpp}` has no production caller: delete (check the `#include` in `tests/render/test_ugr_gpu.cpp:7` first, UNVERIFIED whether used).
5. `HeatmapGpuLayer.hpp/.cpp`: split `chooseTickForView` / `prepareFrame` (`:365-402`); `noteRaster`; metrics.
6. Axis models: `PriceAxisModel.cpp:150-197` and `TimeAxisModel.cpp:126-173` compute positions and the label range from `m_renderer->rasterCameraNow()` (`AxisModel` already holds the renderer, `AxisModel.cpp:20-34`): `position = yDev(value)/dpr`, range = drawn window; drop the raw-drag arithmetic. `AxisTextService::submitAxisText` (`:329-369`) consumes positions unchanged. `kPosEps = 0.5` (`AxisModel.cpp:~199`) becomes `0.5/dpr` or 0.
7. `libs/gui/qml/DepthChartView.qml`: `gridLines.getXForTimePoint` (`:528-549`, raw viewport math) is deleted if unused; `gridRepeater` `x: model.position + unifiedGridRenderer.panVisualOffset.x` (`:570`) becomes `x: model.position` (the model now includes the drag; the double shift during drags is UNVERIFIED but likely pre-existing).
8. `ITimeAxisMappingProvider.hpp:10-34`, `TimeAxisMapping.hpp`: new fields (section 1.3).
9. Agent API: `AgentApiTypes.hpp`, `AgentApiCodec.cpp:587-590`, the snapshot producer (UNVERIFIED location, `GuiApiServer.cpp:52`), `docs/AGENT_API.md`.
10. Slice B (look): `CandlestickOverlayItem.cpp:580-583` body width `max(1, candleW*0.7)` and `:599-603` min height 1.5 logical -> device-integer body: `gap = C >= 3 ? max(1, floor(0.15*C)) : 0`, `bodyDev = C - 2*gap`, `y0 = floor(yDev)`, `y1 = ceil(yDev)`, min height `max(1, lround(1.5*dpr))` device px; `candle_pixels::body(...)` helper next to `stroke/doji` in `CandlePixelGeometry.hpp`. Line mode (`:536-566`, 1.5 px AA segments) stays fractional (diagonals). `VolumeProfileRenderer.cpp:325-326, 353-358, 270-275`: snap y edges with `floor(y*dpr)/dpr` (needs `frame.surfaceDpr` passed into `render`); `TpoOverlayRenderer.cpp:563-564` round `tx/ty` in device px, `:325-344` cell edges likewise. Footprint texture cells remain at their own grid (nearest-sampled quad); rigid motion only.
11. Stays continuous on purpose: `HeatmapGpuLayer::chooseTick` (Auto decides on the continuous camera), `AgentApiCodec.cpp:590` `zoom.msPerPx/pricePerPx`, `LabItem.cpp:85-120` (sentinel-lab has its own view path; out of scope, note in STATUS).

## 3. Files owned, hot files, in flight

Slice A owns: new `libs/gui/render/ChartRaster.{hpp,cpp}`, `tests/render/test_ChartRaster.cpp` (+ `tests/render/CMakeLists.txt`); `libs/gui/UnifiedGridRenderer.{h,cpp,Render.cpp}` (hot: `UnifiedGridRenderer.cpp`); `libs/gui/render/{GridViewState.*, FrameContextBuilder.cpp, TimeAxisMapping.hpp, ITimeAxisMappingProvider.hpp}`; delete `libs/gui/render/UgrFrameMath.*`, `tests/render/test_UgrFrameMath.cpp`, `libs/gui/CoordinateSystem.*`; `libs/gui/render/heatmap/HeatmapGpuLayer.{hpp,cpp}`; `libs/gui/models/{AxisModel.*, PriceAxisModel.cpp, TimeAxisModel.cpp}`; `libs/gui/qml/DepthChartView.qml` (grid lines only); `libs/gui/mainwindow/{AgentApiTypes.hpp, AgentApiCodec.cpp}`; `docs/AGENT_API.md`; tests `tests/render/{test_ugr_gpu.cpp, test_heatmap_tile_node.cpp, test_GridViewState.cpp, test_PriceAxisModel.cpp}`, `tests/agentapi/test_AgentApiCodec.cpp`. Possibly `libs/gui/MainWindowGpu.cpp` (hot) for the viewport snapshot fields only.
Slice B owns: `CandlestickOverlayItem.cpp`, `CandlePixelGeometry.hpp`, `VolumeProfileRenderer.{hpp,cpp}`, `TpoOverlayRenderer.cpp`, `UnifiedGridRenderer.Render.cpp:59-75` (pass dpr), tests `test_ugr_gpu.cpp`, `test_TradeOverlay.cpp`, `test_heatmap_settings_ui.cpp`.
Not touched by either: `HeatmapTileNode.*`, `HeatmapBinGrid.hpp`, both shaders, `HeatmapResolution.hpp`, `HeatmapSourceController.*`, `DataProcessor.cpp`.
In flight (STATUS 2026-10-07 + worktree list): `lt-sol/palette-texel` (must land first, section 0); roller/anchors branches touch no GUI file. Hot files `UnifiedGridRenderer.cpp` and `MainWindowGpu.cpp` are free; slice A takes both for its duration; slice B does not need them.

## 4. FM-201: existing suites over the owned files and what changes

- `UgrGpuTests` (`tests/render/test_ugr_gpu.cpp`, 40 cases incl. `UgrInput.*`, `LocalChunkTransportFaults`; targets `CMakeLists.txt:177-185` incl. `UgrGpuServiceTeardownGuardMalloc`): `CandlesAlignWithHeatmapColumns :235-297` changes (edges exact, not +-1; add rows and a sub-pixel drag); `SettlesOnTheProductionPath :229` (`maxTimeSpanMs == 640*minute`) unchanged (stored clamp); `PriceAxisFit*`, `AutoScale*`, `PriceFitCountsTheCandleUnderTheLeftEdge`, `TimeAxisResetGoesToTheDefaultView`, `ViewportRequestsResolveFlagsAndCommitOnce` may assert exact fitted bounds: fitted spans become `heightDev*tick/P_fit` (allow it; the candles still land in view); `TimeframeSwitch*`, `NowColumnStaysPut*` unchanged (stored); `AutoScaleFitsTheDisplayedWindowDuringADrag :994` uses `displayedTimeWindow` (hook changes the shift by < 1 px worth of ms: re-check). New cases B3-B8.
- `HeatmapTileNodeTests` + `HeatmapTileNodeTeardownGuardMalloc` (`test_heatmap_tile_node.cpp`, 18 cases; `CMakeLists.txt:154-157, 216`): node untouched; `DrawsTheCellStatesAndPansWithoutRebinning :177-224` and `TileEdgesOnPixelCentresLeaveNoSeam :453-483` unchanged; new B1/B2 on the ported `DisplayReadback` (archive `c94690c` `tests/render/test_heatmap_tile_node.cpp:98-235`, as landed by palette-texel).
- `HeatmapLiveNodeTests` (`test_heatmap_live_node.cpp`, 12): unchanged, run.
- `GridViewStateTests` (`test_GridViewState.cpp`, 14): unchanged (no hook/anchor set); add anchor capture, `PanScale` commit (A7), `TheFitFollowsTheDisplayedWindowDuringADrag :261` re-checked.
- `UgrFrameMathTests` (`test_UgrFrameMath.cpp`, 1): deleted with `applyDragPan`.
- `TimeAxisMappingTests` (`test_TimeAxisMapping.cpp`, 27): unchanged (helpers only; new fields default).
- `PriceAxisModelTests` (`test_PriceAxisModel.cpp`, 4: `UsesSharedLadderAcrossViewportRanges`, `HonorsLadderCellTick...`, `IgnoresNonLadderCellAlignment`, `StartsOnStepGridAfterPan`): models without a renderer keep the continuous fallback, so unchanged; add one case with a renderer camera (positions integer device px).
- `AxisLayoutTests` (4): unaffected, run.
- `ChartUiTests` (`test_chart_ui.cpp`; `ChartLabels.* :1529-1788` drive the mapping and labels, `AxisControlsHaveKeyboardActions... :917`): run; `LabelsDrawOnColouredCellsAtTheDrawnTick :1529` may assert glyph positions: re-check.
- `HeatmapLabelLayoutTests` (`test_heatmap_label_layout.cpp`, 10; `LabelBoxesSnapToDevicePixels :227`): unchanged, run.
- `TradeBubbleTests` (`test_TradeBubbles.cpp`, 13; `OrderedRingBinarySearchBoundsScanAndPanReusesGeometry :198` builds its own `TimeAxisMapping`): unchanged, run.
- `HeatmapPlumbingTests`, `HeatmapSettingsUiTests` (`minRowPx` round trips `:128-185`, `:126-138`; `candle_pixels::stroke/doji/bodyAlpha :156-180`): A unchanged; B adds `candle_pixels::body`.
- `TradeOverlayTests` (`test_TradeOverlay.cpp`, `VolumeProfile* :529-755`, 5 cases asserting bar geometry): slice B updates expected y values to device-snapped ones (dpr 1 in tests).
- `TpoProfileModelTests`: model only, unaffected.
- `tests/agentapi/test_AgentApiCodec.cpp`: viewport snapshot encoding (`:90-96` ops) gains `drawn`.
- Lab: `LabItemTests` untouched (own path). The landing gate runs the full ctest (96/96 today) per AGENTS 4.

## 5. Acceptance checks (each fails without its fix)

CPU, `tests/render/test_ChartRaster.cpp` (new target `ChartRasterTests`):
- **A1 edges on pixels:** 2,000 random `(viewport, tick in the preset ladder, tf, dpr in {1, 2}, size)`: every `yDev(b*tick)` and `xDev(k*tf)` within 1e-6 of an integer. Mutation: drop the `llround` on n or m.
- **A2 rigid pan:** 500 drag steps of 0.07 logical px at dpr 2 from one committed viewport: P, C constant; successive cameras differ exactly by `llround(drag*dpr)` in m/n; every drawn row height `yDev(b)-yDev(b+1) == P`. Mutation: apply the drag in continuous units before snapping (today's `applyDragPan`): the translation deviates from `llround(drag*dpr)` by 1 px at some steps.
- **A3 hysteresis:** sweep rRow 2.0 -> 3.0 -> 2.0 in 0.01 steps with `previous` threaded through: exactly one 2->3 (at > 2.6) and one 3->2 (at < 2.4); jitter +-0.05 around 2.5 for 200 steps never flips. Mutation: `kStepBandPx = 0` flips at 2.5 both ways and under jitter.
- **A4 anchor across a step:** `fracY = 0.3`, rRow 2.59 -> 2.61: `|yDev(anchorPrice) - 0.3*heightDev| <= 0.5` before and after (P 2 -> 3). Mutation: top-anchored `n = llround(maxPrice*P/tick)`: the anchor moves by up to `0.3*heightDev*(1/2.6 - 1/3)` device px.
- **A5 DPR:** same logical viewport at dpr 1 and 2: `rowPx(2x) in {2*rowPx(1x) +- 1}`, logical row height within 0.5 logical px. Mutation: use logical height for `heightDev`.
- **A6 floors:** rCol in [1, 1.4] with previous 0 -> C = 1, never 0; rRow 0.4 (hypothetical) -> P = 1. Mutation: remove `max(1, ...)`.
- **A7 release commit (GridViewState + hook):** drag `delta` logical px (choose `delta*dpr = 7`, r = 2.4, P = 2), release: camera after commit equals the camera during the drag (same m, n). Mutation: commit with the continuous scale (`7*P/r = 5.83` -> n differs by 1).
- **A8 fits are integer spans:** `gpuFitPriceWindow`-style widening gives `heightDev*tick/(hi-lo)` an exact integer and `>=` the margined span. Mutation: skip the widening (crop of the top margin measurable in B3's candle test as a body clipped by the view edge).

GPU readback, `tests/render/test_heatmap_tile_node.cpp` on `DisplayReadback` (Metal on the Mac; a skipped GPU case is no result, AGENTS 4):
- **B1 isolated row constant height:** one hot row in an otherwise empty grid, `rRow = 2.4` (today's Auto regime), 300 sub-pixel drag offsets (0.3 device px steps) run through `computeRaster` -> drawn view -> draw params; in one pixel column count the rows of the hot colour: always P (= 2), never 3. Mutation: hand the continuous (unsnapped) view to the draw: heights alternate 2/3 (the owner's flicker).
- **B2 every drawn row the same height:** 10 rows of distinct codes over the view; measure each band's pixel height: all equal P; also for P = 3 and 5. Mutation as B1 (bands of 2 and 3).
- **B3 candle and heatmap edges coincide** (`UgrGpu.CandlesAlignWithHeatmapColumns` extended): view a third of a column in plus a 0.3 device px drag; measured heatmap column edges equal the candle body x edges exactly (not +-1), and a candle whose open/close sit on bin edges has body y edges equal to the heatmap row edges. Mutation: fractional body (today `candleW*0.7`): off by one on some columns.
- **B4 rigid motion of all layers** (`UgrGpu`): pan in 0.3 device px steps for 40 frames; the heatmap image, the candle image and the axis text glyph rects shift by the same integer each frame (image compare after `np.roll`-style shift; glyphs via the renderer's submitted runs or `priceAxisModel` positions). Mutation: axis models on the raw viewport: label positions move fractionally while the picture moves by whole pixels.
- **B5 zoom hysteresis does not oscillate** (`UgrGpu`): alternate wheel +-120 at one cursor position 50 times around the 2.6/2.4 threshold: `drawn.rowPx` changes at most once; `GET /viewport drawn.rowPx` equals the measured band height. Mutation: band 0.
- **B6 anchor across an integer step** (`UgrGpu`): wheel at `y = 0.3*height` until P steps: `screenYToPrice(y)` from the published mapping before and after differ by less than one drawn row. Mutation: top-anchored snapping.
- **B7 follow-live whole-pixel steps** (`UgrGpu`, auto-scroll on, synthetic live advancing): between live steps consecutive images are byte-identical; at a step the picture translates by an integer and `drawn.colPx` is constant. Mutation: fractional m.
- **B8 clamps:** zoomed out to the max span: `drawn.colPx == 1` and every column 1 device px wide; Manual at its max price span: `drawn.rowPx == 1`. Mutation: step rule without the floor.
- Existing: `TileEdgesOnPixelCentresLeaveNoSeam`, `DrawsTheCellStatesAndPansWithoutRebinning` (0.3 bucket at 20 px = 6 px pan) must stay green.

## 6. Native evidence and the owner's lab look

Conductor-driven hosted GUI (`scripts/dev/gui-shot.sh launch --build <worktree>`, `docs/AGENT_WORKFLOW.md` "Running and seeing the app"), BTC-USD 1m, Auto tick, DPR 2:
1. `api GET /api/v1/viewport`: `drawn.rowPx`, `drawn.colPx` integers; `drawn.dpr == 2`.
2. Scripted drag through `/api/v1/input` (`dragStart`, 40 x `dragMove` by 0.25 logical px, `dragEnd`) with a `shot --target heatmap` after each move: consecutive PNGs differ only by a whole-pixel translation of the chart region (conductor compares with a small Python/PIL shift test); the number of distinct translations equals the number of distinct `llround(drag*2)` values.
3. Wheel zoom via `/input` across the 2 -> 3 px step: shots before/after show the band heights 2 then 3 everywhere; the price under the cursor (read from the axis labels) unchanged within a row.
4. Item origin check: log probe `raster.origin` (`mapToScene(0,0)*dpr`) once per size change; must be integer in the dock on the owner's Mac.
5. Owner at the Mac (judges feel, not screenshots): slow vertical and horizontal pans (no flicker, picture rigid), zoom through 2/3/4 px rows (stepped, no flip-flop), candles and axis labels locked to rows/columns, follow-live stepping, a 1m -> 15m -> 1m switch, auto price scale fits not cropped, the paper-trade drag line landing on the row under the cursor. Slice B look: candle body proportions at 3-20 px columns.
Evidence files go to `.claude/acting-orchestrator/pixel-raster/` (ignored), listed in the landing `Evidence:` trailer.

## 7. Risks, ranked; owner questions

1. **Drawn != stored after wheel/axis zoom (<= 25% at P = 2).** Mitigated for fits (1.4) and the controller view (1.8); remaining deviation is the accepted stepped zoom. Tests asserting stored bounds from pixels must read `drawn`.
2. **Sibling frame skew.** `CandlestickOverlayItem` reads `currentFrameContext()` in its own `updatePaintNode` (`:353`); if Qt syncs it before `UnifiedGridRenderer` in a frame, it draws last frame's camera: a visible 1 px desync during pans (today fractional, so unnoticed). QML declares the renderer first (`DepthChartView.qml:72, 95`); sync order is UNVERIFIED. Lab check 2 exposes it; fallback: sync candles from `afterSynchronizing` like `TradeBubbleOverlayItem` (`TradeBubbleOverlayItem.cpp:52-64`), or move candle geometry under the chart root (STATUS item 6a option).
3. **Item origin not on a device pixel** (fractional DPR on Windows, dock splitters): layers stay locked to each other (item-relative), absolute pixel alignment is lost; probe logs it. Owner's Mac is 2x with integer layout.
4. **Hot-file contention** (`UnifiedGridRenderer.cpp`, maybe `MainWindowGpu.cpp`): nobody else holds them; sequence after palette-texel.
5. **Auto tick after a fit** at the 1.25 preset ratio: r = 1.25*P_fit is integer only for P_fit % 4 == 0, else one re-round (<= 12.5%) in the frame after the fit.
6. **Candle look** (slice B): integer gaps replace the 0.7 proportion at small columns (owner question 1).
7. **`handlePanEnd` remainder logic** (`GridViewState.cpp:279-281` `m_panRemainderTimeMs`) with the drawn scale: keep the int64 remainder; A7 covers it.
8. **Precision** at float boundaries exactly on pixel edges: rasterization samples at pixel centres, so +-1e-12 at an edge cannot flip a pixel; A1 tolerance 1e-6.

Owner questions (defaults apply if unanswered):
- Q1 (slice B): candle body rule at small columns. Default: `gap = C >= 3 ? max(1, floor(0.15*C)) : 0` device px each side, body `>= 1 px`, min body height `lround(1.5*dpr)` device px.
- Q2: integer-step hysteresis band. Default 0.1 device px beyond the half-integer (2 -> 3 at r > 2.6, back at r < 2.4). Alternative 0.25 (2.75/2.25) if slow zooms still feel twitchy in the lab.

## 8. Roles

- Slice A writer: Claude `opus`. Reason: Metal GPU readback tests (B1-B8) and the hosted-GUI checks must run natively, the slice holds the hot file, and the frame-order change is cross-cutting. Reviewer: Codex `gpt-6-astra` at high (AGENTS 10: strongest other-provider model for GPU heatmap core). Budget (STATUS): Codex 59% with deficit, Claude 56%: one Opus writer plus one Astra review fits.
- Slice B writer: Codex `gpt-6-sol` (CPU geometry in QSG items; existing GPU tests cover alignment; the conductor attaches the native B3 shot and the owner's look). Reviewer: Claude Fable (Codex-written, high-risk by owner-look).

## 9. Split and intermediate correctness

Split into A then B as above. After A alone, every layer maps through the snapped mapping: candles are centred in their columns with snapped wicks (fractional bodies, as today), VP/TPO/footprint move rigidly with the picture (their own bin edges fractional, as today). No layer is misaligned at any intermediate state, so the split holds; B is a pure look change on top. Splitting rows from columns is rejected: one function produces both, a half-snapped mapping saves no risk and doubles the lab sitting.

### Critical Files for Implementation
- /Users/copeharder/Programming/Sentinel/libs/gui/UnifiedGridRenderer.Render.cpp
- /Users/copeharder/Programming/Sentinel/libs/gui/render/GridViewState.cpp
- /Users/copeharder/Programming/Sentinel/libs/gui/UnifiedGridRenderer.cpp
- /Users/copeharder/Programming/Sentinel/libs/gui/render/heatmap/HeatmapGpuLayer.cpp
- /Users/copeharder/Programming/Sentinel/tests/render/test_ugr_gpu.cpp

WORKFLOW: `main` is `62f1c6a`, not `4913fc1` as the brief said (docs-only difference). The brief's "candles bodies fractional" note hid the bigger silent consumers: the two axis models and `UnifiedGridRenderer::worldToScreen/screenToWorld` map from the raw `GridViewState`, not from `TimeAxisMapping`, and `DepthChartView.qml` has its own viewport math for grid lines; none of them appear in the verify.md evidence. The DisplayReadback harness exists only in the archive tag and the uncommitted palette-texel worktree, so the test plan had to be written against the archived source rather than main.
