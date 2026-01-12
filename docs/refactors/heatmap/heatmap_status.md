# Heatmap Refactor Status (Session Summary)

This document captures the current implementation state and what to load first when resuming.

## Current Behavior (Working)
- Dummy GPU heatmap renders as a **single quad** via `QSGSimpleTextureNode`.
- Handles **8192x8192** dummy data (67M cells) without geometry rebuilds.
- **Pan and zoom** are smooth and behave as expected (including extreme zoom).
- **FPS overlay** shows real-time FPS.
- Dummy data now resembles **order-book bands** (asks on top, bids on bottom).
- **Ring-buffer append** works (new columns stream in without full rebuilds).
- **Smooth scrolling** decoupled from data cadence (render tick + wall-clock offset).
- **Zoom-threshold labels** appear when cells are large enough (debug visuals).

## How to Run (Dummy Heatmap)
```
SENTINEL_DUMMY_HEATMAP=1 SENTINEL_DUMMY_GRID=8192 ./build/linux-gcc/apps/sentinel_gui/sentinel_gui
```

Optional debug:
```
SENTINEL_ZOOM_DEBUG=1  # Logs zoom math
```

## Environment Variables
- `SENTINEL_DUMMY_HEATMAP=1` — enables single-quad dummy heatmap path.
- `SENTINEL_DUMMY_GRID=<size>` — sets dummy texture size (e.g., 2048, 4096, 8192).
- `SENTINEL_ZOOM_DEBUG=1` — zoom math logging.

## Build/Runtime Dependencies (WSL)
- Needed QML modules for Slider:
  - `qml6-module-qtquick-controls2`
  - `qml6-module-qtquick-templates2`

## Implemented Changes

### 1) Dummy GPU heatmap path (single quad)
- Implemented in `UnifiedGridRenderer::updatePaintNode`.
- Uses `QSGSimpleTextureNode` with nearest filtering.
- Viewport-mapped source rect; pan is applied while dragging only.
- Clamps source rect safely to avoid stretch or out-of-bounds artifacts.

### 2) Dummy data generator (order-book-like)
- Bands + segment variability + spikes.
- Red top (asks), green bottom (bids).

### 3) Pan/Zoom fixes
- Max zoom increased to **500x**.
- Time range math avoids collapsing to zero.
- Fractional time pan accumulator prevents horizontal pinning at extreme zoom.
- Zoom logging added behind `SENTINEL_ZOOM_DEBUG`.

### 4) FPS overlay
- FPS tracked on render thread and exposed via `getCurrentFPS()`.
- QML overlay updates every 250ms.
- Added a debug line for zoom/time/price/pan under FPS.

### 5) Ring buffer + append (dummy)
- Append-only column updates with a write cursor (no full texture rebuild per tick).
- Partial uploads for a single column using GL texture sub-image updates.
- Time offset interpolated per-frame for smooth scroll.

### 6) Zoom-threshold labels (dummy)
- Cell labels render only when pixel size crosses a threshold.
- Labels follow the heatmap scrolling (source-aligned).

## Files Modified (Key)

**Core rendering + dummy heatmap**
- `libs/gui/UnifiedGridRenderer.h`
- `libs/gui/UnifiedGridRenderer.cpp`
- `libs/gui/render/HeatmapIntensityNode.hpp`
- `libs/gui/render/HeatmapIntensityNode.cpp`
- `libs/gui/render/shaders/heatmap_intensity.vert`
- `libs/gui/render/shaders/heatmap_intensity.frag`

**Zoom/pan math**
- `libs/gui/render/GridViewState.hpp`
- `libs/gui/render/GridViewState.cpp`

**QML FPS overlay**
- `libs/gui/qml/DepthChartView.qml`

**Build fixes**
- `CMakeLists.txt` (added Qt6 QuickControls2)
- `libs/gui/CMakeLists.txt` (link Qt6::QuickControls2)

## Current Focus (Single-Quad Heatmap Goal)
- Render **100M+ cells** (10k x 10k) via a single quad with **GPU shading**.
- **No per-cell CPU loop** during render; CPU only uploads intensity data.
- **Append-only updates** at the right edge (time), with **dirty-region uploads** only.
- **No rebuilds** unless viewport or texture size actually changes.
- Maintain **buttery pan/zoom** at all scales.

## Next Tasks (GPU Heatmap Path Only)
1) Replace dummy RGBA8 texture with **R16** intensity + palette LUT shader.
2) Add a **ring-buffered grid** (time x price) with an append cursor.
3) Implement **dirty-region uploads** (tiles or rects) with a per-frame upload budget.
4) Add **viewport uniforms** (time/price extents) so zoom/pan only updates shader uniforms.
5) Add **mip/prefilter option** for zoomed-out stability (optional, can be a second pass).
6) Add a **perf overlay** for texture upload cost and dirty-rect count.

## Optional Cleanup (Later)
- Remove old heatmap strategy when GPU path is ready.
- Unify viewport zoom math into a pure function for clarity.
