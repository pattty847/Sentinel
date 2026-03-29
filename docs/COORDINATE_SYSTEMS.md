# Sentinel Coordinate Systems and Render Contracts

This document defines the coordinate spaces used by chart rendering and the contract each renderer family must follow. It exists to prevent bugs that occur when a layer mixes contracts (for example, a timeline TPO sampling X from the heatmap `srcRect.x`, causing drift or stretch on pan).

## 1. Canonical coordinate spaces

| Space | Axes | Meaning | Produced / owned by |
|-------|------|---------|---------------------|
| **World** | `time_ms`, `price` | Market truth; independent of viewport and texture layout | `TimeAxisMapping` inputs and helpers |
| **Grid / texture** | `column`, `row` | Ring-buffer and texture indexing (`gridWidth` × `gridHeight`) | Stream state and upload paths (`HeatmapStreamState`, `FootprintStreamState`, `TpoStreamState`) |
| **Screen** | `x`, `y` (pixels) | Final rendered coordinates in the QQuickItem draw area | Per-frame `drawRect` and `srcRect` mapping |

## 2. Single source of truth for world↔screen

`TimeAxisMapping` is authoritative for frame mapping:

- **World → screen:** `timeToScreenX()`, `priceToScreenY()`
- **Screen → world:** `screenXToTime()`, `screenYToPrice()`

Any world-semantic renderer (candles, labels, volume profile price bands) must map through this authority. Do not implement ad-hoc world↔screen math when these helpers exist.

## 3. Renderer families and contracts

### 3.1 Texture-quad overlays

Use when data is a dense 2D grid and incremental column texture uploads matter.

**Heatmap (`HeatmapOverlayRenderer`)**  
- Contract: ring-texture sampling.  
- Uses `drawRect`, `srcRect`, and `timeOffset` in the shader path.  
- `timeOffset` is valid here; X/Y sampling is bound to heatmap grid semantics.

**Footprint (`FootprintOverlayRenderer`)**  
- Contract: ring-texture sampling over the footprint delta grid.  
- May share `srcRect` when dimensions match the heatmap grid; computes its own wrapped time offset for ring alignment.  
- Ring-coupled behavior is intentional; do not treat as session-timeline.

**TPO texture path (`TpoOverlayRenderer`)**  
Two distinct contracts:

1. **HorizontalProfile**  
   - Rank-indexed profile columns.  
   - Texture-space semantic is not candle-time aligned.

2. **VerticalTimeline**  
   - Session-time anchored columns; X-domain is the session time range `[sessionStartMs, sessionEndMs]` (texture columns map to period indices).  
   - **Must not** sample texture X from the heatmap `srcRect.x`; doing so causes TPO to drift/stretch on pan.  
   - Texture X must use full session coverage; only Y clipping may be taken from the viewport `sourceRect`.  
   - Draw rect is clipped to the visible portion of the session range via `computeTimelinedDrawRect()`.

### 3.2 Geometry overlays

Use when primitives are sparse and semantic (bars, lines, labels) and must be invariant to texture ring internals.

**Candles (`CandlestickOverlayItem`)**  
- Contract: world-semantic.  
- X: `mapping.timeToScreenX(...)`; Y: `mapping.priceToScreenY(...)`.  
- Does not use heatmap `timeOffset`.

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
5. Add a single debug log line (behind `SENTINEL_CHART_DEBUG`) showing the active domains.
6. When introducing a new mapping rule, add or update an invariant in `_agent/INVARIANTS.md`.

## 7. Reference regression (TPO VerticalTimeline)

On `tpo-footprint-v1`, `TpoOverlayRenderer` VerticalTimeline previously used the heatmap `sourceRect.x` for texture X. That made TPO appear to grow or shrink while panning. The fix was to lock VerticalTimeline texture X to full session coverage and keep viewport-driven Y clipping only. This is the reference example for keeping coordinate contracts explicit.

---

*See also: `libs/gui/render/TimeAxisMapping.hpp`, `_agent/INVARIANTS.md` (INV-004, INV-005, INV-031–INV-036), and `docs/ARCHITECTURE.md` (Coordinate System: TimeAxisMapping).*
