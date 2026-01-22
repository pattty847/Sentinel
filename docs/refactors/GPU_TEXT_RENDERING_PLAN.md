# ✅ GPU Text Rendering — Codex Implementation Spec

## Small additions I’d include
1. **Geometry reuse requirement**  
   Explicitly: “Never allocate new `QSGGeometry` per frame; reuse buffers.”

2. **Font bucket strategy**  
   Add a tiny section: “Use 3–4 fixed font buckets; atlas per bucket; choose based on cell size.”

3. **Char set lock**  
   Spell out the exact chars to support: `0123456789.kMB$+-`

4. **Coord space note**  
   Clarify that label quads must be **data‑relative** (ring/timeOffset), not view‑relative.

5. **Testing checklist**  
   - fast wheel zoom in/out  
   - auto‑scroll on for 10s  
   - long pan across data window  
   - now‑in‑view append stress

**Goal:** Replace QPainter label rendering with GPU‑driven glyph atlas so labels are perfectly synchronized with the heatmap during pan/zoom/auto.

## 0) Constraints / Guardrails
- Render thread only: no locks, no GUI‑thread QPainter per frame.
- Glyph atlas built once (QImage → QSGTexture).
- Label geometry uses data‑relative coordinates and ring/timeOffset.
- Reuse geometry buffers; no per‑frame heap churn.
- Grid dimensions: **8129 × 8129** (price levels × time columns).
- Two text colors only: **white** and **black** (contrast against cell intensity).

---

## 0.1) Label Appearance

**Format:** Compact SI notation — `1.3k`, `2M`, `4B`, `$420.69`  
**Position:** Centered within each cell (both horizontal and vertical).  
**Visibility threshold:** Only render labels when `cellHeight >= 12px`.  
**Baseline:** Vertically center the glyph bounding box, not the font baseline.

---

## 0.2) Font Bucket Strategy

| Bucket | Cell Height Range | Atlas Font Size | Scale Factor      |
|--------|-------------------|-----------------|-------------------|
| 0      | 12–17 px          | 12 px           | cellHeight / 12   |
| 1      | 18–27 px          | 20 px           | cellHeight / 20   |
| 2      | 28–47 px          | 32 px           | cellHeight / 32   |
| 3      | 48+ px            | 48 px           | cellHeight / 48   |

**Selection logic:**
```cpp
int pickFontBucket(float cellHeight) {
    if (cellHeight < 18) return 0;
    if (cellHeight < 28) return 1;
    if (cellHeight < 48) return 2;
    return 3;
}
```

Scale factor is applied to quad size for smooth scaling within each bucket.

---

## 0.3) Capacity Planning

**Worst‑case viewport at min zoom (labels visible):**
- Assume viewport shows ~100 columns × ~80 rows = **8,000 cells** max.
- Average label length: 4 chars (e.g. "1.3k").
- Max quads needed: **8,000 × 4 = 32,000 quads**.
- Pre‑allocate geometry buffer for **32,000 quads** at init.
- 6 verts/quad × 4 floats/vert (pos+uv) × 32k = ~3 MB. Trivial.

---

## 1) Create `GlyphAtlas` (new)

**Files:**
- `libs/gui/render/GlyphAtlas.hpp`
- `libs/gui/render/GlyphAtlas.cpp`

**Responsibilities:**
- Build a single QImage atlas with fixed character set:  
  `0123456789.kMB$+-`
- Track `QRectF` UVs per glyph.
- Expose `QSGTexture*` creation in render thread.

**API Sketch:**
```cpp
class GlyphAtlas {
public:
  struct Glyph { QRectF uv; QSize pixelSize; };
  void build(QFont font, const QString& charset);  // GUI thread
  QSGTexture* createTexture(QQuickWindow* window) const; // render thread
  const Glyph& glyph(QChar c) const;
};
```

**Notes:**
- Build **one atlas per font bucket** (12/20/32/48 px).
- Store glyphs in a map.
- Atlas format: **alpha‑only** (or RGBA with white glyphs). Colorize via vertex color.

---

## 1.1) Text Color Handling

Two colors: **white** (`#FFFFFF`) and **black** (`#000000`).

**Selection:** Based on cell intensity.
```cpp
QColor pickLabelColor(float intensity) {
    return (intensity > 0.5f) ? Qt::black : Qt::white;
}
```

**Implementation:** Add vertex color to `HeatmapGlyphNode` vertex format:
- position (2 floats) + texcoord (2 floats) + color (4 bytes packed as 1 uint)
- Or use `QSGOpaqueTextureMaterial` subclass with color uniform per‑label.

Simpler approach: **two geometry batches** — one for white labels, one for black.
Draw white batch first, black batch second. Two draw calls, still fast.

---

## 2) Create `HeatmapGlyphNode` (new)

**Files:**
- `libs/gui/render/HeatmapGlyphNode.hpp`
- `libs/gui/render/HeatmapGlyphNode.cpp`

**Responsibilities:**
- Own `QSGGeometry` + `QSGTextureMaterial` for glyphs.
- Provide `updateGeometry()` that fills vertex data **in place**.

**Vertex format:**
- 6 vertices per quad (2 triangles).
- Each vertex: position (2f) + texcoord (2f) = 16 bytes/vert.

**API Sketch:**
```cpp
class HeatmapGlyphNode : public QSGGeometryNode {
public:
  void setAtlas(QSGTexture* texture);
  void setColor(const QColor& color);  // white or black
  void ensureCapacity(int maxQuads);
  void updateGeometry(const std::vector<GlyphQuad>& quads);
};
```

**Color approach:** Use two `HeatmapGlyphNode` instances — one white, one black.
Partition labels by intensity threshold, fill each node's geometry separately.

---

## 3) Refactor `HeatmapLabelRenderer`

**Replace** `buildFromSnapshot()` with a **geometry builder**:
- Input: snapshot + viewport + glyph atlas
- Output: `std::vector<GlyphQuad>` (positions in screen coords + atlas UVs)

**New structure:**
```cpp
struct GlyphQuad { QVector2D pos[6]; QVector2D uv[6]; };
```

**Logic changes:**
- Compute visible labels from ring snapshot.
- For each label string:
  - For each char, append quad with correct UVs.
- Use **data‑relative** anchoring + ring/timeOffset alignment.

---

## 4) Update `UnifiedGridRenderer::updatePaintNode`

**Remove**:
- `m_heatmapLabelDirty`
- QPainter image path
- texture swap logic

**Add:**
- `GlyphAtlas` instances (one per font bucket)
- `HeatmapGlyphNode` child node
- Every frame:
  - pick atlas by font bucket
  - generate glyph quads
  - update geometry in place

**Pseudo:**
```cpp
// Init (once)
if (!m_whiteGlyphNode) {
    m_whiteGlyphNode = new HeatmapGlyphNode();
    m_whiteGlyphNode->setColor(Qt::white);
    m_whiteGlyphNode->ensureCapacity(32000);
}
if (!m_blackGlyphNode) {
    m_blackGlyphNode = new HeatmapGlyphNode();
    m_blackGlyphNode->setColor(Qt::black);
    m_blackGlyphNode->ensureCapacity(32000);
}

// Per frame
int bucket = pickFontBucket(cellHeight);
auto [whiteQuads, blackQuads] = buildLabelQuads(snapshot, viewport, atlas[bucket]);

m_whiteGlyphNode->setAtlas(atlas[bucket].texture());
m_whiteGlyphNode->updateGeometry(whiteQuads);

m_blackGlyphNode->setAtlas(atlas[bucket].texture());
m_blackGlyphNode->updateGeometry(blackQuads);
```

---

## 5) Data alignment rules (critical)
- Labels are **data‑relative**, tied to:
  - ring cursor
  - timeOffset
  - srcRect
- No async rebuilding. No stale frames.  
Everything is drawn **in the same frame**.

---

## 6) Performance Notes
- Pre‑allocate **32,000 quads** per glyph node at init (~3 MB each).
- Two geometry nodes total (white + black). Two draw calls.
- Update only vertex data; no texture rebuilds after init.
- Skip label generation entirely when `cellHeight < 12px`.

---

## ✅ Done Criteria
- Text moves *perfectly* with heatmap under pan/zoom/auto.
- No flicker, no swap lag.
- Labels appear/disappear cleanly at zoom threshold (12px).
- Labels scale smoothly within each font bucket.
- White/black contrast is correct against cell intensity.
- `updatePaintNode()` becomes simpler: render only.

---

## Implementation Notes (Codex)
- Avoided full `LabelSnapshot` copies per frame by streaming per-column label updates into a render-thread ring buffer.
- Label geometry is built from that render-thread ring each frame only when `cellHeight >= 12px`.
