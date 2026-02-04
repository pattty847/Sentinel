# Heatmap + Candles Alignment (Option A: World-Space Axis)

## 1. Audit current heatmap mapping

### Renderer mapping + draw/src rect math
- **`UnifiedGridRenderer::updatePaintNode()`** computes `drawRect` and `srcRect` based on the visible viewport and heatmap data range, then pushes those into the `HeatmapIntensityNode` (single quad + texture sampling). This is where **world time range (viewport)** is intersected with **dataStart/dataEnd** and mapped into source texture coordinates. It also stores the last mapping into `HeatmapTimeMapping` for the candle overlay to consume. 【F:libs/gui/UnifiedGridRenderer.cpp†L700-L927】
- **`HeatmapTimeMapping`** is the current cross-component contract (drawRect/srcRect/dataStartMs/appendMs/gridWidth/timeOffset, etc.) used by the candle overlay to align geometry. 【F:libs/gui/render/HeatmapTimeMapping.hpp†L1-L16】
- **`HeatmapIntensityNode`** accepts a `timeOffset` uniform, which shifts sampling for ring-buffered columns. 【F:libs/gui/render/HeatmapIntensityNode.hpp†L26-L43】

### Data/time range derivation
- **`dataStart/dataEnd`** are derived inside `UnifiedGridRenderer` from `HeatmapStreamState::Snapshot` (gridWidth, appendMs, lastSliceStartMs, timeOriginMs) and stored into the last mapping used by candles. 【F:libs/gui/UnifiedGridRenderer.cpp†L742-L878】
- **`actualDataStart/actualDataEnd`** represent the filled window (filled columns) and are calculated in the same region, influencing candle filtering. 【F:libs/gui/UnifiedGridRenderer.cpp†L757-L878】

### Ring buffer + timeOffset semantics
- **`HeatmapStreamState`** owns the write cursor, ring buffer contents, and the **`timeOffset`** that maps the oldest column into the texture origin. `updateTimeOffset()` computes a fractional offset based on the write column and a fractional offset, then stores it for the renderer. 【F:libs/gui/render/HeatmapStreamState.cpp†L268-L294】
- **`HeatmapStreamState::Snapshot`** carries the fields used to reconstruct the data span (`gridWidth`, `appendMs`, `lastSliceStartMs`, `timeOriginMs`, `filledColumns`) and the ring offset (`timeOffset`). 【F:libs/gui/render/HeatmapStreamState.hpp†L26-L59】

### Candle overlay alignment points
- **`CandlestickOverlayItem::updatePaintNode()`** consumes `HeatmapTimeMapping` and derives the candle column index from `actualDataStartMs` / `appendMs` and the mapping’s source rect + timeOffset. This is the current candle/heatmap alignment behavior. 【F:libs/gui/render/CandlestickOverlayItem.cpp†L240-L466】

## 2. Option A mapping strategy (world-space axis + single quad)

### Core idea
Use **world-space time** (`viewport timeStart/timeEnd`) as the single source of truth for both candles and heatmap. The heatmap continues to draw as a **single quad**, but its **texture sampling is mapped to world time** via a shared mapping that understands:
- `dataStartMs` / `dataEndMs` (full ring span)
- `actualDataStartMs` / `actualDataEndMs` (filled span)
- `appendMs` (bucket width)
- `gridWidth` (ring capacity)
- `timeOffset` (ring alignment)

### Mapping definition (conceptual)
- Define `colF = (timeMs - dataStartMs) / appendMs` as **world-time → column index** (float). This is stable across both heatmap and candles.
- The heatmap’s visible world range is `viewport [timeStart, timeEnd]`, intersected with `dataStart/dataEnd` and **clamped to `actualDataStart/actualDataEnd`** so that **no heatmap or candles appear where there is no data**.
- Keep the **single quad** and set:
  - `drawRect` from world-time overlap → screen-space (same as today).
  - `srcRect` from overlap → column indices (same as today), but define it in **world-space columns**, not ring-order columns.
  - `timeOffset` stays the only ring-buffer correction: the shader (or node) uses it to wrap column sampling into the actual ring location.

### Ring buffer wrap in world-space terms
- The ring has a **logical index** (`colF` from world time) and a **physical index** (`(colF + oldestColumn) mod gridWidth`).
- The existing `timeOffset = oldestColumn / gridWidth` is still valid, but the mapping must be explicit that **`srcRect.x` is expressed in logical columns** (world-space), not ring columns.
- Sampling becomes:
  - `logicalCol = srcRect.x + u * srcRect.width`
  - `physicalCol = (logicalCol + timeOffset * gridWidth) mod gridWidth`
- This keeps the quad count at 1 while enforcing a **world-space axis** and correct wrap.

## 3. Shared time mapping interface

### Proposed API (single source of truth)
Introduce a lightweight, renderer-local mapping struct to remove duplicated math between heatmap and candles:
- `TimeAxisMapping` (new file in `libs/gui/render/`):
  - `double dataStartMs; double dataEndMs;`
  - `double actualStartMs; double actualEndMs;`
  - `double appendMs; int gridWidth; float timeOffset;`
  - `QRectF drawRect; QRectF srcRect;`
  - `double worldToColumn(double timeMs) const;`
  - `double columnToWorld(double colF) const;`
  - `double bucketWidthMs() const;`
  - `std::pair<double, double> visibleWorldRange() const;`

### Ownership / location
- **Owner:** `UnifiedGridRenderer` (the only place that already computes both viewport overlap and heatmap data ranges). It should emit or expose the `TimeAxisMapping` for use by `CandlestickOverlayItem`. 【F:libs/gui/UnifiedGridRenderer.cpp†L742-L878】【F:libs/gui/render/CandlestickOverlayItem.cpp†L240-L466】
- **Location:** `libs/gui/render/TimeAxisMapping.hpp` (renderer/local, not a global util) so it remains GPU-renderer specific.

### Performance expectations
- The API is **pure math**, no allocations, no per-column geometry. It formalizes the current mapping while keeping the **single quad + texture sampling** path intact.

## 4. Risks & edge cases

- **Ring wrap precision:** floating-point drift around `timeOffset * gridWidth` can shift columns by sub-pixel amounts. Use consistent double/float conversions and prefer stable `appendMs` math. 【F:libs/gui/render/HeatmapStreamState.cpp†L268-L294】【F:libs/gui/UnifiedGridRenderer.cpp†L921-L925】
- **Partial data windows:** when `filledColumns < gridWidth`, `actualDataStart/End` must clamp both heatmap and candle visibility, or candles can show in empty ranges. 【F:libs/gui/UnifiedGridRenderer.cpp†L757-L878】【F:libs/gui/render/CandlestickOverlayItem.cpp†L311-L350】
- **History window mismatch:** candle history might extend farther than the heatmap data window; enforce the **heatmap data window as the cap** to prevent candle-only ranges. 【F:libs/gui/render/CandlestickOverlayItem.cpp†L311-L350】
- **Viewport lag/pan:** `GridViewState` can apply pan visual offset during drag; ensure world-time mapping uses the same viewport offsets so candles and heatmap stay aligned. 【F:libs/gui/UnifiedGridRenderer.cpp†L771-L806】【F:libs/gui/render/GridViewState.cpp†L167-L213】
- **Precision at large timestamps:** `timeStart/timeEnd` are millisecond timestamps; avoid integer truncation when computing `colF` and `srcRect` bounds.

## 5. Implementation steps (no code)

1. **Document current mapping contract**
   - Confirm the exact fields populated in `HeatmapTimeMapping` and how `CandlestickOverlayItem` derives column positions from it. 【F:libs/gui/render/HeatmapTimeMapping.hpp†L1-L16】【F:libs/gui/render/CandlestickOverlayItem.cpp†L311-L466】

2. **Define `TimeAxisMapping` (new header)**
   - Specify a world-space mapping struct in `libs/gui/render/` with functions for time→column and column→time. Keep it POD-like and inline, no allocations.

3. **Refactor `UnifiedGridRenderer` to be the sole mapping source**
   - Replace internal/implicit mapping math with `TimeAxisMapping` fields (still computing `drawRect` and `srcRect` once per frame). Store it as the last mapping and expose it for candles, replacing or augmenting `HeatmapTimeMapping`. 【F:libs/gui/UnifiedGridRenderer.cpp†L742-L878】

4. **Align candle overlay to world-space mapping**
   - Update `CandlestickOverlayItem` to use the new mapping methods (`worldToColumn`, `bucketWidthMs`, `visibleWorldRange`) so the candle range matches heatmap visibility and respects `actualDataStart/End`. 【F:libs/gui/render/CandlestickOverlayItem.cpp†L311-L466】

5. **Ring wrap validation**
   - Verify that `timeOffset` still maps the logical column to the correct ring column in the shader. The quad stays single; only sampling math changes. 【F:libs/gui/render/HeatmapStreamState.cpp†L268-L294】【F:libs/gui/render/HeatmapIntensityNode.hpp†L26-L43】

6. **Stage changes safely**
   - Add a temporary debug overlay or log line that dumps `TimeAxisMapping` (timeStart/timeEnd, srcRect, timeOffset) to compare current vs. new behavior (use existing `getViewportMathDebug()` infrastructure). 【F:libs/gui/UnifiedGridRenderer.cpp†L1397-L1524】

7. **Validation checklist**
   - Pan/zoom while live data streams in and confirm:
     - First candle aligns with first heatmap slice.
     - Candles never appear where heatmap has no data.
     - No performance regressions (still one quad + texture). 【F:libs/gui/UnifiedGridRenderer.cpp†L742-L878】【F:libs/gui/render/CandlestickOverlayItem.cpp†L311-L466】

