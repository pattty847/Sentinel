# Viewport / coordinate / transform architecture

Reference for auditing heatmap vs candle alignment and choosing a unification strategy (Option A vs B). All paths and line references are to the current codebase.

---

## 1) View state ownership

**Authoritative object:** `GridViewState`  
**Files:** `libs/gui/render/GridViewState.hpp`, `libs/gui/render/GridViewState.cpp`

**Stored state:**

| Field | Type | Meaning |
|-------|------|----------|
| `m_visibleTimeStart_ms` | qint64 | Left edge of visible time window |
| `m_visibleTimeEnd_ms` | qint64 | Right edge of visible time window |
| `m_minPrice` / `m_maxPrice` | double | Visible price range (Y) |
| `m_viewportWidth` / `m_viewportHeight` | double | Size of the grid item (pixels) |
| `m_panVisualOffset` | QPointF | Pixel offset during/after drag (accumulated delta) |
| `m_zoomFactor` | double | Zoom level |
| `m_viewportVersion` | uint64_t | Bumped on any viewport/size change (rebuild signal) |
| `m_isDragging` | bool | True while user is dragging |
| `m_timeWindowValid` | bool | True once viewport has been set |

**Invariant (AGENTS.md):** Zoom/pan must update via `setViewport()` so `viewportVersion` increments. Never mutate viewport fields directly.

---

## 2) Who updates the viewport

- **UnifiedGridRenderer** calls:
  - `GridViewState::setViewport(timeStart, timeEnd, priceMin, priceMax)` when the view changes (e.g. zoom, pan-end, auto-scroll).
  - `GridViewState::setViewportSize(w, h)` in `geometryChange()` and `componentComplete()`.
- **ViewportAutoScrollController** drives viewport updates for auto-scroll (via the renderer/view state).
- `viewportVersion` is incremented in `setViewport()` and `setViewportSize()` only.

---

## 3) Heatmap render path (current)

Heatmap rendering **does not** use `GridViewState::calculateViewportTransform()`.

**Flow:**

1. **Data:** Slices come from server → `DataProcessor::onHeatmapSliceReceived` / `heatmapColumnReady` → **HeatmapStreamState::ingestSlice()** (`libs/gui/render/HeatmapStreamState.cpp`). State is ring-buffer: columns by index, Y = price bucket index (tick size per row).

2. **Rendering:** `UnifiedGridRenderer::updatePaintNode()` (GPU heatmap branch, ~706–987).  
   - Reads **GridViewState**: `getVisibleTimeStart/End()`, `getMinPrice()`, `getMaxPrice()`, `getPanVisualOffset()`, `isDragging()`.
   - **Pan during drag only:** If `!pan.isNull() && isDragging()`, it adjusts the *logical* view window used for overlap (lines 775–788):
     - `timePixelsToUnits = timeRange / bounds.width()`, `pricePixelsToUnits = priceRange / bounds.height()`
     - `timeDelta = -pan.x() * timePixelsToUnits`, `priceDelta = pan.y() * pricePixelsToUnits`
     - Then `timeStartF += timeDelta`, `timeEndF += timeDelta`, `minPriceF += priceDelta`, `maxPriceF += priceDelta`.
   - **Overlap math (lines 805–847):** View window `[timeStartF, timeEndF]` × `[minPriceF, maxPriceF]` is intersected with heatmap data window `[dataStart, dataEnd]` × `[dataMin, dataMax]` (data span from `lastSliceStartMs`, `appendMs`, `gridWidth`; price from `snapshot.minPrice/maxPrice`). From the overlap it computes:
     - **Screen rect** `drawRect`: time/price ratios map into `bounds` (e.g. `bounds.x() + bounds.width() * timeRatioStart`, etc.).
     - **Texture source rect** `setSourceRect(QRectF(srcX, srcY, srcW, srcH))`:
       - `srcX = (overlapStart - dataStart) / snapshot.appendMs`
       - `srcY = (snapshot.maxPrice - overlapMax) / snapshot.tickSize`
       - So **time → column index** = `(time - dataStart) / appendMs`, **price → row index** = `(maxPrice - price) / tickSize`.
   - **HeatmapIntensityNode:** `setRect(drawRect)`, `setSourceRect(...)`, `setTimeOffset(snapshot.timeOffset)`. The node draws a single quad; no screen-space pan is applied to the quad—pan is only baked into the shifted view window used for overlap (during drag).

3. **HeatmapStreamState::updateTimeOffset()** (`HeatmapStreamState.cpp` ~266–282): `timeOffset` is a fractional column offset for smooth scrolling (0..1 over grid width), used in the shader and for label positioning.

**Summary:** Heatmap mapping is **view-based overlap** in `UnifiedGridRenderer::updatePaintNode()`: view time/price range (with pan applied only while dragging) → overlap with data time/price → `drawRect` (screen) and `sourceRect` (texture column/row). No use of `calculateViewportTransform()`; no screen-space `panVisualOffset` applied to the heatmap quad after the fact.

---

## 4) Candle overlay render path

**File:** `libs/gui/render/CandlestickOverlayItem.cpp`

- Candles use **GridViewState::calculateViewportTransform(boundingRect())** (line 353).
- **Transform (GridViewState.cpp ~53–75):**
  - `sx = viewportWidth / (timeEnd - timeStart)`
  - `sy = -viewportHeight / (maxPrice - minPrice)`
  - World: scale(sx, sy), then translate(-timeStart, -maxPrice). Y flip from `sy < 0`.
  - Then, if `panVisualOffset` is non-null: **screen-space** translate(pan.x(), pan.y()) applied *first* in the combined matrix (so: `screenSpace * worldTransform`).
- Candle geometry: world (timestamp_ms, price) is mapped with `mapWorld(xform, time, price)` to screen; candles use **continuous** time/price, not bucket indices.
- **Pan:** Applied **always** when `panVisualOffset` is non-null (no `isDragging()` check in the transform). So candles get a persistent screen-space offset whenever pan is set.

---

## 5) Axis models (QML)

**Files:** `libs/gui/models/AxisModel.cpp`, time/price axis models.

- **Source of data:** `UnifiedGridRenderer` (target) → `GridViewState` from renderer.
- They use: `visibleTimeStart/End`, `minPrice`/`maxPrice`, viewport size, and **panVisualOffset** (via `panVisualOffsetChanged` and polling view state).
- Used for labels/ticks overlays only; they do not define the heatmap’s core mapping.

---

## 6) Coordinate spaces

| Space | X | Y | Used by |
|-------|---|---|--------|
| **Heatmap (internal)** | Column index (time bucket: `appendMs` per column) | Row index (price bucket: `tickSize` per row) | HeatmapStreamState, texture, overlap math in UnifiedGridRenderer |
| **Heatmap (data time/price)** | `dataStart + columnIndex * appendMs` | `maxPrice - rowIndex * tickSize` (or equivalent) | Overlap vs view window |
| **Candle / viewport** | `timestamp_ms` (continuous) | Price (continuous) | CandlestickOverlayItem, calculateViewportTransform |
| **Screen** | Pixels (0..viewportWidth) | Pixels (0..viewportHeight) | drawRect, candle transform output, axes |

---

## 7) Transform math (GridViewState) — candles only

**Function:** `GridViewState::calculateViewportTransform(const QRectF& itemBounds)`  
**File:** `libs/gui/render/GridViewState.cpp` lines 53–75.

- `sx = m_viewportWidth / timeRange`, `sy = -m_viewportHeight / priceRange`
- `transform.scale(sx, sy, 1.0); transform.translate(-timeStart, -maxPrice, 0);`
- If `!m_panVisualOffset.isNull()`: `screenSpace.translate(pan.x(), pan.y()); transform = screenSpace * transform;`
- **Used only by the candle overlay**, not by the heatmap.

---

## 8) Where heatmap vs candles can diverge

1. **Pan application:** Heatmap applies pan only **during drag** by shifting the logical view window and recomputing overlap; it does **not** apply `panVisualOffset` as a screen-space translation to the quad. Candles apply `panVisualOffset` in the transform **whenever it is non-null**. So during drag, both move, but the heatmap uses “shifted window” and candles use “same window + pixel offset”—only equivalent if the math matches exactly.
2. **Different mapping sources:** Heatmap uses overlap of (time/price) view with (time/price) data and then maps to texture columns/rows and to screen via ratios. Candles use a single linear transform from world (time, price) to screen. Any difference in time or price origin/scale (e.g. dataStart vs visibleTimeStart, or tick alignment) can cause misalignment.
3. **Data range:** Heatmap history can span a wide time range (server); candle buffer is client-side and may be shallow. Candles can “cluster” in time if the buffer doesn’t cover the visible range, even when the mapping is correct.

---

## 9) Data ranges

- **Heatmap:** Server-authoritative; history can span a long time; `gridWidth` × `appendMs` and `lastSliceStartMs` define the current data window.
- **Candles:** Client buffer (e.g. CandleSeriesBuffer); typically shorter unless the server has been running and feeding history. Visible window is `visibleTimeStart_ms`..`visibleTimeEnd_ms` from GridViewState.

---

## 10) Check (audit checklist)

Use these to verify heatmap behaviour when comparing with candles or changing the pipeline.

### How heatmap computes `dataStart` and `appendMs`

**File:** `libs/gui/UnifiedGridRenderer.cpp` in `updatePaintNode()`, lines 806–811.

- **`appendMs`:** Comes from **HeatmapStreamState::Snapshot** (same as slice timeframe). Set when slices are ingested: `HeatmapStreamState::ingestSlice()` uses `timeframeMs` and stores it as `m_appendMs`; snapshot exposes it as `snapshot.appendMs`. So **appendMs = timeframe per column** (e.g. 60_000 for 1‑minute buckets).
- **`dataStart` / `dataEnd`:**
  - `bufferSpanMs = gridWidth * snapshot.appendMs`
  - `dataEnd = (lastSlice != min) ? (lastSlice + appendMs) : (timeOriginMs + bufferSpanMs)`
  - `dataStart = dataEnd - bufferSpanMs`
  So the heatmap’s logical time window is **one full ring** of columns: from `dataStart` to `dataEnd`, with each column spanning `appendMs`. Column index `i` corresponds to time `dataStart + i * appendMs`.

### How `drawRect` is computed in `UnifiedGridRenderer::updatePaintNode()`

**Same file,** lines 815–837 (inside the overlap branch).

1. **Overlap in time/price:**  
   `overlapStart = max(timeStartF, dataStart)`, `overlapEnd = min(timeEndF, dataEnd)`, and similarly for price (`overlapMin`, `overlapMax`) using view and data bounds.
2. **Ratios within the view window:**  
   - `timeRatioStart = (overlapStart - timeStartF) / viewTimeSpan`  
   - `timeRatioEnd = (overlapEnd - timeStartF) / viewTimeSpan`  
   - `priceRatioTop = (maxPriceF - overlapMax) / viewPriceSpan`  
   - `priceRatioBottom = (maxPriceF - overlapMin) / viewPriceSpan`
3. **drawRect in item coordinates (pixels):**  
   `drawRect = QRectF(bounds.x() + bounds.width() * timeRatioStart, bounds.y() + bounds.height() * priceRatioTop, bounds.width() * (timeRatioEnd - timeRatioStart), bounds.height() * (priceRatioBottom - priceRatioTop))`.  
   So **drawRect** is the screen sub-rectangle that corresponds to the overlap: view time/price range is mapped linearly onto `bounds`, and the overlap interval is that sub-rect.

### Whether heatmap uses `panVisualOffset` after drag end

**It does not.**

- **During drag:** Pan is used only to **shift the logical view window** (`timeStartF`, `timeEndF`, `minPriceF`, `maxPriceF`) in the same block (lines 775–788), and only when `m_viewState->isDragging()` is true. That shifted window is then used for overlap and thus for `drawRect` and `sourceRect`.
- **After drag end:** `handlePanEnd(true)` in `GridViewState` commits the pan into the viewport (`setViewport(...)`) and does **not** keep a residual screen-space offset; the heatmap code never applies `panVisualOffset` to the quad geometry. So once drag ends, the heatmap uses the new viewport only; there is no “leftover” pan applied to the heatmap after drag end.

---

## 11) Options for alignment (from your outline)

- **Option A:** Refactor heatmap to use `GridViewState::calculateViewportTransform()` (or a shared time/price → screen mapping) so heatmap and candles share one transform pipeline (true unification).
- **Option B:** Make candles use the heatmap’s internal mapping (overlap + drawRect/sourceRect style) for short-term alignment.

**Exact heatmap mapping (for Option A/B):** Implemented in `UnifiedGridRenderer::updatePaintNode()` (~763–862): view window (with pan applied only when `isDragging()`) → overlap with `[dataStart, dataEnd]` × `[dataMin, dataMax]` → `drawRect` (screen) and `setSourceRect(srcX, srcY, srcW, srcH)` with `srcX = (overlapStart - dataStart) / appendMs`, `srcY = (maxPrice - overlapMax) / tickSize`. Heatmap quad is drawn at `drawRect` with no extra screen-space pan. Time offset for smooth scroll is `HeatmapStreamState::updateTimeOffset()` and `snapshot.timeOffset` in the shader/labels.

Once the exact behaviour you want (e.g. same pan semantics as candles, or same time/price origin as heatmap) is decided, this doc gives the exact locations to change for Option A or B.
