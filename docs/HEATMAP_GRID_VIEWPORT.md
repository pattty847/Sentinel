# Heatmap Grid, Columns, and Viewport — Mental Model

A short reference for how the 2D heatmap texture, time columns, price rows, and viewport fit together.

---

## 1. The 2D “spreadsheet” (texture)

The heatmap lives in a **single GPU texture** of size **N × N** (e.g. **2048 × 2048**). That’s your “Excel sheet”:

- **Horizontal (X)** = **time**. One texture column = one time bucket (e.g. 100 ms).
- **Vertical (Y)** = **price**. One texture row = one price level (one tick).

So:

- **Grid size** `gridSize` = both width and height of that texture (e.g. 2048).
- **Columns** = the 2048 vertical strips. Each column is **1 pixel wide** and **gridSize pixels tall**. One incoming “slice” from the server fills exactly one column (one timeframe bucket, all price levels).
- **Rows** = the 2048 horizontal strips. Each row is one price level; the server sends `gridHeight` rows per column (often equal to `gridSize`).

So the “spreadsheet” is **gridSize columns × gridSize rows** (e.g. 2048×2048). Not infinite: **fixed size**.

---

## 2. Ring buffer in time

We only keep the **last N time buckets** in the texture. New data overwrites the oldest column:

- **Write cursor** `writeColumn` = next column to fill (0 … gridSize−1).
- **Oldest column** = `(writeColumn + 1) % gridSize`.
- When a new slice arrives, we write it into column `writeColumn`, then advance: `writeColumn = (writeColumn + 1) % gridSize`.

So the texture is a **ring buffer** in the time (X) direction. The “data window” in time is:

- **Length**: `gridSize × appendMs` milliseconds (e.g. 2048 × 100 ms ≈ 204.8 s).
- **End time**: last slice’s `sliceStartMs + appendMs`.
- **Start time**: end − (gridSize × appendMs).

When data “gets to the edge”, we don’t grow the sheet: we **wrap**. The oldest column is reused for the newest bucket.

---

## 3. Viewport = what you’re looking at

The **viewport** is the **visible window in (time, price)**. It is **not** the whole texture; it’s the part of the logical (time, price) plane that is currently on screen.

- **Viewport** = `(visibleTimeStart, visibleTimeEnd, minPrice, maxPrice)`.
- User **zoom** changes the time span and/or price range (e.g. less time = zoomed in).
- User **pan** shifts that window (e.g. scroll left/right in time, up/down in price).

So:

- **Viewport** = “which rectangle of (time, price) we draw” — can be the full buffer or a smaller zoomed/offset portion.
- **Texture** = fixed N×N buffer that holds the last N time columns and N price rows.

The renderer maps the **intersection** of viewport and data range into **texture coordinates** (`srcRect`) and **screen coordinates** (`drawRect`). So you might be looking at the whole 2048×2048, or a small zoomed rectangle that uses only part of the texture.

---

## 4. Summary table

| Concept        | Meaning |
|----------------|--------|
| **Grid size**  | N. Texture is N×N (e.g. 2048×2048). |
| **Column**     | One vertical strip = one time bucket = 1 texel wide, N texels tall. |
| **Row**        | One horizontal strip = one price level. |
| **Texture**   | The N×N “sheet”; time = X, price = Y; ring buffer in X. |
| **Viewport**   | Visible (time, price) window; can be full buffer or a zoomed/paned portion. |
| **Data at edge** | Time wraps: oldest column is overwritten by newest; no growth, fixed size. |

---

## 5. Code anchors

- **Grid size**: `HeatmapStreamState::m_gridSize`, `UnifiedGridRenderer::m_heatmapGridSize`; server can send `gridHeight` and trigger `heatmapRangeReset(minPrice, maxPrice, tickSize, gridHeight)`.
- **Columns**: one slice → one `PendingColumn` with `x = writeColumn`; upload via `glTexSubImage2D(..., x, 0, 1, height, ...)`.
- **Ring cursor**: `HeatmapStreamState::m_writeColumn`, `(writeColumn + 1) % gridSize` = oldest.
- **Viewport**: `GridViewState::setViewport(timeStart, timeEnd, priceMin, priceMax)`; used in `UnifiedGridRenderer::updatePaintNode` to compute `srcRect` / `drawRect` from overlap of viewport and data range.
- **Time shift for ring**: `HeatmapStreamState::updateTimeOffset(fractionalOffset)` → `timeOffset`; fragment shader uses `fract(v_texcoord.x + params.w)` so the ring buffer wraps correctly when sampling.
