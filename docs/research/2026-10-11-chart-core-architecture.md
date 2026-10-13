# Chart core architecture: stopping UnifiedGridRenderer from regrowing

Read-only plan. Base: `main` @ `180d3b7` (2026-10-09 tip of the UGR history). Every claim below has a file:line or commit; the claims I could not confirm are marked **UNVERIFIED**. Line numbers are at `180d3b7`; writers must find code by function name, since earlier slices move lines.

## Executive summary

1. UGR regrew because **view behaviour has no owner outside the QQuickItem**. Since 2026-09-27, 32 of the 54 non-mechanical UGR commits were view-policy work (camera, zoom, fit, follow-live, timeframe switch). They added about +2,660 lines to UGR. The overlay features (bubbles, candles, labels, TPO) added about +200, because they already had classes of their own.
2. Root causes: (a) the view state is split three ways: stored bounds in `GridViewState`, camera inputs and placement flags in UGR, market facts in `HeatmapGpuLayer`. Only UGR can see all three. (b) Earlier splits moved code into more partial `.cpp` files or render-side services. They never moved state. (c) `IOverlayRenderer` leaves dispatch and configuration in UGR on purpose. (d) UGR is the only object that QML, MainWindowGpu and the tests can reach.
3. Target: a thin `UnifiedGridRenderer` shell over a **Qt-Quick-free chart core**. The core is a static lib `sentinel_chart_core` that links only QtCore, so the build enforces the boundary. In it, a `ChartViewController` sequences small policies (camera, glide, zoom, price fit, live placement, timeframe switch, price carry) behind four ports.
4. Render side: one immutable `ChartFrame` per sync, built in `beforeSynchronizing`. Layers implement `IChartLayer` and register in `ChartLayerSet.cpp`. The shell never names a layer.
5. QML properties and the Agent API attach through forwarders, then through grouped sub-objects and a `ChartAgentPort`, not through new members on UGR.
6. Migration: 21 slices, one writer each, run in sequence because UGR.cpp is one branch at a time. Slice 0 adds a CPU view-trace golden that runs anywhere, a native Metal frame-hash golden, a ratchet gate and the dead-surface deletions. The behaviour fixes form a separate phase.
7. End state: `UnifiedGridRenderer.cpp` ≤ 450 lines, `.h` ≤ 350; every `libs/gui/chart/**` file ≤ 500; new functions at CCN ≤ 15 (controller functions ≤ 10).
8. Guardrails: `scripts/dev/chart-health.sh` in the land gate (file caps, a lizard ratchet, jscpd, an include boundary), plus a review checklist and a list of things a reviewer must reject.
9. Honest costs: UGR.cpp is frozen to other work for about 21 landings; there is a port with about 12 trivial getters; floating-point and slot-order drift is a risk, which byte-exact hex-float traces catch.
10. Owner decisions recorded 2026-10-11 (section 6): Q1, Q2, Q4, Q5 approved; Q3 partial (B1 approved, B2 blocked until the candle-interval spec in section 6 is decided). Revision 2 also applies three owner corrections: frame inputs are captured by value (2.5), the late-enable node-order test gates S11, and the Mac golden fails on a missing baseline and hashes explicitly serialized fields (S0b).

---

## 1. Why UGR regrew

### 1.1 The numbers

Total lines across `libs/gui/UnifiedGridRenderer*` at selected commits:

| Commit | Date | Event | Total | Split |
|---|---|---|---|---|
| `41c1159` | 2025-08-07 | "Finish refactor for UnifiedGridRenderer" | 1,141 | cpp 890, h 251 |
| `24a721d` | 2025-10-29 | "Reorganize and restructured functions in UGR" | 780 | cpp 535, h 245 |
| `f484bf3` | 2026-01-15 | legacy CPU path removed | 1,752 | cpp 1,421, h 331 |
| `8b8b776` | 2026-02-14 | **split into Init/Render TUs** | 2,044 | Init 473, Render 342, cpp 837, h 392 |
| `958e5e8` | 2026-03-28 | **extract UGR services, unify overlay interface** | 2,562 | Init 259, Render 580, cpp 1,306, h 417 |
| `35be10e` | 2026-10-01 | S6b GPU heatmap on the main chart | 3,087 | |
| `16e9300` | 2026-10-02 | auto price scale review round 3 | 3,742 | cpp 2,191, h 608 |
| `bdd5d34` | 2026-10-02 | S7b labels | 3,802 | (peak) |
| `8dd53cf` | 2026-10-06 | **legacy heatmap deleted (-1,435 UGR lines)** | 2,587 | cpp 1,618, h 536 |
| `a92f22f` | 2026-10-08 | smooth zoom A2: new partial `Zoom.cpp` | 3,282 | Zoom 371 |
| `e5020c1` | 2026-10-09 | current | 3,630 | cpp 1,844, Zoom 599, Render 307, Init 192, h 688 |

Each of the three "splits" was followed by growth past the pre-split size. The legacy deletion took 1,435 lines out, and three days of zoom work put 1,043 back (`8dd53cf` → `e5020c1`).

### 1.2 What kind of change lands in UGR

`git log --numstat origin/main --since=2026-09-27 -- 'libs/gui/UnifiedGridRenderer*'`, with the LF-normalisation commit `21d41b8` excluded and commits classified by subject (approximate):

| Kind | Commits | UGR + | UGR − |
|---|---|---|---|
| View policy (camera, zoom, fit, follow-live, timeframe, seed) | 32 | **2,663** | 937 |
| Heatmap integration (S6, legacy deletion, recording) | 13 | 884 | 1,766 |
| Layers and overlays (bubbles, candles, labels, TPO) | 6 | **201** | 240 |
| Other | 3 | 133 | 218 |

19 of the 57 commits in that window are titled "review", "fix round" or "fixes".

The control experiment is in the same window. Features that **had a home** put little into UGR; features that **had none** put most of their code into UGR:

| Commit | Feature | UGR + | Other `libs/` + | Home it used |
|---|---|---|---|---|
| `5b160dd`, `bd90d35`, `e4cc3bf` | trade bubbles | 25, 19, 29 | 294, 218, 270 | `TradeBubbleOverlayItem`, `TradeBubbleData` |
| `ab86853` | candle look | 30 | 275 | `CandlestickOverlayItem` |
| `34bce77` | S7b liquidity labels | 37 | 537 | `HeatmapLabelLayout`, `HeatmapGpuLayer` |
| `c0c995a` | TPO profiles | 84 (−209) | 1,283 | `TpoOverlayRenderer`, `TpoProfileModel` |
| `16a6658` | whole-pixel slice B (overlay snapping) | 3 | 143 | the overlays themselves |
| `a92f22f` | smooth zoom A2 | **518** | 263 | none: the glides went into UGR (`h:182-262`) |
| `9446c6f` | A2 fix round 2 | **237** | 35 | none |
| `3ff20ed` | zoom by spans, auto-fit | **292** | 165 | none |
| `29bab1f` | auto price scale model | **188** | 118 | none |
| `9b2627d` | whole-pixel slice A | **247** | 418 | the pure math went to `ChartRaster`; the integration went into UGR |

The whole-pixel case is the clearest. The part that had a Qt-free home (`chart_raster::computeRaster`, `ChartRaster.hpp:220`) went there and is unit-tested by `test_ChartRaster` (774 lines). Everything that needed the stored view, the tick, the size, the DPR and the raster step went into UGR, because only UGR holds all of them.

### 1.3 Structural causes

**C1. The view state is split three ways, and only UGR sees all three.**
- `GridViewState` holds the bounds, follow-live, auto price scale, raster anchor, span limits and the drag (`GridViewState.hpp:109-136`).
- UGR holds the timeframe (`UnifiedGridRenderer.h:116`), five placement flags (`m_gpuPriceKnown`, `m_gpuReseedPrice`, `m_gpuViewPristine`, `m_gpuSelfViewport`, `m_gpuLimitsDeferred`, `h:139-145`), the raster step and its mutex (`h:168-173`), both axis glides and the gesture state (`h:185-221`), the follow padding (`h:206-210`), the price carry (`h:653-654`) and the live prices (`h:660-661`).
- `HeatmapGpuLayer` holds the drawn tick, the span limits, the live anchor and the price scale (`HeatmapGpuLayer.hpp:113-150`).

Any view feature needs all three, so it lands in UGR. The same cause explains the callback knot. `GridViewState` cannot evaluate a camera, so UGR installs `PriceFit` and `PanShift` hooks (`Init.cpp:110-117`). Those hooks call back into UGR, which reads `GridViewState` again (`gpuFitPriceWindow`, `cpp:1204`). The audit rejected class extraction because of "a host interface with about 10 callbacks" (audit-ugr.md §6). That cost is a symptom of C1, not a law. Move the state together with its policy and most of those callbacks disappear.

**C2. The earlier splits moved code, never state.**
- `8b8b776` split UGR into `Init.cpp` and `Render.cpp`. They are member functions of the same class.
- `130e58a` (`UgrFrameMath`) and `958e5e8` (`FrameContext`, `AxisTextService`, `HeatmapStreamService`) moved render-side services.
- A2 added a fourth partial, `Zoom.cpp` (`a92f22f`).

The header is shared, so every new behaviour still adds `m_` fields to `UnifiedGridRenderer.h` (392 lines in Feb, 688 now). One extraction did own view state: `ViewportAutoScrollController`. It was typed against the legacy `HeatmapStreamState` (its `updateLagFromView`/`applySliceAutoScroll` took `const HeatmapStreamState&`, see `git show 8dd53cf^:libs/gui/render/ViewportAutoScrollController.hpp`), so it died with the legacy heatmap (`8dd53cf`). The GPU-era follow-live, seed and bootstrap code was written straight into UGR instead (`followGpuLive`, `seedGpuViewport`, `bootstrapGpuTimeView`: `35be10e`, `ff2248a`, `0cac0dc`). A controller tied to one data source is not a durable home.

**C3. The overlay contract keeps dispatch and configuration in UGR on purpose.** `IOverlayRenderer.hpp:3-6`: "without imposing a virtual render() method… concrete dispatch stays in UnifiedGridRenderer". So each layer adds four things to UGR:
- a member (`h:292-297`);
- data-intake wiring (`Init.cpp:138-192`);
- a dispatch call with its own parameter list (`Render.cpp:44-77`);
- QML-facing setters (`cpp:397-469`, TPO).

`zOrder()` is declared but not used for ordering: draw order is call order (footprint, VP, TPO at `Render.cpp:50-76`), even though VP declares 3 and TPO declares 2.

**C4. UGR is the only object anyone can reach.**
- QML binds 44 distinct UGR members (`DepthChartView.qml`).
- `QmlSceneController::getUnifiedGridRenderer` finds UGR by object name (`QmlSceneController.cpp:120-122`). MainWindowGpu then calls about 40 distinct UGR methods, 12 of them `gpuHeatmapLayer()`.
- `AxisModel` (`AxisModel.cpp:18`) and `TradeBubbleOverlayItem` (`TradeBubbleOverlayItem.cpp:43`) `qobject_cast` to UGR.
- Tests reach in through 116 `getViewState()` calls, test hooks on UGR (`h:505-518`) and a friend struct that touches private members (`test_heatmap_settings_ui.cpp:63-69`).

So every new knob, test hook or API field becomes a UGR member by default.

**C5. Process: fix rounds patch the biggest functions, and nothing measures them.** Today (lizard 1.24.1):
- `gpuFitPriceWindow`: CCN 55, 114 lines (`cpp:1204-1317`)
- `setTimeframe`: CCN 41, 130 lines (`cpp:517-646`)
- `zoomClicks`: CCN 40 (`Zoom.cpp:332-400`)
- `zoomContinuousFor`: CCN 34
- `applyViewportRequest`: CCN 26

No size or complexity gate exists (no CI; `agent-worktree.sh land` runs build + ctest only, `:134`). The legacy deletion (−1,435) made the file look healthy for three days.

**Summary.** The QQuickItem is the only type where these four things meet: the stored view, the camera inputs, the heatmap facts and the outside world (QML, MainWindow, tests). Each new chart behaviour needs that meeting point, and nothing else offers it.

---

## 2. Target architecture

### 2.1 Shape

```
                 QML (DepthChartView.qml)        MainWindowGpu / Agent API
                        │  properties/invokables          │ ChartSessionPort, ChartAgentPort
                        ▼                                  ▼
 ┌──────────────── UnifiedGridRenderer  (QQuickItem shell, libs/gui) ────────────────┐
 │ lifecycle · window binding · updatePolish/updatePaintNode · signal fan-out ·      │
 │ thin Q_PROPERTY forwarders · owns the parts below, holds no chart logic           │
 └───┬─────────────────────┬──────────────────────┬─────────────────────┬────────────┘
     │ GUI thread          │ render thread (sync)  │                     │
     ▼                     ▼                       ▼                     ▼
 ChartViewController   FrameProducer ──► ChartFrame ──► LayerRegistry ──► IChartLayer…
 (chart core, QtCore)   (chart core)     (immutable)    (libs/gui/chart/layers)
  ├ ChartCamera            │                              HeatmapLayer, FootprintLayer,
  ├ ZoomGlide              └──► PublishedFrame ◄── siblings (candles, bubbles, algo,
  ├ ZoomController               (mutex)              paper) via IChartFrameSource
  ├ PriceFitter
  ├ LivePlacement           ports (implemented in the shell by HeatmapChartPort):
  ├ TimeframeSwitch (pure)   IChartEnvironment · ITickOracle · ICandleSource · IHeatmapViewSink
  └ PriceCarry
      owns GridViewState (stored continuous view)
```

### 2.2 Modules: one reason to change each

| Module | Location / target | Owns | Single reason to change | Today at |
|---|---|---|---|---|
| `ChartRaster` (exists) | `render/ChartRaster.*` → compiled into `sentinel_chart_core` | pure camera math | whole-pixel mapping rules | unchanged |
| `GridViewState` (exists) | `render/GridViewState.*` → core | stored continuous view, limits, anchor, drag | stored-view semantics | unchanged |
| `ChartPorts.hpp` | `chart/core/` | interfaces only | what the core needs from outside | new |
| `PublishedFrame` | `chart/core/` | last `MappingFrameContext` + `RasterStep` + the mutex | the GUI↔render publication contract | `h:168-173`, `Render.cpp:16-41`, `cpp:1661-1668` |
| `ChartCamera` | `chart/core/` | nothing persistent; computes rest/drawn/intended cameras, drawn time window, pan shift, tick predictor | how a stored view becomes a camera | `Zoom.cpp:36-150`, `Render.cpp:95-112`, `cpp:889-914, 1179-1202` |
| `ZoomGlide` | `chart/core/` | `AxisGlide` ×2, rung targets, rebase counter | transition animation | `h:185-201`, `Zoom.cpp:152-271` |
| `ZoomController` | `chart/core/` | gesture state, settle timer (QTimer is QtCore), wheel carry | zoom input semantics (clicks, gestures, notch rule) | `Zoom.cpp:273-571` |
| `PriceFitter` | `chart/core/` | fit scratch (`m_fitBars`) | price and time fit rules (auto, manual, one-price) | `cpp:1103-1341` |
| `LivePlacement` | `chart/core/` | `FollowPadding`, the bootstrap timer, pristine/self flags | where the view sits relative to live | `cpp:916-998, 1035-1099` |
| `TimeframeSwitch` | `chart/core/` (free functions) | nothing | the Now-column keep rule | `cpp:517-646` |
| `PriceCarry` | `chart/core/` | `PriceCarry`, the wait timer | symbol-switch price carry | `cpp:343-393, 1000-1033` |
| `ChartViewController` | `chart/core/` | `GridViewState`, `ChartSession` (symbol, tf, live prices), placement flags, the policies above | **sequencing only**: which policy runs for which command or event | everything above, plus `cpp:1343-1487, 1739-1844`, `Init.cpp:35-66, 100-120` |
| `FrameProducer`, `ChartFrame` | `chart/core/` | the per-sync frame | what a frame contains and how it is derived | `Render.cpp:114-234`, `FrameContext.hpp`, `h:266-274` |
| `IChartLayer`, `LayerRegistry`, `ChartLayerSet.cpp` | `chart/layers/` (QtQuick) | layer list and order | which layers exist, in what order | `Render.cpp:44-93, 236-280`, `Init.cpp:71` |
| `HeatmapLayer` | `chart/layers/` | the gate node, label layout state, signature atomics | heatmap drawing and labels | `Render.cpp:80-93, 161-196, 240-253`, `h:276-284` |
| `ChartTextLayer` | `chart/layers/` | atlas, text renderer, axis text submit | chart text | `cpp:1501-1552`, `Render.cpp:266-273` |
| `TradeOverlayFeeds` | `chart/feeds/` | DataProcessor + its thread; enqueue into the layers | trade-overlay data intake | `Init.cpp:81-95, 122-125, 138-192`, `cpp:127-155` |
| `TradeBubbleFeed` | `chart/feeds/` | tape, settings, render frame | bubble publication | `h:286-291`, `Render.cpp:282-288`, `cpp:157-162` |
| `LayerVisibility` | `chart/shell/` | the four layer flags + `primaryField` | exclusion rules between layers | `cpp:471-494, 702-806` |
| `ChartLookSettings` | `chart/shell/` | tone, candle look, TPO config | look knobs | `cpp:397-469, 648-700` |
| `ChartInputRouter` | `chart/shell/` | nothing | Qt event → core command translation | `cpp:1591-1635`, `Zoom.cpp:575-599` |
| `HeatmapChartPort` | `chart/shell/` | non-owning `HeatmapGpuLayer*`, item pointer | the adapter between the ports and the heatmap layer and item | new |
| `UnifiedGridRenderer` | `libs/gui/` (QML type unchanged) | the parts above | Qt Quick item lifecycle and wiring | the rest |

Design rules that make this durable, not only smaller:
- **State lives with the policy that changes it.** No policy holds a pointer to UGR. Policies get the stored view, value structs and ports.
- **The controller only sequences.** Its functions call policies and emit. Branching on prices, times or pixels belongs in a policy. Budget: CCN ≤ 10 per function, file ≤ 500 lines.
- **The core is QtCore-only, and the build checks it.** `sentinel_chart_core` links `Qt6::Core` and `sentinel_core` only, so any `<QQuick…>`, `<QSG…>`, `<QtGui…>` or `QQuickWindow` include fails to compile. AGENTS.md §1 already sets this rule for `libs/core`; the chart core adopts it inside `libs/gui`. `ChartRaster`, `GridViewState`, `TimeAxisMapping`, `TimeAuthority` and `FrameContext` already include only QtCore headers (checked). `FrameContextBuilder` takes a `QQuickWindow*` only to read the DPR (`FrameContextBuilder.cpp:38`); it changes to take `double dpr`.
- **The policies do not depend on the data source.** This is the lesson of C2. The core sees heatmap facts through ports, never through `HeatmapGpuLayer`, the source controller or recording types.

### 2.3 Ports (the only way the core sees the outside)

```cpp
// libs/gui/chart/core/ChartPorts.hpp  (QtCore only)
#pragma once
#include "render/heatmap/HeatmapBinGrid.hpp"   // heatmap::gpu::ViewWindow (Qt-free; checked)
#include "heatmap/HeatmapResolution.hpp"       // heatmap::AutoTickParams (libs/core)
#include <cstdint>
#include <optional>
#include <vector>

namespace chart {

struct SurfaceFacts { double widthLogical = 0, heightLogical = 0, dpr = 1; };
struct SpanLimits   { double minTimeMs = 0, maxTimeMs = 0, minPrice = 0, maxPrice = 0; };

// Pulled on demand on the GUI thread (no cached snapshot: identical call timing to today).
class IChartEnvironment {
public:
    virtual ~IChartEnvironment() = default;
    virtual SurfaceFacts surface() const = 0;          // width(), height(), window DPR (D3: one place)
    virtual double drawnTick() const = 0;              // HeatmapGpuLayer::tickPrice
    virtual double priceScale() const = 0;
    virtual bool manualTick() const = 0;
    virtual bool hasCurrentResolution() const = 0;
    virtual bool priceScaleCurrent() const = 0;
    virtual SpanLimits limits() const = 0;             // min/max time/price spans
    virtual int64_t liveAnchorMs() const = 0;
    virtual int64_t liveOpenEndMs() const = 0;
    virtual int64_t oldestAvailableMs() const = 0;
    virtual double recentMidPrice() const = 0;
};

class ITickOracle {                                    // Auto/Manual tick questions
public:
    virtual ~ITickOracle() = default;
    virtual int64_t predictTickUnits(const heatmap::gpu::ViewWindow&) const = 0;
    virtual bool buildsTick(int64_t units, const heatmap::gpu::ViewWindow&) const = 0;
    virtual int64_t autoTickState() const = 0;
    virtual heatmap::AutoTickParams autoTickParams() const = 0;  // {minRowPx, hysteresis}
    virtual bool setAutoPriceTick(std::optional<int64_t> units) = 0; // true: request a frame
};

class IHeatmapViewSink {                               // the controller plans for the drawn window
public:
    virtual ~IHeatmapViewSink() = default;
    virtual void setView(const heatmap::gpu::ViewWindow& drawn, bool priceKnown) = 0;
};

struct CandleBar;                                      // = CandleSeriesBuffer::CandleBar (alias)
class ICandleSource {
public:
    virtual ~ICandleSource() = default;
    virtual void visibleBars(const QString& symbol, int64_t tfSec, int64_t start, int64_t end,
                             std::vector<CandleBar>& out) const = 0;
    virtual int64_t oldestTimeMs(const QString& symbol, int64_t tfSec) const = 0;
};

} // namespace chart
```

`HeatmapChartPort` (in `chart/shell/`) implements the first three with one-line forwarders to `HeatmapGpuLayer` and the item. A `CandleBufferSource` adapts `CandleSeriesBuffer`, which is QtCore and could be used directly; the interface exists so policy tests can feed fixed bars. That is 12 + 5 + 1 + 2 trivial methods. This is the honest cost of the design, and it replaces the audit's "about 10 callbacks": these are getters with no re-entrancy, not callbacks.

### 2.4 Core sketches

```cpp
// chart/core/PublishedFrame.hpp — the one GUI↔render publication point
class PublishedFrame {
public:
    MappingFrameContext current() const;                 // any thread (sibling items' sync, GUI)
    chart_raster::RasterStep step() const;               // the hysteresis state (GUI and render)
    void publish(const MappingFrameContext&);            // render thread, GUI blocked
    void setStep(chart_raster::RasterStep);              // render thread, GUI blocked
private:
    mutable std::mutex mutex_;                           // today's m_frameContextMutex (h:168)
    MappingFrameContext last_;
    chart_raster::RasterStep step_;
};

// chart/core/ChartCamera.hpp — every "viewportSnapshot → rasterInputs → computeRaster" (D1, D2, D3, D22)
struct BinView { double timeLo = 0, timeHi = 0, priceLo = 0, priceHi = 0; };
struct Cameras { chart_raster::RasterCamera rest, drawn; std::optional<BinView> binView; };

class ChartCamera {
public:
    ChartCamera(const GridViewState&, const ChartSession&, const IChartEnvironment&,
                const ITickOracle&, const PublishedFrame&, const ZoomGlideState&);
    chart_raster::RasterInputs inputs(const FrameViewportSnapshot&, double tick, SurfaceFacts) const;
    Cameras camerasFor(const chart_raster::RasterInputs&, chart_raster::RasterStep previous) const;
    chart_raster::RasterCamera drawnNow(bool includeDrag = true) const;   // = rasterCameraNow
    chart_raster::RasterCamera restNow(bool includeDrag = true) const;    // = restCameraNow
    chart_raster::RasterCamera intendedEndNow() const;
    std::pair<double, double> drawnTimeWindow(int64_t start, int64_t end, std::optional<double> fracX) const;
    bool panShift(QPointF drag, int64_t& timeShiftMs, double& priceShift) const; // GridViewState::PanShift
    chart_raster::TickAt tickPredictor(int64_t start, int64_t end, double anchorPrice, double fracY) const;
};

// chart/core/ZoomGlide.hpp
struct AxisGlide { /* verbatim from h:185-196 */ };
struct ZoomGlideState {
    AxisGlide time, price;
    bool gesture = false, gestureTime = false, gesturePrice = false;   // read by camerasFor
    int rebases = 0, colPx = 0; chart_raster::RowRung row;
};
class ZoomGlide : public QObject {                       // QtCore: signals only
    Q_OBJECT
public:
    void start(const chart_raster::RasterCamera& drawn, const chart_raster::RasterCamera& restBefore,
               double anchorTime, double anchorPrice, bool forceTime, bool forcePrice);
    void advance(int64_t nowMs);                          // from updatePolish
    void end(bool time = true, bool price = true);
    void shiftBoth(double dxDev, double dyDev);           // pan release (cpp:1800-1820)
    void shiftTime(int64_t worldMs);                      // follow-live step (cpp:948-953)
    const ZoomGlideState& state() const;
    bool gliding() const;
signals:                                                  // connected DirectConnection, same order as today
    void rasterChanged(); void viewportChanged(); void polishRequested(); void updateRequested();
};

// chart/core/TimeframeSwitch.hpp — pure, unit-testable (the 130-line setTimeframe solve)
struct TimeframeSwitchInputs {
    int64_t previousTfMs, nextTfMs, oldStart, oldEnd, liveAnchorMs;
    bool following, keepColumns; double maxTimeSpanMs;
    chart_raster::RasterCamera drawnBefore;
    std::function<chart_raster::RasterCamera(int64_t end, int64_t span)> restAt;  // camera probe
    std::function<int64_t(int64_t span)> nominalLiveEnd;                           // gpuLiveEndMs
};
struct TimeframeSwitchPlan {
    int64_t start = 0, end = 0; double anchorFracX = 1.0;
    std::optional<FollowPadding> followPadding;           // FollowPadding moves to LivePlacement.hpp
};
TimeframeSwitchPlan planTimeframeSwitch(const TimeframeSwitchInputs&);

// chart/core/ChartViewController.hpp — the chart's GUI-thread brain; sequencing only
struct ChartSession { QString symbol; int64_t timeframeMs = 100; double bookMid = 0, lastTrade = 0; };
struct PlacementFlags { bool priceKnown = false, reseedPrice = false, pristine = false,
                        selfViewport = false, limitsDeferred = false; };

class ChartViewController : public QObject {
    Q_OBJECT
public:
    ChartViewController(IChartEnvironment&, ITickOracle&, IHeatmapViewSink&, PublishedFrame&,
                        QObject* parent = nullptr);
    void setCandleSource(const ICandleSource*);
    GridViewState& view();                                // tests and AxisModel (via the shell)
    const ChartSession& session() const;
    // Commands (QML / API / MainWindow, through the shell)
    void setViewport(int64_t s, int64_t e, double lo, double hi);
    QString applyViewportRequest(const ViewportRequest&);
    bool fitView(bool time, bool price);  bool resetView();
    void setAutoPriceScale(bool);  void enableAutoScroll(bool);
    void setTimeframe(int tfMs);  void setActiveSymbol(const QString&);
    void zoomClicks(int, double x, double y, bool time, bool price);
    void zoomContinuous(double f, double x, double y, bool time, bool price, int settleMs);
    void endZoomGesture();  void wheel(const WheelInput&);
    void beginPan(QPointF); void updatePan(QPointF); void endPan(); void pan(PanDirection);
    // Events (from the heatmap port and the session feeds)
    void onHeatmapSnapshot(); void onHeatmapLive(); void onHeatmapTick(); void onHeatmapLimits();
    void onBookTop(double bid, double ask); void onTrade(const QString& symbol, double price);
    void onSurfaceChanged(SurfaceFacts); void advanceFrame(int64_t nowMs);  // updatePolish
    // Render thread, GUI blocked: everything the frame needs, by value
    FrameInputs frameInputs() const;
    // Read-only views for the facade and the Agent API
    ViewStateSnapshot snapshot() const;
signals:  // the shell re-emits these 1:1 with direct connections (order preserved)
    void viewportChanged(); void rasterChanged(); void liveRenderTick(); void timeframeChanged();
    void autoScrollEnabledChanged(); void autoPriceScaleChanged(); void heatmapTickSizeChanged();
    void updateRequested(); void polishRequested();
private:
    std::unique_ptr<GridViewState> view_; ChartSession session_; PlacementFlags flags_;
    ZoomGlide glide_; ChartCamera camera_; ZoomController zoom_; PriceFitter fit_;
    LivePlacement live_; PriceCarryPolicy carry_;
};
```

Neutrality note. During the migration, everything that emits does it **through Qt signals connected `Qt::DirectConnection`**, never by returning effect structs. Today `startAxisGlides` emits `rasterChanged`, `viewportChanged`, then calls `polish()`, then `update()`, in the middle of the function (`Zoom.cpp:190-193`), and listeners read state synchronously. An effects-return design would move those emissions to the end of the command and change what listeners see. The controller may move to effect structs later, in a non-neutral slice with its own review.

### 2.5 Threads, the frame snapshot and the sync handoff

| Thread | Runs | May read | May write |
|---|---|---|---|
| GUI | the controller and all policies, `GridViewState`, layer controllers (settings, enqueue), `updatePolish` → `advanceFrame` | everything GUI-side; `PublishedFrame::current()/step()` | GUI-side state; layer mailboxes |
| Render, inside sync (GUI blocked): `beforeSynchronizing` → items' `updatePaintNode` | `FrameProducer::produce`, `IChartLayer::sync/updateNodes`, `HeatmapGpuLayer::chooseTickForView/prepareFrame/labelsForFrame` | `controller.frameInputs()` (by value), layer mailboxes, `ChartFrame` | `PublishedFrame` (publish, step), layer-owned render state, `FrameAck` pending ids |
| Render, outside sync: `afterRendering` | `FrameAck::rendered`, per-layer `afterRendering()` | atomics only | atomics; queued `invokeMethod` to the GUI |

The frame, per sync:
1. `beforeSynchronizing` (`UnifiedGridRenderer.cpp:97-98`): `FrameProducer::produce(controller.frameInputs(), tickAuthority)`. This builds the `ChartFrame`:
   - the surface (the device-integer bounds, `FrameContextBuilder.cpp:41-44`);
   - the `TimeAuthority` snapshot;
   - the stored viewport snapshot;
   - the tick: heatmap `chooseTickForView` on the continuous view (`Render.cpp:222-227`, the "D21" parallel mapping, kept as is);
   - the cameras (`camerasFor`) and `toMapping(drawn)`;
   - the transition (`coverageGamma`, `binView`);
   - layer activation;
   - frame ids.

   Then it publishes to `PublishedFrame` and writes the raster step. It announces a raster change with the queued `rasterChanged` + `viewportChanged` (`Render.cpp:145-158`).
2. UGR `updatePaintNode` (sync): it stamps the control revisions (`Render.cpp:302-305`). Then `LayerRegistry::updateNodes(frame, ctx)` runs in registry order: heatmap gate, footprint, VP, TPO, then text (axis + labels). That order is exactly `Render.cpp:245-273` today.
3. Sibling items (candles, algo, paper, trade bubbles) sync in whatever order Qt chooses. They read `PublishedFrame::current()` through `IChartFrameSource` (today `ITimeAxisMappingProvider::currentFrameContext`). That is safe because step 1 published before any item synced (`Render.cpp:198-202`).
4. `afterRendering`: `FrameAck` copies the pending ids to the rendered atomics (`cpp:110-111`). The layers' `afterRendering()` hooks (heatmap `wantsFrame`, labels incomplete, `cpp:114-121`) post queued updates.

```cpp
// chart/core/ChartFrame.hpp — immutable once produced; layers get const&
struct FrameIds { uint64_t controlRevision = 0, selectionEpoch = 0, viewportVersion = 0, frameId = 0; };
struct ZoomTransition { float coverageGamma = 0.f; std::optional<BinView> binView; };
struct LayerActivation { bool heatmap = false, footprint = false, tpo = false, volumeProfile = false; };
struct ChartFrame {
    FrameIds ids;
    QRectF surfaceBounds; double dpr = 1.0;
    TimeAuthority::Snapshot time; qint64 presentationTimeMs = 0;
    FrameViewportSnapshot stored;                 // committed view, drag, anchor
    chart_raster::RasterCamera rest, drawn;       // drawn == what every layer maps through
    TimeAxisMapping mapping;                      // toMapping(drawn)
    ZoomTransition transition;
    LayerActivation layers;
    bool drawHeatmap = false;
};
struct FrameInputs {                              // by value from the controller (GUI blocked)
    FrameViewportSnapshot viewport; int64_t timeframeMs; ZoomGlideState glide;
    LayerActivation layers; SurfaceFacts surface; QRectF bounds;
    CameraInputs camera;                          // values copied out of GridViewState/ChartSession/IChartEnvironment
};
class ITickAuthority { public: virtual ~ITickAuthority() = default;
    virtual void chooseTickForView(const heatmap::gpu::ViewWindow&) = 0; virtual double tick() const = 0;
    virtual void noteRaster(int rowPx, int colPx, double rowPxContinuous) = 0; };
class FrameProducer {
public:
    // Render thread, GUI blocked. No allocation (today's guarantee, Render.cpp:116, 202).
    // Every input is a value in FrameInputs. No reference into GUI-owned state.
    const ChartFrame& produce(const FrameInputs&, ITickAuthority&, PublishedFrame&);
};
```

Race rules (written into the layer contract):
- **Owner correction (2026-10-11): render-side camera math consumes captured inputs only.** `ChartCamera` as sketched in 2.4 holds references to GUI-owned `GridViewState`, `ChartSession` and `IChartEnvironment`; that object stays GUI-side. The render side calls pure functions (`chart::camerasFor(const CameraInputs&)`, `chart::toMapping(...)`) over `FrameInputs::camera`, which `ChartViewController::frameInputs()` fills by value while the GUI is blocked. `FrameProducer` and every `IChartLayer` must not hold or follow a reference into GUI-owned state.
- The render thread reads GUI-side state only inside sync, and only through `frameInputs()` or a layer mailbox. Never through `GridViewState` or a `QObject` graph. AGENTS.md §2 already states the QObject rule.
- A layer copies what it needs from `ChartFrame` and keeps no pointer into it past `updateNodes`.
- Shared payloads cross threads as `shared_ptr<const T>`: palette, label cells, the bubble tape frame.
- Anything read in `afterRendering` is an atomic.

### 2.6 The layer contract

```cpp
// chart/layers/IChartLayer.hpp
struct LayerNodeContext {
    QQuickWindow* window;                 // textures, materials
    QSGNode* root;                        // append-only; a layer owns only the nodes it appended
    ChartTextRenderer& text;              // submit glyphs (priority) — text is drawn by ChartTextLayer
    const ChartTextAtlas& atlas; bool atlasReady;
    FrameProfiler* profiler;              // may be null
};
class IChartLayer {
public:
    virtual ~IChartLayer() = default;
    virtual std::string_view name() const = 0;                 // logs, probes, Agent API layer state
    virtual void onRootRebuilt() = 0;                          // node pointers are stale
    virtual void sync(const ChartFrame&) {}                    // render thread, GUI blocked: drain mailbox
    virtual void updateNodes(const ChartFrame&, LayerNodeContext&) = 0;  // render thread, GUI blocked
    virtual void afterRendering() {}                           // render thread, GUI running: atomics only
};
// GUI → render handoff for a layer's data and settings (the existing enqueue/drainPending pattern, named)
template <class T> class LayerMailbox {
public:
    void post(T value);                   // GUI thread
    bool take(T& out);                    // render thread (sync); false: nothing new
    void clear();
private:
    std::mutex mutex_; std::optional<T> pending_;
};
// chart/layers/LayerRegistry.hpp
class LayerRegistry {
public:
    void add(IChartLayer*);               // order of add == draw order (zOrder() is dropped: unused today)
    void onRootRebuilt();
    void updateNodes(const ChartFrame&, LayerNodeContext&);   // marks profiler stages between layers
    void afterRendering();
};
// chart/layers/ChartLayerSet.cpp — the ONE place that lists layers
void registerChartLayers(LayerRegistry& r, ChartLayers& owned);  // heatmap, footprint, vp, tpo, text
```

How each layer plugs in:

| Layer | Kind | Consumes | Data intake (GUI) | Notes |
|---|---|---|---|---|
| Heatmap (`HeatmapLayer`) | in-root, first | `frame.drawn` window, `transition`, `drawHeatmap`; owns the gate node | `HeatmapGpuLayer` (snapshots from the source controller) | Also the frame's `ITickAuthority`, and the place labels are laid out (`Render.cpp:161-196`) |
| Footprint, VP, TPO | in-root | `frame.mapping`, `frame.drawn`, `surfaceBounds`, `dpr`, axis snap | `TradeOverlayFeeds` → each layer's mailbox | Thin adapters over the existing renderers; renderer classes unchanged |
| Axis text + labels (`ChartTextLayer`) | in-root, last | `frame` + `AxisTextService` | atlas built once | Must stay last: low-priority label glyphs go after axis text (`Render.cpp:266-273`) |
| Candles, algo, paper trade | sibling `QQuickItem` | `IChartFrameSource::currentFrameContext()` | their own models | Unchanged; documented as the sibling kind (QML z interleave) |
| Trade bubbles | sibling, `afterSynchronizing` link | `TradeBubbleFeed::renderFrame()` | `TradeBubbleFeed` | `TradeBubbleOverlayItem` stops casting to UGR and takes `ITradeBubbleFrameSource` |
| Hit testing | GUI thread | `PublishedFrame::current().mapping` inverses | n/a | No caller exists today (`worldToScreen`/`screenToWorld` are dead, audit-ugr §2b). A layer that needs picking adds a GUI-side `hitTest(QPointF, const MappingFrameContext&)` on its own controller |

**Adding a new overlay without touching UGR:**
1. Write `chart/layers/FooLayer.{hpp,cpp}` implementing `IChartLayer`, and if it needs data, `FooFeed` posting to a `LayerMailbox`.
2. Add one line to `ChartLayerSet.cpp` (position = draw order) and one activation flag to `LayerActivation` (filled by `LayerVisibility`).
3. Expose settings on `FooLayerSettings : QObject`, reachable from QML as `unifiedGridRenderer.layers.foo` after S15 (until then, a forwarder in `UnifiedGridRenderer.Qml.cpp`, the one allowed facade file).
4. Test it against a hand-built `ChartFrame`. No UGR instance is needed for geometry tests.

### 2.7 The QML facade and the Agent API

- **Phase 1 (neutral):** QML keeps the same names. All `Q_PROPERTY` and `Q_INVOKABLE` members stay on UGR as one-line forwarders, in the header plus `UnifiedGridRenderer.Qml.cpp`. No forwarder may contain logic. Lint rule: every forwarder body is a single statement.
- **Phase 2 (S15, recommended; it changes QML paths, which is mechanical but visible to tests):** grouped attached objects, owned by their modules and exposed as `CONSTANT` `QObject*` properties:
  - `view` (`ChartViewQml`): `autoPriceScale`, `autoScrollEnabled`, `timeframeMs`, zoom/pan/fit/reset;
  - `candles` (`ChartLookSettings`);
  - `tpo`;
  - `debug`;
  - `layers` (`LayerVisibility`).

  A new knob goes on the object of the module that owns it. UGR's own property list stops changing.
- **Agent API:** `ChartAgentPort` (in `mainwindow/`, next to `AgentApiCodec`) reads `ChartViewController::snapshot()` (stored bounds, `followLive`, `autoScale`, `viewportVersion`, size, timeframe) and `PublishedFrame::current().raster` (`drawn`). It applies `ViewportRequest` through the controller. `FrameAck` owns the control-revision handshake (`h:301-318`, `Render.cpp:254-255`, `cpp:110-111`). This replaces `MainWindowGpu::agentApiViewportSnapshot` (`MainWindowGpu.cpp:1740-1768`) and the renderer reach at `:662-680, :1848`. Adding a field to `GET /viewport` touches the port and the codec, not UGR.

### 2.8 Boundaries with HeatmapSourceController and MainWindowGpu

- `HeatmapDataService` and `HeatmapSourceController` stay behind `HeatmapGpuLayer`. The chart core never includes them, or `HeatmapGpuLayer.hpp`. It sees the facts (`IChartEnvironment`), the tick questions (`ITickOracle`) and the view sink (`IHeatmapViewSink`). Today `syncGpuView` posts the drawn window (`cpp:876-887`); it becomes `IHeatmapViewSink::setView`. The source-controller work in the heatmap audit (slices 5-9) is independent and never touches UGR.
- MainWindowGpu keeps ownership of the service, the settings model and store, and `HeatmapChartControls`. It talks to the chart through three narrow ports instead of about 40 methods:
  - `ChartSessionPort`: symbol, timeframe, book top, trades, data processor access;
  - `ChartSettingsPort`: heatmap settings and tick memory, candle look, TPO config, service injection; `gpuHeatmapLayer()` becomes `heatmapLayer()` on this port;
  - `ChartAgentPort` (above).
- Hot-file rule: MainWindowGpu changes are limited to S15 (and one-line call-site renames in other slices if unavoidable). Coordinate with heatmap-audit slice 10 (`MainWindowAgentApi.cpp`). Do one, then the other, not both at once.

---

## 3. Alternatives considered

| Design | What it is | Why not (or why) | Cost |
|---|---|---|---|
| **A. One class, stricter partials** (audit N5-N8) | Dedupe helpers, more per-job `.cpp` files (Camera/Fit/Live/Layers/Input/Debug), header regroup | Cheapest and lowest risk, but it repeats `8b8b776` and `a92f22f`: state stays on the class, so the next feature adds `m_` fields and test hooks to the one header. It fixes file size, not C1/C3/C4. Parallel work on two jobs still collides in `UnifiedGridRenderer.h` | ~8 slices |
| **B. Thin shell + QtCore chart core with policies (chosen)** | Section 2 | The only option that gives new behaviour a home that is (1) outside the QQuickItem, (2) testable without a GPU or window, and (3) build-enforced. Each policy owns its state, so the "10 callbacks" problem shrinks to read-only ports | ~21 slices; ports with about 20 trivial methods; a temporary forwarder layer on UGR; FP/slot-order risk during moves (caught by the hex-float trace); the controller could become the next god class (mitigated by CCN ≤ 10 and 500 lines, and "sequencing only") |
| **C. ECS / data-oriented** | One `ChartState` blob plus systems (zoom, fit, live, frame) run in a fixed order each tick | Flexible on paper, but it is a rewrite, not a sequence of behaviour-neutral moves. The behaviour is event-driven (clicks, snapshots, ticks) and mid-command signal order matters (D23, `Zoom.cpp:190-193`), which a tick-ordered system loop breaks. The blob becomes the new god object. Unfamiliar to Codex writers | Large; not neutral-sliceable |
| D. Signal bus / mediator | Each feature is a QObject subscribing to view signals | Order depends on connection order (D23 is already a live hazard). Hidden coupling; worse to review than explicit sequencing | Medium; makes ordering bugs likely |

Why B: the regrowth comes from *where state lives* (C1) and *what the outside can reach* (C4). A fixes neither. C fixes C1 but costs a rewrite and breaks ordering semantics. B moves state and policy together in steps that are each a move, each verified by a byte-exact trace. B also costs more than A, and the controller needs active policing (section 5) to avoid repeating the problem one level down.

---

## 4. Migration plan

### Rules for every slice

- **Base and branch:** a fresh worktree from current `main` (`agent-worktree.sh create`). One slice at a time holds `UnifiedGridRenderer.cpp` (AGENTS.md §10), so slices land strictly in order, "run straight through".
- **Neutral means:**
  - the CPU view trace is byte-identical (hex floats);
  - the Metal frame hashes are identical;
  - existing tests are unchanged, except the mechanical renames listed in the slice;
  - moved blocks are verbatim (`git diff --color-moved=dimmed-zebra` shows them as moved), except the call-site renames the slice lists;
  - floating-point expressions keep operand order;
  - signal emission order is kept.
- **Size:** ≤ ~600 lines of new or modified code per slice. Verbatim moved blocks and whole-file deletions are listed but do not count.
- **Guard set (G):**
  ```
  ctest -R 'UgrGpu|ChartRaster|GridViewState|TimeAxisMapping|PriceAxisModel|TradeOverlay|TradeBubble|HeatmapSettingsUi|ChartUi|HeatmapLabelLayout|MainWindowSymbolLifecycle|AgentApiCodec|ChartViewTrace|ChartGolden'
  ```
  On native Metal the `UgrGpu`/`UgrGlide` cases must report run, not "GPU case skipped" (AGENTS.md §4).
- **Roles (AGENTS.md §10):**
  - Codex writers (`gpt-6-sol` for moves, `gpt-6-astra` for S8-S12).
  - Reviewer: Fable for Codex-written slices. GPU heatmap core is high risk.
  - The conductor (or a native Claude agent) runs the Metal golden and `UgrGpu` on the Mac before landing each slice. Sandboxed Codex cannot run Metal and reports that as unverified.
- **Every slice also runs** `scripts/dev/chart-health.sh` (from S0f on) and must not raise any ratchet.

### Phase 0: safety net and deletions

**S0a: CPU view-trace golden** (writer: Codex; risk: low)
- New `tests/render/test_chart_view_trace.cpp` (~350 lines) and `tests/render/golden/view_trace.txt`, plus a CMake target `ChartViewTraceTests` (label `render`).
- It reuses the `SyntheticHmc2Fixture` configured once in `main()`, as `test_ugr_gpu.cpp:90-99` does. The fixture has a pinned end, so there is no live clock.
- It drives a headless `UnifiedGridRenderer`. No window is needed; `UgrZoomCpu` already does this (`test_ugr_gpu.cpp:3202-3236`). Inject `setZoomClockForTest`.
- Scripted scenarios:
  1. wheel notches in/out at 3 cursor points, with clock steps of 0/65/130/300 ms;
  2. a trackpad gesture plus settle;
  3. time-axis and price-axis drags;
  4. a pan drag and release during a glide;
  5. follow-live steps (`followLiveForTest`);
  6. timeframe switch 1m→15m→1m, following and historical;
  7. symbol switch BTC→TINY with auto scale off (carry);
  8. `fitView` t/p/both, `resetView`, the `applyViewportRequest` variants;
  9. cold bootstrap.
- After each step, wait for `layer().settled()`, then print one line:
  - stored bounds, as `%a` hex floats;
  - `autoScroll`, `autoPriceScale`, `rasterAnchor`;
  - `viewportVersion`, `placementVersion`;
  - every field of `rasterCameraNow()` and `restCameraNow()`;
  - glide flags, `glideRebasesForTest()`, `autoPriceTickUnits`.
- `--bless` (a test argv flag, not an env var) rewrites the golden.
- Accept:
  - 20 consecutive runs are identical;
  - changing `kZoomStepRatio` to 1.26 locally makes it fail (report this, do not commit it).
- Guard: the new test itself.

**S0b: Metal frame-hash golden** (writer: native Claude; risk: low)
- New `tests/render/test_chart_golden.cpp` (~400 lines) and `tests/render/golden/frames.json`, target `ChartGoldenTests`.
- The scene is `lab::OffscreenQuick` 640×320 with UGR, a C++-built `CandlestickOverlayItem` and a `TradeBubbleOverlayItem` (mapping provider set as in `DepthChartView.qml:95-118`).
- Scenes:
  1. rest, auto scale + candles;
  2. Manual tick;
  3. mid-glide (zoom clock at 65 ms);
  4. an active drag of 0.3 px;
  5. a follow-live step;
  6. TPO split and TPO collapsed;
  7. VP;
  8. footprint (via `DataProcessor` columns if the fixture can feed them, else skipped with the reason, **UNVERIFIED**);
  9. S7b labels;
  10. trade bubbles (`addTrade`);
  11. after a 1m→15m switch;
  12. rest at `setDevicePixelRatio(2)`.
- Each scene is hashed only after the tile node has settled (`!crossfading && !holding`). Crossfades use `steady_clock` (`HeatmapTileNode.cpp:31, 1261`), so unsettled frames are not deterministic.
- Hash: SHA-256 of the RGBA readback plus an **explicit serialization** of the published mapping (each field written by name in a fixed order, doubles as hex-float). Never hash raw struct memory: padding bytes are indeterminate.
- The manifest is keyed by backend + `QRhi::driverInfo()`. **The Mac M4 + Metal key is required:** on that machine a missing baseline or a missing scene **fails** the test ("golden: required baseline missing for <key>/<scene>"); it never skips. Other machines skip loudly with the reason (owner Q1: no Windows/D3D12 validation yet). On a mismatch it dumps PNGs to `build/<preset>/golden-out/`. It supports `--bless`.
- Accept:
  - 10 identical runs on the Mac M4 (Metal);
  - a one-line local change to the candle body gap fails scene 1;
  - deleting one scene's entry from `frames.json` fails the run on the Mac.

**S0c: dead C++ surface** (Codex; low; audit N1 + N3 + owner decision 1)
- Delete:
  - `calculateOptimalResolution` / `GridResolution`, `gridResolutionChanged`, `priceResolutionChanged`;
  - `getCurrentTimeResolution`, `setGridResolution`, `getGridDebugInfo`, `getDetailedGridDebug`, `togglePerformanceOverlay`, `getCacheHitRate`;
  - `lastTimeAxisMapping`, `getScreenWidth/Height`, `currentTimeframe()`, `worldToScreen/screenToWorld`;
  - the `autoScrollSmoothEnabled` chain;
  - `m_manualTimeframeSet/Timer`;
  - the S8a leftover includes (audit-ugr §2a, 2b, 2f).
- `autoScrollPaddingFrac` becomes `static constexpr double kAutoScrollPaddingFrac = 0.08`. Its property and setter go.
- Delete `m_candleStreamGeneration` / `candleGeneration` and the unread footprint generation. Siblings: `FrameContext.hpp:27-30`, `FrameContextBuilder`, `ITimeAxisMappingProvider.hpp:187-188`, `CandlestickOverlayItem.cpp:373-388`. These compare a constant 0, so deleting them is neutral (**verify** in review: the overlay's change check must not now skip a rebuild).
- Owner-approved (2026-10-10): delete the `showRenderStrategyOverlay`, `showMemoryCacheOverlay` and `showModeFlagsOverlay` properties and their menu items (`HeatmapChartControls.cpp:249-252`, `MenuBuilder.cpp:154, 178`). This is a visible menu change, approved.
- Fix the stale docs: `UI_ARCHITECTURE.md:26, 41` and `ARCHITECTURE.md` "worldToScreen" mentions.
- Accept: trace + golden identical; one GUI-host smoke with `rg 'ReferenceError|TypeError|Cannot assign'` on the run log comes back empty.
- Guard: G + `ChartUiTests`.

**S0d: dead QML chain** (Codex; low; audit N2)
- Delete `qml/controls/{ChartToolRail,VolumeFilter,PriceResolutionSelector,GridResolutionSelector,NavigationControls,TimeframeSelector}.qml`, `qml/CandleChartView.qml` and `qml/Theme.qml`. Remove their CMake entries (`libs/gui/CMakeLists.txt:276-283`).
- Delete the C++ they keep alive: `minVolumeFilter`, `setPriceResolution`, `setGridResolutionPreset`, `getCurrentPriceResolution`. Optionally also the `DataProcessor` stubs (`DataProcessor.cpp:276-297`).
- Accept: the grep in audit-ugr §2d is empty; the QML smoke is clean.

**S0e: comment pass** (Codex; none)
- The 30 process comments (audit-ugr §4).
- The misplaced comment at `h:600-602` and the false comment at `cpp:163`.
- Trim `h:397-424` to a pointer to ARCHITECTURE.md.
- Accept: a comment-only diff, proven by `g++ -fpreprocessed -dD -E -P` token equality of each touched file before and after.

**S0f: ratchet gate** (Codex; low)
- `scripts/dev/chart-health.sh` (section 5.2) in ratchet mode, with a baseline file `scripts/dev/chart-health.baseline.json` (current line counts, per-function CCN).
- Wired into `agent-worktree.sh land` before the build step (`:134`).
- Accept: it passes on `main`; it fails on a scratch commit that adds one `if` to `gpuFitPriceWindow`.

### Phase 1: foundation

**S1: `sentinel_chart_core` target, ports, `PublishedFrame`** (Codex; low)
- CMake: `add_library(sentinel_chart_core STATIC …)` with `render/ChartRaster.*`, `render/GridViewState.*`, `render/TimeAxisMapping.hpp`, `render/TimeAuthority.*`, `render/FrameContext.hpp`, `render/FrameContextBuilder.*`, and new `chart/core/{ChartPorts.hpp, PublishedFrame.{hpp,cpp}}`.
- `target_link_libraries(sentinel_chart_core PUBLIC sentinel_core Qt6::Core)`. Remove those files from `GUI_SOURCES`; `sentinel_gui_lib` links the core `PUBLIC`.
- `FrameContextBuilder::build` takes `double dpr` instead of `QQuickWindow*`. One call site: `Render.cpp:209`, passing `window() ? window()->effectiveDevicePixelRatio() : 1.0`.
- UGR's `m_frameContextMutex`, `m_lastFrameContext` and `m_rasterStep` move into a `PublishedFrame m_published`. All 8 lock sites (D1) become `m_published.step()`, `.setStep()`, `.publish()`, `.current()`.
- Files: CMake, UGR h/cpp/Render/Zoom, `FrameContextBuilder`, 2 new files.
- Accept: G + trace + golden; `nm`/link shows `sentinel_chart_core` has no Qt6Gui or Qt6Quick dependency.
- Risk: low. A missed lock site is a race, not a pixel change: review all 8.

**S2: controller skeleton, session and flags relocation, view sink** (Codex; medium)
- New `chart/core/ChartViewController.{hpp,cpp}` and `chart/shell/HeatmapChartPort.{hpp,cpp}`.
- The controller owns `GridViewState` (moved out of `UGR::init`, `Init.cpp:30`). UGR's `getViewState()`/`viewState()` forward to it, so the 116 test calls are unchanged.
- Relocate data members, with mechanical renames only:
  - `m_currentTimeframe_ms`, `m_activeSymbol`, `m_gpuBookMid`, `m_gpuLastTrade` → `ChartSession`;
  - the five placement flags → `PlacementFlags`.
- Move `syncGpuView` and `applyGpuLimits` (`cpp:870-887`, through `IHeatmapViewSink` and `IChartEnvironment`), `setGpuViewportSelf` (`cpp:1093-1099`) and `gpuLivePrice` (`cpp:1133-1137`) into the controller. The UGR call sites call `m_view->…`.
- Accept: G + trace + golden; the UGR.h `m_` count drops by ≥ 10.
- Risk: medium (wide rename; slot order unaffected).

**S3: `ChartCamera`** (Codex astra; medium)
- Move `rasterInputs` (`Render.cpp:95-112`), `camerasFor` and `glidePart` (`Zoom.cpp:36-87`), `rasterCameraNow`/`restCameraNow`/`intendedEndNow`/`tickPredictor` (`Zoom.cpp:89-150`), `rasterPanShift` (`cpp:889-914`) and `drawnTimeWindow` (`cpp:1179-1202`) into `chart/core/ChartCamera.{hpp,cpp}`.
- `Cameras` and `SyncFrame` share one `BinView` (D22).
- Also move the glide/gesture *data* (`AxisGlide`, `m_glideTime/Price`, `m_zoomGesture`, `m_gestureTime/Price`, `m_glideColPx`, `m_glideRow`, `m_glideRebases`) into a `ZoomGlideState` struct owned by the controller. Mechanical renames only; the logic moves in S4.
- The `PanShift` hook (`Init.cpp:115-117`) points at `camera.panShift`.
- UGR's `rasterCameraNow`/`restCameraNow` forward (AxisModel and tests use them).
- Accept: G + trace + golden.
- Risk: medium. The DPR (D3) and step snapshot (D1) dedupe must keep the exact expression `window() ? window()->effectiveDevicePixelRatio() : 1.0`, now in `HeatmapChartPort::surface()`.

### Phase 2: policies, moved bottom-up so nothing calls back into UGR

**S4: `ZoomGlide` engine** (Codex astra; medium)
- Move `endZoomGlide`, `startAxisGlides`, `advanceAxisGlide`, `advanceZoomGlide` (`Zoom.cpp:152-271`, minus `updatePolish`), the pan-release shift (`cpp:1807-1819`) and the follow-live glide shift (`cpp:948-953`) into `ZoomGlide`.
- UGR `updatePolish` keeps the polish-timer plumbing (`Zoom.cpp:196-212`) and calls `controller.advanceFrame(now)`.
- `ZoomGlide`'s signals (`rasterChanged`, `viewportChanged`, `polishRequested`, `updateRequested`) are direct-connected to UGR in the same order as `Zoom.cpp:190-193, 266-270`.
- Accept: G + trace + golden.
- Risk: medium (emission order).

**S5: `PriceFitter`** (Codex astra; medium-high)
- Move `gpuFitTimeWindow` (`cpp:1139-1177`), `gpuFitPriceWindow` (`cpp:1204-1317`), `autoPriceFit` and `refitAutoPrice` (`cpp:1319-1341`) and the candle-dirty hook body (`cpp:1110-1127`, through `ICandleSource`) into `chart/core/PriceFitter.{hpp,cpp}`.
- Move the frozen-A2 manual row rule (`cpp:1306-1314`) verbatim into `chart_raster::fitPriceManualRows(lo, hi, H, tick)` (audit N8).
- Split `gpuFitPriceWindow` into named steps (`collectVisibleBars`, `centreAndSpan`, `applySpanLimits`, `snapToRows`). Do not reorder any expression. Goal CCN ≤ 15 each.
- The `PriceFit` hook (`Init.cpp:110-112`) points at the fitter.
- Accept:
  - G + trace + golden;
  - lizard: no fitter function above CCN 15;
  - `test_ChartRaster` unchanged and passing.
- Risk: medium-high (largest function; FP order). Review it statement by statement against `180d3b7`.

**S6: `LivePlacement`** (Codex; medium)
- Move `followGpuLive` / `followGpuLiveTo` (`cpp:918-960`), `seedGpuViewport` (`cpp:966-998`), `gpuLiveEndMs`, `returnGpuToLive` and `bootstrapGpuTimeView` with its timer, and `gpuInitialSpanMs` (`cpp:1035-1091`).
- Move `FollowPadding` (`h:206-210`), the price-seed parts of `onTradeReceived` and `setLiveBookTop` (`cpp:163-177`), the pristine-resize branch of `geometryChange` (`cpp:238-244`) and `enableAutoScroll` (`cpp:808-815`).
- **Keep both live-end formulas** as two named helpers: `liveEndCeil(x, tf)` = `floorDiv(x+tf-1, tf)*tf`, and `nextBucketEnd(x, tf)` = `floorDiv(x, tf)*tf + tf`. Each gets a `// FLAG: differs at bucket boundary` comment (audit D4; fixed in B1).
- Accept: G + trace + golden.
- Risk: medium.

**S7: `TimeframeSwitch` planner + `PriceCarry`** (Codex astra; medium-high)
- `setTimeframe` (`cpp:517-646`, CCN 41) becomes:
  - the controller's `setTimeframe`: sequencing (end glides, limits deferral, apply plan, `syncGpuView`, emits, the queued `DataProcessor` call stays in the shell via a `timeframeChanged` hook);
  - `planTimeframeSwitch` (pure, section 2.4).
- Move the carry: the `setActiveSymbol` carry part (`cpp:350-372`), `applyPriceCarry` + timer (`cpp:1000-1033`) and the limits/priceInteracted reactions (`Init.cpp:58-66, 118-120`) → `PriceCarryPolicy`.
- `setActiveSymbol` keeps its order exactly: reseed flag, then `setSymbol`, `clearData`, refit (`cpp:376-383`).
- Accept:
  - G + trace (scenarios 6 and 7 must be byte-identical) + golden;
  - lizard: the planner ≤ 15 per function.
- Risk: medium-high.

**S8: `ZoomController` + `WheelInterpreter`** (Codex astra; medium)
- Move `setZoomRung`, `zoomStoredView`, `zoomClicks`, `zoomContinuous(For)` with the settle timer, `endZoomGesture`, `wheelZoom` with the carry, and the axis wrappers (`Zoom.cpp:273-571`).
- Pure notch classification (`Zoom.cpp:519-546`) → `WheelInterpreter::classify(angle, pixelDeltaNull, phase, precise)`.
- Dedupe D7 (`zoomFrac`), D8 (`columnLimits`), D9 (`clampSpan`), D10 (post-zoom tail), keeping each expression.
- UGR keeps `event()` (QNativeGestureEvent is QtGui) until S14.
- Accept: G + trace + golden; no function above CCN 25 (it was 40, `zoomClicks`); the split target is 15.
- Risk: medium.

**S9: explicit view commands + heatmap reactions** (Codex; medium)
- Move `setViewport` (`cpp:1739-1768`), `applyViewportRequest` (`cpp:1395-1453`), `fitView` (`cpp:1354-1393`), `resetView` (`cpp:1455-1487`), `setAutoPriceScale` (`cpp:1343-1352`) and `pan*`/`beginPanAt`/`updatePanAt`/`endPanAt` (`cpp:1782-1844`).
- Move the `HeatmapGpuLayer` signal reactions (`Init.cpp:35-57`) and the constructor connections (`cpp:73-81`) into `onHeatmapSnapshot`/`onHeatmapLive`/`onHeatmapTick`/`onHeatmapLimits`. **Preserve D23's order:** init's slots first, then the constructor's. Connect in that order in one place, with a comment saying why.
- Dedupe D12 (land zoom), D13 (price known), D19 (drop the allocating compare at `cpp:168`; neutral).
- Accept:
  - G + trace + golden;
  - controller functions ≤ CCN 10, apart from `applyViewportRequest`, which is split into resolve flags / resolve window / commit.
- Risk: medium (connection order).

After S9, UGR has no view logic left. Expected sizes: `.cpp` ~800, `Zoom.cpp` deleted or ~60 lines.

### Phase 3: frame and layers

**S10: `ChartFrame`, `FrameProducer`, `FrameAck`** (Codex astra; medium-high)
- Move `prepareSyncFrame` and `computeGpuFrameMapping` (`Render.cpp:114-159, 203-234`) and `publishFrameContext` (`Render.cpp:16-41`) into `chart/core/FrameProducer`.
- `SyncFrame` (`h:266-274`) folds into `ChartFrame`.
- `HeatmapChartPort` implements `ITickAuthority` (`chooseTickForView`, `tickPrice`, `noteRaster`).
- The raster-surface announce (`h:175-180`, `Render.cpp:144-158`) moves with it.
- Control revisions and frame ids (`h:301-308`, `Render.cpp:254-255, 302-305`, `cpp:110-111`) → `FrameAck`.
- Accept:
  - G + golden (frames) + trace;
  - the frame profiler marks are unchanged (`SENTINEL_FRAME_PROFILE=1` log lines keep their stage names).
- Accept (owner correction): `produce` takes no `ChartCamera` and no reference to GUI-owned state; a test mutates `GridViewState` after `frameInputs()` is captured and the produced `ChartFrame` does not change.
- Risk: medium-high (render thread). No allocation in `produce`: review it against `Render.cpp:116, 202`.

**S11: `IChartLayer`, `LayerRegistry`, `ChartLayerSet`; trade-overlay layers; `TradeOverlayFeeds`** (Codex astra; medium)
- `IOverlayRenderer` is replaced by `IChartLayer`.
- Thin adapters `FootprintLayer`, `VolumeProfileLayer`, `TpoLayer` wrap the existing renderers. Their `render(...)` signatures are unchanged; the adapter maps `ChartFrame` to the arguments (`Render.cpp:44-77`).
- **Registry order: footprint, VP, TPO** (today's call order, not today's `zOrder()` values).
- `DataProcessor` + its thread + `connectDataProcessorSignals` (`Init.cpp:81-95, 122-125, 138-192`) and the dtor teardown (`cpp:132-154`) → `TradeOverlayFeeds`. `UGR::getDataProcessor()` forwards (MainWindowGpu uses it).
- `clearData` (`cpp:327-341`) sequences the feeds.
- Accept: G + golden; QSG node order is identical (a debug dump of the root's child types before and after, in one scene, attached to the hand-off).
- **Accept (owner correction, was B8): late-enable node-order test.** Build a scene with chart text already present, then enable VP, then footprint; assert the root's child order equals registry order (text last). If it fails, S11 adds one container node per layer, created in registry order at registration, so lazily created nodes append inside their own container. Registry order guaranteeing QSG order is the registry's central promise; it cannot wait for phase 5.
- Risk: medium (node order, teardown order).

**S12: `HeatmapLayer`, `ChartTextLayer`, `TradeBubbleFeed`** (Codex astra; medium)
- `ensureGpuRootNode`, the gate, `prepareFrame` + tile `setFrame` (`Render.cpp:80-93, 237-253`), `updateGpuLabels` with its state (`Render.cpp:161-196`, `h:279-284`) and the `afterRendering` checks (`cpp:112-121`) → `HeatmapLayer`.
- `buildMsdfAtlas` (`cpp:1501-1552`), `getMsdfAtlasMemory`, `ChartTextRenderer` begin/end and `AxisTextService::submitAxisText` (`Render.cpp:266-273`) → `ChartTextLayer`. It stays registered last.
- The bubble tape, settings and `publishTradeBubbleFrame` (`Render.cpp:282-288`, `cpp:157-162`, the `setHeatmapChartSettings` bubble part `cpp:833-844`) → `TradeBubbleFeed`.
- Mechanical test update: `TradeBubbleRendererTest` (`test_heatmap_settings_ui.cpp:63-69`) reaches the feed instead of UGR privates. Keep it a friend of the feed.
- Accept: G + golden.
- Risk: medium (labels' cross-frame signature logic).

### Phase 4: shell and facade

**S13: `LayerVisibility` + `ChartLookSettings` + config bridges** (Codex; low)
- `setPrimaryField` and `set*LayerEnabled` (`cpp:471-494, 702-806`, verbatim; the D15 log helper only).
- TPO config and style (`cpp:397-469`); tone (`cpp:648-680`, `syncGpuTone`); candle look (`cpp:681-700`).
- `applyClientConfig`/`applyServerConfig` sequencing (`cpp:1554-1576`), with D24's redundant tone call **kept** (dropping it is a phase 5 item).
- UGR keeps the `Q_PROPERTY` forwarders.
- Accept: G + golden; `ChartUiTests` and `HeatmapSettingsUiTests` unchanged.
- Risk: low.

**S14: `ChartInputRouter`, header regroup, decoupling** (Codex; low-medium)
- Mouse, wheel and native-gesture events (`cpp:1591-1635`, `Zoom.cpp:575-599`) → `ChartInputRouter`. It keeps the `mousePressEvent`-without-`update()` asymmetry (D18).
- Header order (audit N7): properties; public API by job; a fenced `// Test hooks (forwarders only)` block; private members (≤ 15).
- `AxisModel` and `TradeBubbleOverlayItem` stop `qobject_cast`-ing to UGR. `AxisModel` takes an `IChartCameraSource` (`rasterCameraNow`, `viewState`, size; the signals stay on a `QObject`, see the **UNVERIFIED** note below). The bubble item takes `ITradeBubbleFrameSource`.
- Final caps switch on in `chart-health.sh`: UGR `.cpp` ≤ 450, `.h` ≤ 350.
- Accept: G + golden + trace; `rg -l 'UnifiedGridRenderer.h' libs/gui/render libs/gui/models libs/gui/chart` is empty, apart from the shell allowlist.
- Risk: low-medium.
- **UNVERIFIED:** whether `AxisModel` can connect to the interface's signals without a `QObject` base. The fallback is a small `ChartCameraSource : QObject` owned by the shell.

**S15 (recommended): grouped QML objects + `ChartAgentPort`** (Codex astra; medium; MainWindowGpu hot file)
- Add the `view`, `candles`, `tpo`, `debug` and `layers` sub-objects (section 2.7). Update `DepthChartView.qml`, `HeatmapChartControls`, `MenuBuilder`, MainWindowGpu call sites and the tests (mechanical renames).
- Delete the flat forwarders (no-backcompat rule, AGENTS.md §1).
- Move `agentApiViewportSnapshot` and the viewport-request and revision code to `mainwindow/ChartAgentPort.cpp`.
- Accept:
  - G + `AgentApiCodecTests` + `AgentApiDocksTests` + golden;
  - QML smoke clean;
  - `GET /viewport` JSON byte-equal for a fixed scene before and after.
- Risk: medium (QML paths). Needs owner yes (Q2).

### Phase 5: behaviour fixes (each its own reviewed slice; not neutral; golden re-bless named per slice)

| # | Fix | Where after the migration | Test that must fail without it | Needs |
|---|---|---|---|---|
| B1 | One live-end formula (audit D4): seed and bootstrap use `liveEndCeil` | `LivePlacement` | Policy test: `x` exactly on a bucket boundary; seed end == follow end | owner default (Q3) |
| B2 | **Blocked (owner, 2026-10-11).** One candle-interval rule (audit D6), but no rounding change until the candle-interval spec in section 6 Q3 is decided. Today three sites disagree: `PriceFitter` `(tf+500)/1000`, `requestCandleHistoryForSymbol` (`MainWindowGpu.cpp:1213`) `(tf+999)/1000`, QML (`DepthChartView.qml:103`) `Math.round`. | core + MW + QML | tf = 100 ms and 1,500 ms behave as the spec says at all three sites | candle-interval spec |
| B3 | Reset `autoPriceTick_` in `HeatmapGpuLayer::destroyController` and `forgetService` (`HeatmapGpuLayer.cpp:70-81, 119-136`; `setSymbol`/`setTimeframeMs` already reset it at `:140`, `:153`) | heatmap layer | service swap with a pending decision: the next frame judges fresh | none |
| B4 | Rename `UgrZoomCpu.SymbolSwitchBeforeFirstSnapshotUsesPlainFitSilently` → `…PublishesRawBoundsSilently` (`test_ugr_gpu.cpp:3530`) | tests | n/a | conductor confirm |
| B5 | Double fit per live tick (audit-ugr §5): skip the `liveChanged` refit when `followTo` already fitted | controller | count `solveAutoPriceFit` calls per live step == 1 | perf item |
| B6 | Remove the ~78 dead `m_viewState` null checks and the 13 `m_axisTextService` checks (audit §2g); they become non-optional references in the controller | everywhere | build + G | none (neutral, noisy: done last) |
| B7 | `SENTINEL_DUMP_GLYPH_ATLAS` `/tmp` dump (`cpp:1534-1548`) violates AGENTS.md §4a | `ChartTextLayer` | n/a | owner decision |
| B8 | Moved into S11's acceptance gate (owner correction 2026-10-11). Late-created VP/footprint nodes `appendChildNode` lazily (`VolumeProfileRenderer.cpp:197-226`, `FootprintOverlayRenderer.cpp:103`). | — | — | — |

The audit's other A3 minor (refit cost) is B5. D21 (the parallel drag mapping for Auto's tick choice, `Render.cpp:223-226`) is intentional. Leave it alone unless the "heatmap/candle drift" item is taken up.

### Sequencing summary

```
S0a ─ S0b ─ S0c ─ S0d ─ S0e ─ S0f ─ S1 ─ S2 ─ S3 ─ S4 ─ S5 ─ S6 ─ S7 ─ S8 ─ S9 ─ S10 ─ S11 ─ S12 ─ S13 ─ S14 ─ S15 ─ B1, B3…B7 (B2 after its spec)
                  (all hold UnifiedGridRenderer.cpp; strictly serial)
S0a/S0b can run alongside other work (tests only); S0b needs the Mac.
```

---

## 5. Guardrails for AGENTS.md

### 5.1 Proposed text (new section "3a) Chart core: where chart behaviour goes")

> **Chart core rules (owner, 2026-10-xx).** `UnifiedGridRenderer` is a Qt Quick shell. It changes only for item lifecycle, window/sync wiring and new `QQuickItem` overrides.
> - View behaviour (camera, zoom, glide, fit, follow-live, placement, timeframe/symbol switch) goes in a policy in `libs/gui/chart/core/`. If none fits, add a new policy class. The `ChartViewController` only sequences policies (no arithmetic or branching on prices, times or pixels).
> - `sentinel_chart_core` is QtCore only. No `QtGui`, `QtQuick`, `QSG` or `QQuickWindow` (the build enforces this). It never includes `HeatmapGpuLayer.hpp`, the source controller or recording types; it uses the ports in `ChartPorts.hpp`.
> - A new chart overlay is an `IChartLayer` in `libs/gui/chart/layers/`, plus one line in `ChartLayerSet.cpp`. Data comes through a feed and a `LayerMailbox`. Render-thread code reads only `ChartFrame`, its own state and its mailbox.
> - New QML knobs go on the owning module's sub-object (`view`, `candles`, `tpo`, `layers`, `debug`). New Agent API fields go in `ChartAgentPort`. New test hooks go on the policy under test, not on UGR.
> - New view behaviour comes with a Qt-free unit test of its policy. A GPU test alone is not enough.
> - `tests/render/golden/*` may only be re-blessed in a slice that declares a visible change. The hand-off names every changed scene and why.

### 5.2 Budgets and how they are checked (`scripts/dev/chart-health.sh`, run by `agent-worktree.sh land` and by writers)

| Check | Rule | Command |
|---|---|---|
| File caps | `UnifiedGridRenderer.cpp` ≤ 450, `.h` ≤ 350 (from S14; ratchet before: never grows). Every `libs/gui/chart/**` file ≤ 500 (warn at 400) | `wc -l` against the baseline JSON |
| UGR members | `m_` fields in `UnifiedGridRenderer.h` ≤ 15 from S14; never grows | `rg -c '^\s+[^/(]*\bm_[a-zA-Z]' libs/gui/UnifiedGridRenderer.h` |
| Complexity, new functions | CCN ≤ 15, ≤ 60 NLOC, ≤ 6 parameters; `ChartViewController` CCN ≤ 10 | `lizard -l cpp -C 15 -L 60 -a 6 -w <changed files>` (exit 1 on a breach; checked with lizard 1.24.1) |
| Complexity ratchet, existing functions | a touched function's CCN may not rise above max(15, its baseline) | the script diffs `lizard --csv` at merge-base vs HEAD for changed files, by function name |
| Duplication | no new clone ≥ 8 lines / 60 tokens involving a changed file in `libs/gui/chart/**` or `UnifiedGridRenderer*` (baseline: 0 in `chart/`) | `npx jscpd@<pinned> --min-lines 8 --min-tokens 60 --format cpp --reporters json -o build/jscpd libs/gui/chart libs/gui/UnifiedGridRenderer*.cpp` (pin the exact version in the script; the tool scan did not record one) |
| Boundary | no Quick/SG/Gui includes in core; no `UnifiedGridRenderer.h` outside the shell allowlist | `rg -n '#include\s*[<"](QQuick\|QSG\|QtGui\|QtQuick\|QQml)' libs/gui/chart/core` must be empty; `rg -l 'UnifiedGridRenderer\.h' libs/gui --glob '!libs/gui/UnifiedGridRenderer*' --glob '!libs/gui/MainWindowGpu*' --glob '!libs/gui/mainwindow/**'` must be empty |
| Forwarders | `UnifiedGridRenderer.Qml.cpp` function bodies are a single statement | lizard NLOC ≤ 3 for that file |

### 5.3 Review checklist (add to the reviewer packet for any `libs/gui/UnifiedGridRenderer*` or `libs/gui/chart/**` diff)

1. Does the diff add a member, method, property or signal to UGR? Is it lifecycle or wiring? If not, where should it live?
2. Does new state live in the policy that changes it, with value or port inputs (no pointer back to UGR or the controller)?
3. Do render-thread paths read only `ChartFrame`, their own state or a mailbox? Do they allocate nothing per frame (AGENTS.md §5)?
4. Is signal emission order unchanged in a neutral slice (compare with `180d3b7` or the previous tip)?
5. Did a fix round add a branch to a function already at CCN ≥ 15? If so, it must extract first.
6. Is there a Qt-free policy test for the new behaviour, and does it fail without the change (AGENTS.md §10 acceptance rule)?
7. Are trace and golden unchanged? If re-blessed, is the visible change declared and owner-approved?
8. Are comments current facts, with no slice, review-round or plan-path references (audit-ugr §4)?

### 5.4 A reviewer must reject

- New `m_` fields or `Q_PROPERTY`s on `UnifiedGridRenderer` (after S14), unless they own a new top-level component the conductor agreed to.
- `#include "UnifiedGridRenderer.h"` or a `qobject_cast<UnifiedGridRenderer*>` in `chart/`, `render/` or `models/`.
- A re-blessed golden or trace in a slice labelled neutral.
- A hand-copied camera setup (`viewportSnapshot` → `rasterInputs` → `computeRaster`) instead of `ChartCamera`.
- A layer reading `GridViewState`, a `QObject` or UGR from the render thread.
- A new parameter added to `IChartLayer` for one layer's needs (put it in `ChartFrame` or the layer's mailbox instead).
- A lizard ratchet breach, a new jscpd clone, or a core include of QtGui/QtQuick, even with "will fix later".
- Process comments in production code.

---

## 6. Owner decisions (2026-10-11) and the original questions

**Decisions.**
- **Q1 approved.** Mac M4 + Metal goldens only, settled-frame hashes committed. CPU regression tests stay portable. No Windows/D3D12 validation yet. A missing or mismatched Mac golden fails the test (see S0b).
- **Q2 approved.** S15 goes ahead: chart features get their own interfaces instead of growing UGR; obsolete flat APIs are removed in the same push.
- **Q3 partial.** B1 approved (one live-end formula, the ceil form). **B2 blocked:** integer-second rounding cannot represent a 100 ms or 1,500 ms candle, and the earlier claim that "only sub-second timeframes change" was wrong (ceil and round also differ at e.g. 1,499 ms: 2 s vs 1 s). The chart offers 25, 50, 100, 250 and 500 ms timeframes today (`TimeAxisModel.cpp:9`), so this is live. **Candle-interval spec to decide before B2:** (i) the candle interval is `tfMs` (integer milliseconds) end to end, in the core, MainWindowGpu and QML; nothing rounds it to seconds; (ii) seconds exist only at a boundary that truly speaks seconds (display labels, and any history API whose granularity is in seconds); (iii) at such a boundary, a `tfMs` that is not a whole supported number of seconds is refused explicitly (no history request, a logged reason, live-built candles only), never rounded. Recommended; **inferred, not checked:** what granularities the candle-history path accepts. The B2 writer verifies that first and brings the spec back for owner sign-off.
- **Q4 approved, tightened.** Feature freeze on chart-view features for the whole migration, through S15. Necessary bug fixes land in the slice that owns the code. Independent heatmap/backend work continues. S9 is a checkpoint, not permission to resume features. **The freeze ends when S15 has landed and the Mac goldens and CPU traces are green on main.**
- **Q5 approved, usage-aware.** Codex Sol: repetitive migrations, renames, tests, straightforward slices (S0c-S2, S13-S14). Codex Astra: hard extraction and frame/thread work (S3-S12, S15). Fable: independent reviews of high-risk slices only (S3, S10, S11, S12, S15). Opus: coordination, architecture calls, exceptions. A native Claude session on the Mac runs `ChartGoldenTests` + `UgrGpuTests` before every land. No premium cross-provider review of trivial extractions. Spend Codex aggressively on approved work; when it runs low, assess reset credits before the ordinary reset, and do not assume credits stack or roll over.

The original questions and defaults follow for the record.

### Original questions (superseded by the decisions above)

1. **May golden baselines be committed per GPU (Metal M4 only)?**
   - Risk: GPU hashes are device- and driver-specific, and crossfades and upload budgets are time-based. A flaky golden would block every slice.
   - Default: commit Metal-M4 hashes of **settled frames only**, keyed by `QRhi::driverInfo()`, skipping (with the reason) on other machines. The CPU hex-float view trace is the primary neutrality proof and runs on every machine and in Codex sandboxes.
2. **Do the grouped QML objects (S15) go ahead?**
   - Risk: they change QML paths and touch MainWindowGpu (hot), HeatmapChartControls, MenuBuilder and tests.
   - Without S15, the facade keeps growing one forwarder per knob (cheap, but it is how C4 returns).
   - Default: **yes**, after S14, in one slice, deleting the flat names in the same push (AGENTS.md §1).
3. **Which behaviour wins in the two latent mismatches?**
   - Default B1: the ceil form `floorDiv(x+tf-1, tf)*tf` wins. Follow-live, return-to-live and the timeframe switch use it in 6 places; seed and bootstrap adopt it. Effect: on an exact bucket boundary the first view sits one timeframe earlier than today.
   - Default B2: `(tf+500)/1000` (UGR's rule) for MainWindowGpu and QML too. Only sub-second timeframes change.
4. **Freezing UGR for ~21 landings.**
   - Risk: chart-view feature requests during the migration either wait or land in the old structure and get moved twice.
   - Default: from S2 to S9, no new view behaviour lands; bug fixes land in the slice that currently owns the code. After S9, new behaviour goes straight into the policies. Heatmap-stack work (source controller, binner, tile node) continues in parallel because it never touches UGR.
5. **Routing GPU-sensitive slices.**
   - Risk: Codex writes most slices but cannot run Metal (AGENTS.md §10), so the Metal golden and the `UgrGpu` cases need a native run every landing.
   - Default: Codex writes (Sol for S0c-S2 and S13-S14, Astra for S3-S12 and S15), Fable reviews, and the conductor (Claude, native) runs `ChartGoldenTests` + `UgrGpuTests` on the Mac before each land and attaches the summary as `Evidence:`. If Codex is out of usage, Opus writes and Astra or Fable reviews (the §10 fallback).

---

### Appendix: unverified items (writers check before relying on them)

- Whether the synthetic fixture can feed footprint columns for golden scene 8 (S0b).
- Whether late-created VP or footprint nodes after the text root are ever visible (B8).
- Whether `AxisModel` can follow an interface without a `QObject` base (S14).
- Whether deleting the always-0 `candleGeneration` comparison in `CandlestickOverlayItem.cpp:373-388` changes any rebuild decision (S0c review; reading says no).
- The `jscpd` version used in the 2026-10-11 tool scan (not recorded).

- **2026-10-13 owner decision (B2):** option A. The candle interval is `tfMs` end to end; sub-second timeframes (25-500 ms) get real candles built from trades, live and from the journal; a boundary that speaks seconds refuses unrepresentable intervals with a logged reason, never rounds; ranges without journal coverage show no candles.
