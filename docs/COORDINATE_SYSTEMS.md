# Sentinel Coordinate Systems and Render Contracts

This document defines the coordinate spaces used by chart rendering and the contract each renderer family must follow. It exists to prevent bugs that occur when a layer mixes contracts (for example, a timeline TPO sampling X from the heatmap `srcRect.x`, causing drift or stretch on pan).

## 1. Canonical coordinate spaces

| Space | Axes | Meaning | Produced / owned by |
|-------|------|---------|---------------------|
| **World** | `time_ms`, `price` | Market truth; independent of viewport and texture layout | `TimeAxisMapping` inputs and helpers |
| **Grid / texture** | `column`, `row` | Ring-buffer and texture indexing (`gridWidth` × `gridHeight`) | Stream state and upload paths (`FootprintStreamState`, `TpoStreamState`) |
| **Screen** | `x`, `y` (pixels) | Final rendered coordinates in the QQuickItem draw area | Per-frame `drawRect` and `srcRect` mapping |

## 2. Single source of truth for world↔screen

`TimeAxisMapping` is authoritative for frame mapping:

- **World → screen:** `timeToScreenX()`, `priceToScreenY()`
- **Screen → world:** `screenXToTime()`, `screenYToPrice()`

Any world-semantic renderer (candles, labels, volume profile price bands) must map through this authority. Do not implement ad-hoc world↔screen math when these helpers exist.

The mapping is the frame's raster camera (`render/ChartRaster.hpp`): its `view*` bounds are the drawn window, row and column edges sit on whole device pixels (`rowPxDev`/`colPxDev` per row/column, `dpr`), and a drag is already baked in as whole device pixels. Never add `panVisualOffset` or the stored `GridViewState` bounds on top of it; GUI-thread code that needs the next frame's mapping uses `UnifiedGridRenderer::rasterCameraNow()`.

## 3. Renderer families and contracts

### 3.1 Texture-quad overlays

Use when data is a dense 2D grid and incremental column texture uploads matter.

The heatmap (`HeatmapTileNode`, S6) is not a ring texture: it draws the span sources
for the viewport-only `TimeAxisMapping` (columns anchored to epoch multiples of the
timeframe, rows at the drawn tick). The legacy ring heatmap went in S8a.

**Footprint (`FootprintOverlayRenderer`)**  
- Contract: ring-texture sampling over the footprint delta grid.  
- May share `srcRect` when dimensions match the heatmap grid; computes its own wrapped time offset for ring alignment.  
- Ring-coupled behavior is intentional; do not treat as session-timeline.

**TPO (`TpoOverlayRenderer`)** is a geometry overlay: see 3.2.

### 3.2 Geometry overlays

Use when primitives are sparse and semantic (bars, lines, labels) and must be invariant to texture ring internals.

**Candles (`CandlestickOverlayItem`)**  
- Contract: world-semantic.  
- X: `mapping.timeToScreenX(...)`; Y: `mapping.priceToScreenY(...)`.  
- Does not use heatmap `timeOffset`.

**TPO profiles (`TpoOverlayRenderer`)**  
- Contract: world-semantic per session, from `surfaceBounds` + view time/price (INV-037), never the heatmap `drawRect`/`srcRect`.  
- Geometry is built in session layout space: x from the session start (split: `period * periodPx`; collapsed: `k * cellW`), y from the top of the trade grid (`(topAbs - (G+1)*group) * tickPx`). A per-session `QSGTransformNode` translates it to the screen, so panning only changes the matrix.  
- Split keeps the session X-domain `[sessionStartMs, sessionEndMs]` (INV-034). Collapsed profiles anchor at the session start and stick to the plot's left edge while the session is still on screen.

**Labels / axis text**  
- Contract: world-semantic placement with pixel-density constraints.  
- Must use `TimeAxisMapping` and the label density policy.

**Volume Profile (`VolumeProfileRenderer`)**  
- Contract: price-domain geometry.  
- Y mapping uses view min/max price and bin price levels.  
- X anchoring is visual only (right/left/overlay); it must not change price-mapping semantics.

## 4. Decision table: texture-quad vs geometry

| Use **texture-quad** when | Use **geometry** when |
|---------------------------|------------------------|
| Data is a dense matrix per column | Primitives are semantic (candles, profile bars, lines) |
| Incremental column texture upload matters | Exact world alignment matters more than upload density |
| Nearest/linear sampling fits the visual goal | Logic should be robust to ring-buffer implementation changes |
| Ring-wrap semantics are desired | — |

## 5. Forbidden cross-contract patterns

- Using heatmap shader `timeOffset` in candle or label mapping.
- Using heatmap `srcRect.x` to drive session-timeline layers (e.g. TPO VerticalTimeline).
- Implementing ad-hoc world↔screen math in overlays when `TimeAxisMapping` helpers exist.
- Changing volume-profile anchoring in a way that alters its price-mapping semantics.

## 6. Implementation checklist for new overlays

1. Classify the layer: **texture-quad** or **geometry**.
2. Declare the coordinate contract in a header comment.
3. If world-semantic, use `TimeAxisMapping` helpers only.
4. If texture-semantic, document ring vs timeline semantics for X and Y separately.
5. Add a single probe (`sLog_Probe("viewport.domains", ...)`, enabled with `SENTINEL_PROBES`) showing the active domains.
6. When introducing a new mapping rule, add or update an invariant in `_agent/INVARIANTS.md`.

## 7. Reference regression (TPO VerticalTimeline)

On `tpo-footprint-v1`, `TpoOverlayRenderer` VerticalTimeline previously used the heatmap `sourceRect.x` for texture X. That made TPO appear to grow or shrink while panning. The fix was to lock VerticalTimeline texture X to full session coverage and keep viewport-driven Y clipping only. This is the reference example for keeping coordinate contracts explicit.

---

*See also: `libs/gui/render/TimeAxisMapping.hpp`, `_agent/INVARIANTS.md` (INV-004, INV-005, INV-031–INV-036), and `docs/ARCHITECTURE.md` (Coordinate System: TimeAxisMapping).*
