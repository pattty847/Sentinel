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
- DataProcessor can emit **dense column uploads** (slice → BGRA column) for the GPU heatmap path.
- Added **price drift recenter** hooks for GPU heatmap ranges (rare full reset trigger).
- Live data debugging is in progress; GPU heatmap can be driven by **resting book snapshots** (see below).

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
- `SENTINEL_GPU_HEATMAP=1` — enables real data GPU heatmap path (single quad + streamed columns).
- `SENTINEL_HEATMAP_GRID=<size>` — sets GPU heatmap grid size (default 8192).
- `SENTINEL_GPU_HEATMAP_ONLY=1` — disables CPU cell path; use GPU-only rendering.
- `SENTINEL_GPU_HEATMAP_DEBUG=1` — enable GPU heatmap debug logs.
- `SENTINEL_GPU_HEATMAP_DEBUG_MARK=1` — debug mark columns (forces visible marker).
- `SENTINEL_GPU_HEATMAP_FORCE_FULL=1` — force full texture view (bypass viewport mapping).
- `SENTINEL_HEATMAP_FILL_GAPS=1` — fill missing 100ms buckets by carry-forward.
- `SENTINEL_HEATMAP_RESTING=1` — use resting book snapshots instead of LTSE slices for GPU heatmap.
- `SENTINEL_HEATMAP_RECENTER=<fraction>` — recenter threshold for price drift (e.g., 0.15).

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

### 7) GPU heatmap (live data path)
- **GPU-only path** (`SENTINEL_GPU_HEATMAP=1`) streams columns into the single-quad shader.
- **Resting snapshot mode** (`SENTINEL_HEATMAP_RESTING=1`) samples the full LiveOrderBook each 100ms bucket.
- Tick aggregation currently fixed at **$1 per row** for heatmap columns.
- Column gap filling enabled when `SENTINEL_GPU_HEATMAP_ONLY=1` or `SENTINEL_HEATMAP_FILL_GAPS=1`.

## Current Live Data Debug Notes (Active Work)
- We are debugging **live GPU heatmap** behavior (resting book snapshots).
- Observed issues: non-continuous 100ms slice logs, panning/auto-scroll conflicts, and missing columns.
- Suspected root: bucket timer not firing on strict 100ms boundaries and/or snapshot gaps not filled.
- Next step: verify 100ms bucket cadence, snapshot size, and viewport alignment.

## Target Architecture (End State)
- Maintain a **LiveOrderBook** that is updated by deltas.
- **Tick aggregation** buckets levels into **$1 rows** for heatmap rendering.
- **Multiple timeframes** (7 timers) run independently and deterministically.
- Each timer **snapshots the current book** and emits a heatmap column for its timeframe.
- No per-cell CPU loops during render; GPU shades a single quad with streamed columns.

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
