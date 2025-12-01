# Heatmap Texture Optimization Plan (Refined with Codebase Context)

## 0. Mission Recap (what we're doing)

**Goal:**
Replace "one quad per cell (2 triangles)" with **"one quad total + data texture + shader"** so:

* Geometry is **O(1)** instead of O(cells)
* Panning/zooming use UV math instead of geometry rebuilds
* DLOD is just "change sampling" instead of "rebuild a million vertices"

We'll have:

* A **data texture** (R16F / R32F) storing liquidity intensities (bid/ask)
* A **single quad** filling the viewport
* A **fragment shader** reading the texture and mapping intensity → color
* A **ring buffer** mapping time slices → texture columns

Opus's job: fit this cleanly into your existing Sentinel codebase (Qt/QSG, UGR, LTSE, viewport transforms).

---

## 1. Key Design Decisions — CODEBASE CONTEXT ANSWERS

### 1.1 Texture resolution & layout

**CONTEXT FROM CODEBASE:**
- Current cell structure (`GridTypes.hpp:8-17`):
```cpp
struct CellInstance {
    int64_t timeStart_ms = 0;
    int64_t timeEnd_ms = 0;
    double priceMin = 0.0;
    double priceMax = 0.0;
    float liquidity = 0.0f;
    bool isBid = true;
};
```
- Price resolution is configurable via `DataProcessor::m_priceResolution` (default: $1.0)
- Time resolution = timeframe (100ms base, up to 10000ms)
- LTSE uses tick-based O(1) storage: `Tick = int32_t` (price / tickSize)

**DESIGN DECISION:**
```
textureWidth  = 2048  (max time columns = ~200 seconds at 100ms, or 34 min at 1000ms)
textureHeight = 1024  (price bins = 1024 price levels at $1 resolution = $1024 range)

Mapping:
  x = (timestamp_ms - textureTimeStart) / timeframe_ms  % textureWidth  (ring buffer)
  y = (price - texturePriceMin) / priceResolution  (clamped to [0, textureHeight))
```

**Price bin definition:** 1 row per tick (1 row = 1 × priceResolution, default $1). For zoomed-out views, aggregate 2-4 ticks per row.

---

### 1.2 Format choice

**CONTEXT FROM CODEBASE:**
- Current color calculation (`HeatmapStrategy.cpp:23-37`):
```cpp
void HeatmapStrategy::calculateColorInline(double liquidity, bool isBid, double intensity,
                                           int& r, int& g, int& b, int& a) const {
    double alpha = std::min(intensity, 1.0);
    a = std::clamp(static_cast<int>(alpha * 255.0), 0, 255);
    if (isBid) {
        g = std::clamp(static_cast<int>(255.0 * intensity), 0, 255);
        r = 0; b = 0;
    } else {
        r = std::clamp(static_cast<int>(255.0 * intensity), 0, 255);
        g = 0; b = 0;
    }
}
```
- Intensity = `liquidity * intensityScale` (calculated in strategy)
- Bid/Ask are separate in LTSE: `slice.bidMetrics[]` and `slice.askMetrics[]`

**DESIGN DECISION:**
**Two separate R16F textures** (cleaner, avoids packing complexity):
```cpp
GLuint m_bidTexture;   // R16F - bid liquidity intensity [0.0, 1.0+]
GLuint m_askTexture;   // R16F - ask liquidity intensity [0.0, 1.0+]
```
- R16F provides 16-bit float precision (sufficient for normalized intensity)
- Separate textures allow independent bid/ask blending in shader
- Total memory: 2 × 2048 × 1024 × 2 bytes = **8 MB** (trivial)

---

### 1.3 Ring buffer scheme

**CONTEXT FROM CODEBASE:**
- LTSE already uses time slices with `startTime_ms` / `endTime_ms`
- Current visible slices query: `LTSE::getVisibleSlices(timeframe, viewStart, viewEnd)`
- Timeframe selection: `LTSE::suggestTimeframe()` based on viewport size

**DESIGN DECISION:**
```cpp
struct HeatmapTextureRingBuffer {
    int64_t baseTimestamp_ms;     // Time corresponding to column 0
    int64_t timeframe_ms;         // Current timeframe (100, 250, 500, etc.)
    int     writeColumn;          // Next column to write (ring buffer head)
    int     validColumnCount;     // How many columns contain valid data
    
    int timeToColumn(int64_t timestamp_ms) const {
        int64_t slotIndex = (timestamp_ms - baseTimestamp_ms) / timeframe_ms;
        return static_cast<int>(slotIndex % textureWidth);
    }
    
    void advanceToTime(int64_t newTimestamp_ms) {
        // Invalidate old columns, update writeColumn
    }
};
```

---

### 1.4 Integration point

**CONTEXT FROM CODEBASE:**
- Current render path:
```
UnifiedGridRenderer::updatePaintNode()  [UGR.cpp:550-682]
    └── GridSceneNode::updateLayeredContent()  [GridSceneNode.cpp:24-84]
        └── HeatmapStrategy::buildNode() / updateNode()  [HeatmapStrategy.cpp:40-266]
            └── Creates QSGGeometryNode per chunk (6 verts/cell)
```
- `GridSceneNode` holds `m_heatmapNode` (QSGNode*)
- `IDataAccessor` interface provides `getVisibleCells()`, `getViewport()`, etc.

**DESIGN DECISION:**
**QSGRenderNode** approach (full GL control):
```cpp
class HeatmapTextureNode : public QSGRenderNode {
    // Full OpenGL control for texture management and custom shaders
    void render(const RenderState* state) override;
    void releaseResources() override;
    
private:
    GLuint m_bidTexture, m_askTexture;
    GLuint m_shaderProgram;
    GLuint m_quadVAO, m_quadVBO;
    HeatmapTextureRingBuffer m_ringBuffer;
};
```

**Integration hook:**
- Replace `m_heatmapNode` in `GridSceneNode` with `HeatmapTextureNode*`
- `HeatmapTextureNode` implements both `IRenderStrategy` interface AND `QSGRenderNode`
- Feature flag to switch between old (GeometryCells) and new (Texture2D) backends

---

### 1.5 Zoom/Pan mapping

**CONTEXT FROM CODEBASE:**
- Viewport struct (`CoordinateSystem.h:18-25`):
```cpp
struct Viewport {
    int64_t timeStart_ms = 0;
    int64_t timeEnd_ms = 0;
    double priceMin = 0.0;
    double priceMax = 0.0;
    double width = 800.0;
    double height = 600.0;
};
```
- `GridViewState` manages viewport via `setViewport()` / `getVisible*()` methods
- Viewport changes trigger `viewportChanged()` signal → `DataProcessor::updateVisibleCells()`

**DESIGN DECISION:**
UV rect calculation from viewport:
```cpp
// In HeatmapTextureNode::render()
void calculateUVRect(const Viewport& vp, const HeatmapTextureRingBuffer& ring,
                     float& uMin, float& uMax, float& vMin, float& vMax) {
    // Time → U (horizontal)
    int colStart = ring.timeToColumn(vp.timeStart_ms);
    int colEnd   = ring.timeToColumn(vp.timeEnd_ms);
    uMin = static_cast<float>(colStart) / textureWidth;
    uMax = static_cast<float>(colEnd)   / textureWidth;
    
    // Handle ring buffer wrap-around in shader
    
    // Price → V (vertical, flip Y)
    vMin = (vp.priceMin - m_texturePriceMin) / m_texturePriceRange;
    vMax = (vp.priceMax - m_texturePriceMin) / m_texturePriceRange;
}
```

Shader handles:
- **Pan in time:** shifts uMin/uMax
- **Zoom in time:** narrows uMin/uMax range
- **Pan in price:** shifts vMin/vMax
- **Zoom in price:** narrows vMin/vMax range

---

### 1.6 DLOD logic

**CONTEXT FROM CODEBASE:**
- LTSE already has DLOD: `suggestTimeframe()` (`LiquidityTimeSeriesEngine.h:187`):
```cpp
int64_t suggestTimeframe(int64_t viewStart_ms, int64_t viewEnd_ms, int maxSlices = 4000) const;
```
- Available timeframes: `{100, 250, 500, 1000, 2000, 5000, 10000}` ms
- Manual override via `DataProcessor::setTimeframe()` (5-second timeout)

**DESIGN DECISION:**
```cpp
// DLOD rules (same as existing logic, but texture-aware)
int64_t HeatmapTextureManager::selectTimeframe(int64_t viewDuration_ms) {
    // Prefer filling ~75% of texture width for good density
    const int targetColumns = textureWidth * 0.75;  // ~1536 columns
    
    for (int64_t tf : {100, 250, 500, 1000, 2000, 5000, 10000}) {
        int64_t columns = viewDuration_ms / tf;
        if (columns <= targetColumns) return tf;
    }
    return 10000;  // Max timeframe for huge views
}
```

Price LOD (optional):
- Zoomed out > 2x: aggregate 2 ticks per row
- Zoomed out > 4x: aggregate 4 ticks per row
- Implemented as shader parameter (`u_ticksPerPixel`)

---

### 1.7 Performance safeguards

**CONTEXT FROM CODEBASE:**
- Current limits: `m_maxCells = 200000` (UGR property)
- Current chunk limit: `kMaxVerticesPerNode = 60000` (Windows/ANGLE 16-bit index limit)
- LTSE history limit: `m_maxHistorySlices = 5000` per timeframe

**DESIGN DECISION:**
```cpp
// Texture-based limits
static constexpr int MAX_TEXTURE_WIDTH  = 4096;  // 4K columns max
static constexpr int MAX_TEXTURE_HEIGHT = 2048;  // 2K price rows max
static constexpr int MAX_COLUMNS_PER_FRAME = 5;  // Limit texture updates per frame

// Ring buffer eviction
if (validColumnCount > textureWidth * 0.9) {
    // Shift baseTimestamp forward, invalidate oldest 25%
    evictOldestColumns(textureWidth / 4);
}
```

---

## 2. Phase Plan (step-by-step)

### **Phase 0 – Recon & Map** ✅ COMPLETE

**Architecture Summary:**

| Component | Location | Role |
|-----------|----------|------|
| UGR | `libs/gui/UnifiedGridRenderer.cpp` | Orchestrates render via `updatePaintNode()` |
| GridSceneNode | `libs/gui/render/GridSceneNode.cpp` | Holds heatmap/bubble/flow nodes |
| HeatmapStrategy | `libs/gui/render/strategies/HeatmapStrategy.cpp` | Builds 6-vert geometry per cell |
| GridViewState | `libs/gui/render/GridViewState.hpp` | Viewport management, pan/zoom |
| Viewport | `libs/gui/CoordinateSystem.h:18-25` | Time/price bounds + dimensions |
| DataProcessor | `libs/gui/render/DataProcessor.cpp` | Converts LTSE slices → CellInstance |
| LTSE | `libs/core/LiquidityTimeSeriesEngine.cpp` | Order book → time slices |
| CellInstance | `libs/gui/render/GridTypes.hpp:8-17` | World-space cell data |

**Current hot path (per frame):**
```
updatePaintNode() → updateVisibleCells() → getPublishedCellsSnapshot()
    → HeatmapStrategy::buildNode() → iterate ALL cells → create vertices
    → QSGGeometryNode per chunk → GPU draw
```

**New hot path (per frame):**
```
updatePaintNode() → HeatmapTextureNode::render()
    → updateDirtyColumns() (only new slices, ~1-3 columns)
    → glTexSubImage2D() × 2 (bid + ask)
    → glDrawArrays(GL_TRIANGLE_STRIP, 4) (single quad)
    → fragment shader samples textures, computes color
```

---

### **Phase 1 – Data Texture Layout & Types**

Create new header: `libs/gui/render/HeatmapTextureTypes.hpp`

```cpp
#pragma once
#include <cstdint>
#include <vector>

// Configuration for the texture-based heatmap
struct HeatmapTextureConfig {
    int textureWidth  = 2048;   // Max time columns
    int textureHeight = 1024;   // Price bins
    float priceMin    = 0.0f;   // Bottom of texture
    float priceMax    = 0.0f;   // Top of texture
    float priceResolution = 1.0f;  // $ per row
    int64_t timeframe_ms = 100; // Current timeframe
};

// Ring buffer state for time axis
struct HeatmapRingBuffer {
    int64_t baseTimestamp_ms = 0;  // Time at column 0
    int64_t timeframe_ms = 100;    // ms per column
    int writeColumn = 0;           // Next column to write
    int validColumns = 0;          // Columns with valid data
    
    int timeToColumn(int64_t timestamp_ms) const;
    int64_t columnToTime(int column) const;
    void advance(int64_t newTimestamp_ms, int textureWidth);
};

// CPU-side column data (extracted from LiquidityTimeSlice)
struct HeatmapColumnData {
    int64_t timestamp_ms;
    std::vector<float> bidIntensities;  // size = textureHeight
    std::vector<float> askIntensities;  // size = textureHeight
};
```

**Price binning functions:**
```cpp
// In HeatmapTextureManager
int priceToRow(double price) const {
    int row = static_cast<int>((price - m_config.priceMin) / m_config.priceResolution);
    return std::clamp(row, 0, m_config.textureHeight - 1);
}

double rowToPrice(int row) const {
    return m_config.priceMin + row * m_config.priceResolution;
}
```

---

### **Phase 2 – GPU Resource Management**

Create: `libs/gui/render/HeatmapTextureManager.hpp/.cpp`

```cpp
class HeatmapTextureManager {
public:
    void initialize(QOpenGLFunctions* gl, const HeatmapTextureConfig& config);
    void shutdown();
    
    // Update single column from LiquidityTimeSlice
    void updateColumn(int colIndex, const HeatmapColumnData& data);
    
    // Bind textures for rendering
    void bindTextures(int bidUnit, int askUnit);
    
    // Ring buffer access
    const HeatmapRingBuffer& getRingBuffer() const { return m_ring; }
    void advanceTime(int64_t newTimestamp_ms);
    
private:
    QOpenGLFunctions* m_gl = nullptr;
    GLuint m_bidTexture = 0;
    GLuint m_askTexture = 0;
    HeatmapTextureConfig m_config;
    HeatmapRingBuffer m_ring;
    
    void createTextures();
    void uploadColumn(GLuint texture, int column, const std::vector<float>& data);
};
```

**Texture creation:**
```cpp
void HeatmapTextureManager::createTextures() {
    auto createTex = [&](GLuint& tex) {
        m_gl->glGenTextures(1, &tex);
        m_gl->glBindTexture(GL_TEXTURE_2D, tex);
        m_gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_R16F, 
                          m_config.textureWidth, m_config.textureHeight,
                          0, GL_RED, GL_FLOAT, nullptr);
        m_gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        m_gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        m_gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        m_gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    };
    createTex(m_bidTexture);
    createTex(m_askTexture);
}

void HeatmapTextureManager::uploadColumn(GLuint texture, int column, 
                                         const std::vector<float>& data) {
    m_gl->glBindTexture(GL_TEXTURE_2D, texture);
    m_gl->glTexSubImage2D(GL_TEXTURE_2D, 0,
                         column, 0,              // x, y offset
                         1, m_config.textureHeight,  // width=1 column, full height
                         GL_RED, GL_FLOAT, data.data());
}
```

---

### **Phase 3 – Shader & Material Integration**

**Vertex shader:** `heatmap_texture.vert`
```glsl
#version 330 core
layout(location = 0) in vec2 a_position;  // Quad corners [-1,1]
layout(location = 1) in vec2 a_texCoord;  // UV [0,1]

out vec2 v_texCoord;

uniform vec4 u_uvRect;  // (uMin, vMin, uMax, vMax)

void main() {
    gl_Position = vec4(a_position, 0.0, 1.0);
    
    // Map quad UV to viewport UV rect
    v_texCoord = mix(u_uvRect.xy, u_uvRect.zw, a_texCoord);
}
```

**Fragment shader:** `heatmap_texture.frag`
```glsl
#version 330 core
in vec2 v_texCoord;
out vec4 fragColor;

uniform sampler2D u_bidTexture;
uniform sampler2D u_askTexture;
uniform float u_intensityScale;
uniform float u_maxIntensity;

// Ring buffer handling
uniform float u_ringWrapU;  // Column where ring buffer wraps (0-1)
uniform float u_ringValidStartU;  // First valid column (0-1)

void main() {
    // Handle ring buffer wrap in U coordinate
    float u = v_texCoord.x;
    // (Ring buffer logic here if needed)
    
    vec2 uv = vec2(u, v_texCoord.y);
    
    float bid = texture(u_bidTexture, uv).r;
    float ask = texture(u_askTexture, uv).r;
    
    // Apply intensity scaling
    float bidIntensity = clamp(bid * u_intensityScale, 0.0, 1.0);
    float askIntensity = clamp(ask * u_intensityScale, 0.0, 1.0);
    
    // Color mapping (green=bid, red=ask)
    vec3 color = vec3(askIntensity, bidIntensity, 0.0);
    float alpha = max(bidIntensity, askIntensity);
    
    // Discard fully transparent pixels
    if (alpha < 0.01) discard;
    
    fragColor = vec4(color, alpha);
}
```

**Qt Integration via QSGRenderNode:**
```cpp
class HeatmapTextureNode : public QSGRenderNode {
public:
    RenderingFlags flags() const override {
        return BoundedRectRendering | DepthAwareRendering;
    }
    
    QRectF rect() const override { return m_rect; }
    
    void render(const RenderState* state) override;
    void releaseResources() override;
    
    // Data update interface
    void updateFromSlices(const std::vector<const LiquidityTimeSlice*>& slices,
                         const Viewport& viewport);
    
private:
    HeatmapTextureManager m_texManager;
    GLuint m_shaderProgram = 0;
    GLuint m_quadVAO = 0;
    QRectF m_rect;
    
    // Uniforms
    GLint m_locUvRect, m_locIntensityScale, m_locBidTex, m_locAskTex;
};
```

---

### **Phase 4 – Single Quad Node + UV Transform**

**Quad geometry (static, created once):**
```cpp
void HeatmapTextureNode::initializeQuad() {
    float quadVerts[] = {
        // Position    // TexCoord
        -1.0f, -1.0f,  0.0f, 0.0f,
         1.0f, -1.0f,  1.0f, 0.0f,
        -1.0f,  1.0f,  0.0f, 1.0f,
         1.0f,  1.0f,  1.0f, 1.0f,
    };
    // Create VAO/VBO once, reuse forever
}
```

**UV rect from viewport:**
```cpp
void HeatmapTextureNode::calculateUVRect(const Viewport& vp, 
                                         float& uMin, float& uMax,
                                         float& vMin, float& vMax) {
    const auto& ring = m_texManager.getRingBuffer();
    const auto& cfg = m_texManager.getConfig();
    
    // Time → U
    int colStart = ring.timeToColumn(vp.timeStart_ms);
    int colEnd   = ring.timeToColumn(vp.timeEnd_ms);
    uMin = static_cast<float>(colStart) / cfg.textureWidth;
    uMax = static_cast<float>(colEnd)   / cfg.textureWidth;
    
    // Price → V (flip Y: high price = top = V=1)
    float priceRange = cfg.priceMax - cfg.priceMin;
    vMin = (vp.priceMin - cfg.priceMin) / priceRange;
    vMax = (vp.priceMax - cfg.priceMin) / priceRange;
    vMin = 1.0f - vMin;  // Flip Y
    vMax = 1.0f - vMax;
    std::swap(vMin, vMax);  // After flip, vMin > vMax, so swap
}
```

---

### **Phase 5 – LTSE + DLOD Integration**

**Connect to existing LTSE output:**
```cpp
// In DataProcessor or HeatmapTextureNode
void HeatmapTextureNode::updateFromSlices(
    const std::vector<const LiquidityTimeSlice*>& slices,
    const Viewport& viewport) 
{
    // Determine which columns need updating
    for (const auto* slice : slices) {
        int col = m_ring.timeToColumn(slice->startTime_ms);
        
        // Check if this column is already up-to-date
        if (m_columnVersions[col] >= slice->dataVersion) continue;
        
        // Extract intensity data from slice
        HeatmapColumnData colData;
        colData.timestamp_ms = slice->startTime_ms;
        extractSliceToColumn(*slice, viewport, colData);
        
        // Upload to GPU
        m_texManager.updateColumn(col, colData);
        m_columnVersions[col] = slice->dataVersion;
        m_dirtyColumnsThisFrame++;
    }
}

void extractSliceToColumn(const LiquidityTimeSlice& slice,
                          const Viewport& vp,
                          HeatmapColumnData& out) {
    out.bidIntensities.resize(m_config.textureHeight, 0.0f);
    out.askIntensities.resize(m_config.textureHeight, 0.0f);
    
    for (Tick tick = slice.minTick; tick <= slice.maxTick; ++tick) {
        double price = slice.tickToPrice(tick);
        int row = priceToRow(price);
        if (row < 0 || row >= m_config.textureHeight) continue;
        
        size_t idx = static_cast<size_t>(tick - slice.minTick);
        if (idx < slice.bidMetrics.size()) {
            out.bidIntensities[row] = slice.bidMetrics[idx].avgLiquidity;
        }
        if (idx < slice.askMetrics.size()) {
            out.askIntensities[row] = slice.askMetrics[idx].avgLiquidity;
        }
    }
}
```

**DLOD timeframe selection (reuse existing logic):**
```cpp
int64_t HeatmapTextureNode::selectTimeframe(const Viewport& vp) {
    // Delegate to LTSE's existing suggestTimeframe
    return m_ltse->suggestTimeframe(vp.timeStart_ms, vp.timeEnd_ms, 
                                    m_config.textureWidth * 0.75);
}
```

---

### **Phase 6 – Kill Old Geometry Path + Profiling**

**Feature flag:**
```cpp
// In UnifiedGridRenderer.h
enum class HeatmapBackend {
    GeometryCells,  // Current: 6 verts/cell
    Texture2D       // New: single quad + data textures
};

Q_PROPERTY(int heatmapBackend READ heatmapBackend WRITE setHeatmapBackend)
```

**Profiling hooks:**
```cpp
// In HeatmapTextureNode::render()
QElapsedTimer timer;
timer.start();

int columnsUpdated = updateDirtyColumns();
qint64 uploadUs = timer.nsecsElapsed() / 1000;

timer.restart();
drawQuad();
qint64 drawUs = timer.nsecsElapsed() / 1000;

sLog_RenderN(10, "HEATMAP TEXTURE: columns=" << columnsUpdated
             << " upload=" << uploadUs << "us"
             << " draw=" << drawUs << "us");
```

**Expected metrics comparison:**
| Metric | Old (GeometryCells) | New (Texture2D) |
|--------|---------------------|-----------------|
| Vertices | 1,560,000 | **6** |
| Draw calls | ~260 | **1** |
| CPU/frame (full) | ~60ms | **<1ms** |
| CPU/frame (append) | ~5ms | **<0.1ms** |
| GPU | ~5ms | **<0.5ms** |

---

## 3. Constraints for Implementation

* **DO NOT** thread texture updates yet. Single-thread first.
* Keep **types, class names, namespace style** consistent with existing code.
* New files go in `libs/gui/render/` (textures) and `libs/gui/render/strategies/` (node).
* Break into **smaller PRs**:
  - PR1: `HeatmapTextureTypes.hpp` + config structs
  - PR2: `HeatmapTextureManager` + texture creation/upload
  - PR3: `HeatmapTextureNode` + shader + single quad
  - PR4: LTSE integration + column extraction
  - PR5: DLOD integration + feature flag

**Refactoring allowed:**
- Viewport logic in `GridViewState`
- Slice indexing in `DataProcessor`
- Price binning utilities

**DO NOT touch:**
- Core layer (`libs/core/*`)
- Other render strategies (bubbles, flow, candles)
- MainWindowGPU or dock widgets

---

## 4. Mental Model Recap

* **Grid of intensities** → R16F data textures (bid + ask)
* **View window** → UV rectangle on textures
* **Pan/Zoom** → change UV; geometry stays O(1)
* **DLOD** → coarser timeframe = fewer columns to update
* **Performance win** → 1 quad, 1 shader, ~3 column uploads/frame vs 1.5M vertices

---

## 5. File Locations Summary

| New File | Purpose |
|----------|---------|
| `libs/gui/render/HeatmapTextureTypes.hpp` | Config, ring buffer, column data structs |
| `libs/gui/render/HeatmapTextureManager.hpp/cpp` | GPU texture management |
| `libs/gui/render/strategies/HeatmapTextureNode.hpp/cpp` | QSGRenderNode + shader |
| `libs/gui/render/shaders/heatmap_texture.vert` | Vertex shader |
| `libs/gui/render/shaders/heatmap_texture.frag` | Fragment shader |

---

**Ready for implementation.** Feed this to Opus and say: "Start with Phase 1 – create the type definitions and config struct."
