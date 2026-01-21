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
- Build **one atlas per font bucket** (e.g. 10/14/18/22 px).
- Store glyphs in a map.

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
- Each vertex: position + texcoord.

**API Sketch:**
```cpp
class HeatmapGlyphNode : public QSGGeometryNode {
public:
  void setAtlas(QSGTexture* texture);
  void ensureCapacity(int maxQuads);
  void updateGeometry(const std::vector<GlyphQuad>& quads);
};
```

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
if (!glyphNode) create once;
glyphNode->setAtlas(atlasTex);
glyphNode->ensureCapacity(maxChars);
glyphNode->updateGeometry(quads);
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
- Pre‑allocate enough quads for worst‑case viewport.
- Use one geometry node; avoid thousands of children.
- Update only vertex data; no texture rebuilds after init.

---

## ✅ Done Criteria
- Text moves *perfectly* with heatmap under pan/zoom/auto.
- No flicker, no swap lag.
- `updatePaintNode()` becomes simpler: render only.