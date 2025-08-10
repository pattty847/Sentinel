## 🧠 Gemini CLI Prompt: Timestamp Quantization + Unified Frame Rendering

Helpful documents: 

ARCHITECTURE: docs/SYSTEM_ARCHITECTURE.md
GPU Rendering: docs/features/gpu_chart_rendering/00_PLAN.md
GPU Rendering Progress: docs/features/gpu_chart_rendering/01_PROGRESS.md
Fractal Zoom: docs/features/fractal_zoom/2025-06-30_fractal_zoom_outline.md
Hand-off Documents Between Feature Implementations and Agent Context Length Limit Reached: docs/features/gpu_chart_rendering/handoffs
GPU Coordinate Bug: docs/features/gpu_chart_rendering/investigations/2025-06-26_gpu_coordinate_bug_report.md

etc, the whole /docs folder is useful but don't get stuck in analyzing it too much. We are working on a high-performance GPU C++ system using Qt + VBOs. Just give me the best path forward and specific refactors to unify these systems. We want to get back to our wall of liquidity which went away when fractal zoom was implemented or started to be implemented, however we're almost there we need to stich everything together and maybe sniff our some bad logic. Make all plots work together flawlessly. As well as create a generate test data path for viewing the fractal zoom for the order book as we can't wait for the real data to come in, well be waiting forever. 

---

> ### 🧩 PROJECT CONTEXT: Fractal Zoom Financial Chart Engine
>
> I’ve built a GPU-accelerated, real-time order book + trade visualizer using C++ and Qt. The architecture resembles Bookmap/TradingLite and includes:
>
> * A `FractalZoomController` that maps zoom levels to LOD rules (e.g., 100ms, 500ms, 1s frames)
> * A `GPUChartWidget` that handles the main canvas and coordinates world→screen projections
> * Separate `worldToScreen()` functions across `HeatmapBatched`, `CandlestickBatched`, and `GPUChartWidget`
> * A `TemporalSettings` matrix that defines bucket width for order book & trade points
>
> ### ⚠️ CORE ISSUE:
>
> The system paints trades and order book data at high frequency, but **desyncs emerge due to direct timestamp-to-pixel mapping**:
>
> * We map `timestamp_ms → X coordinate` with a floating-point ratio of `(ts - viewStart) / viewRange`
> * This causes **gaps between data frames** (e.g., painted at X=100.2px, 200.4px, etc), leading to **visible stuttering and non-deterministic layouts**
> * Multiple modules use slightly different `worldToScreen()` logic (copied variants) leading to inconsistent visual projection across layers
>
> **What we want instead:**
>
> > A consistent, discrete rendering model where time moves in fixed “buckets” and every frame’s data is centered within that bucket, regardless of when the events arrive.
>
> ### 📐 Desired Behavior:
>
> * Create a unified system where each data point is assigned a **quantized timeframe bucket**:
>
>   ```
>   bucket_start_time = floor(timestamp_ms / bucket_size) * bucket_size;
>   ```
> * Trades should **snap to the center** of their frame (e.g., if 1000–1100ms bucket, render at 1050ms)
> * Renderer should treat X axis as **discrete frames** (e.g., X=frame\_idx \* frame\_width), not floating-point timestamps
> * Replace raw timestamp-based `worldToScreen()` with **frame-aligned projection**
> * Output should be like:
>
>   ```
>   | OB FRAME | OB FRAME | OB FRAME |
>   |  Trades  |  Trades  |  Trades  |
>   ```
> * All series (candles, trades, order book) need to reference the same coordinate system
>
> ### 📦 Documents to Use:
>
> * FractalZoomController (zoom logic + interval definitions)
> * HeatmapBatched::worldToScreen
> * CandlestickBatched::worldToScreen
> * GPUChartWidget::worldToScreen/screenToWorld
> * TemporalSettings\[] matrix
>
> ### 🔧 Your Task:
>
> 1. Refactor or replace `worldToScreen()` logic to align all layers to **discrete frame intervals**
> 2. Implement a consistent **time→frameIndex** function across modules
> 3. Build a `RenderFrame` system that aggregates both trades and order book data by timeframe bucket, and feeds that to the renderer
> 4. Ensure trades and book data are always rendered in sync, with consistent gaps between frames and aligned X-axis
> 5. Optimize for high-frequency drawing (16ms–1000ms cadence), and ensure that quantization does not introduce lag or missed paints
>
> ### Bonus:
>
> * Suggest a better name and abstraction for the new frame-locked projection method
> * Create helper utilities for `timestampToFrameIndex()` and `frameIndexToX()` so all renderers share the same logic
> * Propose a debug overlay that can visualize frame buckets for calibration
>
> I’ll provide you any further files needed. You can assume this is part of a high-performance GPU C++ system using Qt + VBOs. Just give me the best path forward and specific refactors to unify these systems.