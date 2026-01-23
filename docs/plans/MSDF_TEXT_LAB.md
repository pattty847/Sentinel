**What exists:**
- GPU text path scaffolding:
  - Bitmap glyph atlas (`libs/gui/render/GlyphAtlas.hpp/.cpp`)
  - GPU glyph renderer (`libs/gui/render/HeatmapGlyphNode.hpp/.cpp`, `render/shaders/heatmap_glyph.*`)
  - Label quad builder (`libs/gui/render/HeatmapLabelRenderer.hpp/.cpp`)
- Lab dock + QML test surface:
  - `libs/gui/widgets/LabDock.*`
  - `libs/gui/qml/LabView.qml`
- QSG-based rendering pattern already used in `UnifiedGridRenderer::updatePaintNode`.

**What’s needed:**
- MSDF generator integration:
  - Use `msdfgen` (C++ library) to build MSDF atlas from a font.
  - Decide integration: vendor in repo (recommended) or vcpkg/submodule.
- New MSDF atlas builder:
  - `libs/gui/render/MsdfAtlas.hpp/.cpp` (parallel to `GlyphAtlas` but for MSDF).
  - Outputs a texture image (RGB) + per-glyph metrics (advance, bounds, UV).
- MSDF shader + material:
  - New shader pair: `render/shaders/heatmap_msdf.vert/.frag`.
  - Fragment shader uses MSDF signed distance to produce sharp edges at any scale.
- Lab renderer:
  - New `LabTextItem` (QQuickItem) that draws sample text using MSDF atlas.
  - Hooked into `LabView.qml` to toggle MSDF vs bitmap and show scale tests.
- Optional: reuse current glyph quad generator for layout positioning.

**How to connect:**
1) **Atlas generation**
   - Add `MsdfAtlas` that wraps msdfgen font loading and atlas generation.
   - Use the same charset as current bitmap atlas (`0123456789.kMB$+-`), then expand for TPO alphabet later.
   - Store per-glyph metrics in the same shape as `GlyphAtlas` to reuse quad logic.
2) **MSDF rendering node**
   - Create `MsdfGlyphNode` or extend `HeatmapGlyphNode` to allow selecting shader/material.
   - The geometry remains identical (quads with UVs); only texture + shader differ.
3) **Lab integration**
   - Add `LabTextItem` QQuickItem that owns:
     - MSDF atlas
     - MSDF glyph node
     - Sample strings + scale
   - Expose properties in QML (`mode`, `scale`, `sampleText`).
4) **Toggle in Lab**
   - Update `LabView.qml` with UI to switch between Bitmap/MSDF modes and control scale.
   - Use the same sample grid to compare quality.
5) **(Later) Heatmap integration**
   - Swap label atlas from bitmap to MSDF.
   - Reuse existing label quad generation (positions, data-relative anchors).

**Tradeoffs:**
- **Pros:** crisp text at any zoom; fewer size buckets; GPU-native.
- **Cons:** more complex build (msdfgen), heavier shader, more atlas build time.
- **Memory:** MSDF uses RGB textures (larger than alpha-only).

**Areas to reuse from current atlas pipeline:**
- `HeatmapLabelRenderer::buildLabelQuads` for positioning.
- `HeatmapGlyphNode` geometry + batching (two batches for white/black).
- Lab dock/QML infrastructure for quick A/B testing.

**Open questions / decisions:**
- msdfgen integration path: vendor vs vcpkg.
- Target atlas size: start with 96px base glyphs; expand if needed.
- Shader tuning: edge softness, pixel range, and gamma correction.

**Minimal viable implementation (MVP):**
- msdfgen atlas for digits + “.kMB$+-”
- Lab dock toggles between bitmap and MSDF
- Single sample string + scale slider
- No heatmap integration yet
