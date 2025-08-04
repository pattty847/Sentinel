Sentinel GPU Chart – “Micro View Clean-Up & Dynamic LOD”  
Implementation Plan (v0.1 / hand-off draft)
================================================

Objective   
Tighten the ultra-short-term view so that:

1.  Trade scatter remains buttery-smooth but no longer floods the GPU with
    dozens of dots for the same 16 ms batch.
2.  Candle layer is automatically hidden for views < 10 seconds
    and shown again (≥ 1 s candles) as you zoom out.
3.  Foundations laid for future “tick / volume” candles and
    drill-down navigation.

------------------------------------------------
A.  Reduce trade-dot noise *(GPUChartWidget)*
------------------------------------------------
File(s)  
• `libs/gui/gpuchartwidget.cpp`  
• `libs/gui/gpuchartwidget.h`

Steps  
1. Add member `m_pointsPerFrameLimit` (default = 30).  
2. Inside `GPUChartWidget::onTradesReady` – when the incoming batch is
   larger than the limit, subsample (e.g. keep every ⌈N/limit⌉-th point).  
   This cuts GPU uploads from 100+ → ≤30 without harming visual flow.
3. Provide `setPointsPerFrameLimit(int)` for tweaking via QML/INI later.

Risk  
None to coordinate math; only VBO fill size changes.

------------------------------------------------
B.  Hide candle layer when zoomed-in tighter than 10 s
------------------------------------------------
File(s)  
• `libs/gui/candlestickbatched.cpp`  
• `libs/gui/candlestickbatched.h`

Steps  
1. Extend `selectOptimalTimeFrame()`:

```cpp
int64_t viewSpan = m_viewEndTime_ms - m_viewStartTime_ms;
if (viewSpan < 10'000)     // < 10 seconds
    return TF_None;        // special sentinel
```

2. Add enum value `TF_None` to `CandleLOD::TimeFrame`.  
3. In `updatePaintNode()` early-return `nullptr` when active TF is `TF_None`.

Result  
No candle geometry built in deep-zoom; only dots + OB shown.

------------------------------------------------
C.  Automatic switch-back to 1 s candles
------------------------------------------------
Already covered by existing LOD table; once viewSpan ≥ 10 s
the `selectOptimalTimeFrame()` will pick TF_1sec.  
No extra code required after Step B.

------------------------------------------------
D.  Optional-Future: Tick-based candles (scaffolding)
------------------------------------------------
File(s)  
• `libs/gui/candlelod.h/.cpp` – add TF_TICK100 (or TF_TICKN)  
• `libs/gui/gpudataadapter.cpp` – maintain per-symbol tick counter,
  emit candle when count == N.

(Not implemented in this pass, but note where work will occur.)

------------------------------------------------
E.  Drill-down navigation concept
------------------------------------------------
Later we’ll add a `ChartModeController` action:

1. User clicks a daily candle → `ChartModeController::zoomIntoCandle()`  
2. Emits `requestZoom(timeStart, timeEnd)` to GPUChartWidget.  
3. GPUChartWidget resets its window; CandlestickBatched auto-picks
   a lower timeframe (e.g. 1 h → 5 min).

No immediate code – but all current changes are compatible.

------------------------------------------------
Order of execution
------------------------------------------------
1. Implement Section B (invisible candles) first – zero functional overlap.  
2. Add Section A (dot subsampling).  
3. Test performance & visuals.  
4. If desired, continue with Section D in a future sprint.

------------------------------------------------
Testing checklist
------------------------------------------------
• Deep-zoom (< 10 s) – confirm no candles are painted; FPS unchanged.  
• Zoom out (~30 s) – 1 s candles appear, trade dots still smooth.  
• Stress: 10 k trades / s firehose – GPU memory steady, ≤1 frame lost.  
• Subsample toggle: set limit = 1 to restore full density for regression.

------------------------------------------------
Files to touch summary
------------------------------------------------
1. `libs/gui/gpuchartwidget.{h,cpp}`  
2. `libs/gui/candlelod.{h,cpp}`  
3. `libs/gui/candlestickbatched.{h,cpp}`  
(plus unit-test updates if applicable in `tests/`)

------------------------------------------------
Notes & risks
------------------------------------------------
• Adding `TF_None` is ABI-safe; enums are only used internally.  
• Subsampling relies on current triple-buffer logic – no changes there.  
• Coordinate system remains single-source-of-truth (`GPUChartWidget`).

Ready for implementation in a fresh session with these file paths & steps.