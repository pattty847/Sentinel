# TPO & Footprint Charts - GPU-Accelerated Implementation Roadmap

## Overview

Extend the existing GPU heatmap architecture to support TPO (Time Price Opportunity) and Footprint chart rendering modes. Use the same single-quad + texture sampling pipeline for maximum performance.

**All new overlays consume `TimeAxisMapping`** — the unified coordinate system that maps world time/price to screen pixels. This was introduced in the candlestick refactor (Feb 2026) and guarantees all chart layers share the same plane with zero drift.

---

## What Exists

### GPU Rendering Pipeline (`libs/gui/render/`)
- **HeatmapIntensityNode.cpp/hpp** — Single quad geometry, column-wise `glTexSubImage2D` uploads
- **HeatmapStreamState.cpp/hpp** — Ring buffer management, pending upload queue, time alignment
- **TimeAxisMapping.hpp** — **Single source of truth** for world→screen mapping. Produced once per frame in `updatePaintNode()`, consumed by all renderers. Provides `timeToScreenX()`, `priceToScreenY()`, `bucketStartMsForTime()`, `visibleDataStartMs/EndMs()`.
- **CandlestickOverlayItem.hpp/cpp** — GPU-batched candle overlay, fully aligned to heatmap via `TimeAxisMapping`. Proves the pattern for adding new overlay types.
- **HeatmapLabelRenderer.hpp/cpp** — MSDF glyph quad builder. Signature: `buildLabelQuads(const TimeAxisMapping&, snapshot, atlas, rings, scale, dollars, quads)`.
- **shaders/heatmap_intensity.frag** — Fragment shader with palette lookup, bid/ask encoding (0-127/128-255)
- **shaders/heatmap_intensity.vert** — Simple vertex pass-through with uniform buffer

### Data Pipeline (`libs/core/`)
- **LiveOrderBook** — Dense O(1) order book with `accumulateRangeSplit()` for bid/ask aggregation
- **HeatmapTwapStreamer** — TWAP sampling (50ms), intensity/liquidity encoding
- **TradeData.h** — Trade struct with timestamp, price, size, side
- **TimeframeAggregator** — OHLCV bar aggregation
- **CandleSeriesBuffer** — Ring buffer of OHLCV candles, queryable by symbol/timeframe/time range

### Rendering Infrastructure (`libs/gui/`)
- **UnifiedGridRenderer** — QML adapter, viewport management, `updatePaintNode()` orchestration. Owns `m_lastTimeAxisMapping` and exposes `lastTimeAxisMapping()` for all overlay consumers.
- **GridViewState** — Pan/zoom state, viewport versioning. Pan is baked into `TimeAxisMapping.viewStartMs/EndMs` during drag.
- **CoordinateSystem** — QML-facing `worldToScreen/screenToWorld` (simple viewport math). Coexists with `TimeAxisMapping` (render-thread full mapping).

### Reusable Patterns
- Ring buffer with modulo wrapping for continuous streaming
- Column-wise texture updates (1px wide, full height)
- Thread-safe snapshot pattern (multiple mutexes)
- Signed byte encoding for bid/ask discrimination
- **TimeAxisMapping consumption pattern**: overlay calls `m_heatmapRenderer->lastTimeAxisMapping()`, uses `timeToScreenX()`/`priceToScreenY()` for all positioning. No manual column math, no `timeOffset` in screen mapping.

### Coordinate Invariants (from TimeAxisMapping refactor)
1. **1 slice = 1 candle = 1 footprint column.** Same `appendMs`, same epoch-aligned boundaries.
2. **`timeOffset` is shader-only.** Never used in overlay/label screen positioning. Only for ring buffer data lookup.
3. **Pan is baked into view bounds.** `viewStartMs/EndMs` already include drag offset — overlays don't need separate pan handling.
4. **Bucket alignment:** `bucketStartMsForTime(t) = (t / appendMs) * appendMs`. Epoch-clocked, server-side.

---

## What's Needed

### Phase 1: Core Texture Format Extensions

#### 1.1 Multi-Channel Texture Support
**File:** `libs/gui/render/ProfileStreamState.hpp` (NEW)

```cpp
// Extended ring buffer for TPO/Footprint data
struct ProfileStreamState {
    // Channel 1: Volume at price (like current intensity)
    std::vector<uint8_t> m_volumeRing;      // [gridSize x gridSize]

    // Channel 2: Trade count at price (for TPO letters)
    std::vector<uint8_t> m_tradeCountRing;  // [gridSize x gridSize]

    // Channel 3: Delta (buys - sells) for footprint
    std::vector<int8_t> m_deltaRing;        // [-127..+127] signed

    // Channel 4: Time bucket ID for TPO letter assignment
    std::vector<uint8_t> m_timeBucketRing;  // [gridSize x gridSize]

    // Per-column aggregates
    std::vector<AggregateColumn> m_aggregates;

    struct AggregateColumn {
        double totalVolume;
        double buyVolume;
        double sellVolume;
        int tradeCount;
        double vwap;
        uint8_t timeBucket;  // A-Z mapping for TPO
    };
};
```

#### 1.2 RGBA Texture Encoding Scheme
**File:** `libs/gui/render/ProfileTextureEncoder.hpp` (NEW)

```cpp
// Pack multiple data channels into RGBA texture
// R = Volume intensity (0-255)
// G = Delta direction + magnitude (0-127 sell, 128-255 buy)
// B = Time bucket ID (0-25 = A-Z for TPO)
// A = Trade count (for opacity/importance)

struct ProfileTextureEncoder {
    static QByteArray encodeFootprintColumn(
        const std::vector<double>& bidVolumes,
        const std::vector<double>& askVolumes,
        const std::vector<int>& tradeCounts,
        double maxVolume);

    static QByteArray encodeTPOColumn(
        const std::vector<bool>& priceTouched,
        uint8_t timeBucket,  // Current TPO letter (0-25)
        int periodIndex);

    static QByteArray encodeVolumeProfile(
        const std::vector<double>& volumeAtPrice,
        double pocPrice,  // Point of Control
        double valueAreaHigh,
        double valueAreaLow);
};
```

### Phase 2: New Shader System

#### 2.1 Footprint Fragment Shader
**File:** `libs/gui/render/shaders/footprint.frag` (NEW)

```glsl
#version 440

layout(location = 0) in vec2 v_texcoord;
layout(location = 0) out vec4 fragColor;

layout(binding = 1) uniform sampler2D dataTex;      // RGBA encoded data
layout(binding = 2) uniform sampler2D paletteTex;   // Color palette

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    vec4 params;     // x=opacity, y=gamma, z=cellWidth, w=timeOffset
    vec4 params2;    // x=cellHeight, y=mode, z=deltaScale, w=reserved
};

void main() {
    vec2 uv = vec2(fract(v_texcoord.x + params.w), v_texcoord.y);
    vec4 data = texture(dataTex, uv);

    float volume = data.r;
    float delta = data.g;
    float timeBucket = data.b;
    float tradeCount = data.a;

    if (volume < 0.001 && tradeCount < 0.001) {
        discard;
    }

    int mode = int(params2.y);

    if (mode == 0) {  // FOOTPRINT_DELTA
        float deltaSign = step(0.5, delta);
        float deltaMag = abs(delta - 0.5) * 2.0;

        vec3 buyColor = vec3(0.0, 0.4, 0.2);
        vec3 sellColor = vec3(0.4, 0.0, 0.1);
        vec3 cellColor = mix(sellColor, buyColor, deltaSign);

        float intensity = pow(volume, params.y) * params.x;
        fragColor = vec4(cellColor * (0.3 + intensity * 0.7), intensity);

    } else if (mode == 1) {  // FOOTPRINT_VOLUME
        float u = volume * 0.5;
        vec4 color = texture(paletteTex, vec2(u, 0.5));
        fragColor = vec4(color.rgb, color.a * params.x);

    } else if (mode == 2) {  // TPO_LETTERS
        float hue = timeBucket / 26.0;
        vec3 tpoColor = hsv2rgb(vec3(hue, 0.7, 0.9));
        float alpha = step(0.001, volume) * params.x;
        fragColor = vec4(tpoColor, alpha);

    } else if (mode == 3) {  // VOLUME_PROFILE
        float u = volume;
        vec4 color = texture(paletteTex, vec2(u, 0.5));
        fragColor = vec4(color.rgb, color.a * params.x);
    }
}

vec3 hsv2rgb(vec3 c) {
    vec4 K = vec4(1.0, 2.0/3.0, 1.0/3.0, 3.0);
    vec3 p = abs(fract(c.xxx + K.xyz) * 6.0 - K.www);
    return c.z * mix(K.xxx, clamp(p - K.xxx, 0.0, 1.0), c.y);
}
```

#### 2.2 TPO Profile Shader (Stacked Time Vertically)
**File:** `libs/gui/render/shaders/tpo_profile.frag` (NEW)

```glsl
#version 440

layout(location = 0) in vec2 v_texcoord;
layout(location = 0) out vec4 fragColor;

layout(binding = 1) uniform sampler2D profileTex;  // Accumulated TPO data
layout(binding = 2) uniform sampler2D paletteTex;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    vec4 params;     // x=opacity, y=gamma, z=profileWidth, w=timeOffset
    vec4 params2;    // x=pocIndex, y=vahIndex, z=valIndex, w=sessionCount
};

void main() {
    vec2 uv = v_texcoord;
    vec4 data = texture(profileTex, uv);

    float letterCount = data.r * 255.0;
    float timeBucket = data.g * 255.0;
    float isPOC = data.b;
    float isValueArea = data.a;

    if (letterCount < 0.5) {
        discard;
    }

    float hue = mod(timeBucket, 26.0) / 26.0;
    vec3 baseColor = hsv2rgb(vec3(hue, 0.6, 0.85));

    if (isPOC > 0.5) {
        baseColor = vec3(1.0, 0.9, 0.0);  // Gold for POC
    }

    float brightness = mix(0.5, 1.0, isValueArea);
    fragColor = vec4(baseColor * brightness, params.x);
}

vec3 hsv2rgb(vec3 c) {
    vec4 K = vec4(1.0, 2.0/3.0, 1.0/3.0, 3.0);
    vec3 p = abs(fract(c.xxx + K.xyz) * 6.0 - K.www);
    return c.z * mix(K.xxx, clamp(p - K.xxx, 0.0, 1.0), c.y);
}
```

### Phase 3: Data Aggregation Pipeline

#### 3.1 Trade Aggregator for Footprint
**File:** `libs/core/servermodel/FootprintAggregator.hpp` (NEW)

```cpp
class FootprintAggregator {
public:
    struct Config {
        int gridSize = 8192;
        double minPrice;
        double maxPrice;
        double tickSize;
        int timeframeSec = 60;  // 1-minute candles typical
    };

    struct PriceLevel {
        double bidVolume = 0.0;
        double askVolume = 0.0;
        int bidCount = 0;
        int askCount = 0;

        double delta() const { return askVolume - bidVolume; }
        double totalVolume() const { return bidVolume + askVolume; }
    };

    struct FootprintCandle {
        int64_t startMs;
        int64_t endMs;
        std::vector<PriceLevel> levels;  // Indexed by price
        double open, high, low, close;
        double pocPrice;  // Point of Control
        double vwap;
    };

    void onTrade(const Trade& trade);
    void finalizeCandle(int64_t candleEndMs);

    // Encode current candle for GPU upload
    QByteArray encodeCurrentColumn(double& maxVolume);

signals:
    void candleReady(const FootprintCandle& candle);

private:
    Config m_config;
    FootprintCandle m_currentCandle;
    std::map<int64_t, FootprintCandle> m_history;
};
```

#### 3.2 TPO Profile Builder
**File:** `libs/core/servermodel/TPOProfileBuilder.hpp` (NEW)

```cpp
class TPOProfileBuilder {
public:
    struct Config {
        int gridSize = 8192;
        double minPrice;
        double maxPrice;
        double tickSize;
        int periodMinutes = 30;  // Standard TPO period
        int sessionHours = 8;    // Trading session length
    };

    struct TPOPeriod {
        char letter;  // A-Z
        int64_t startMs;
        int64_t endMs;
        std::set<int> priceLevelsTouched;
    };

    struct TPOProfile {
        std::vector<TPOPeriod> periods;
        std::vector<int> letterCountByPrice;  // How many letters at each price
        int pocIndex;          // Point of Control price index
        int vahIndex;          // Value Area High
        int valIndex;          // Value Area Low
        double valueAreaPct;   // Typically 70%
    };

    void onTrade(const Trade& trade);
    void advancePeriod();

    // Build stacked vertical TPO texture
    QByteArray encodeProfileColumn();

private:
    Config m_config;
    TPOProfile m_currentProfile;
    char m_currentLetter = 'A';
};
```

### Phase 4: Rendering Mode System

#### 4.1 Chart Mode Enum
**File:** `libs/gui/render/ChartRenderMode.hpp` (NEW)

```cpp
enum class ChartRenderMode : int {
    Heatmap = 0,           // Current implementation
    FootprintDelta = 1,    // Bid/Ask delta coloring
    FootprintVolume = 2,   // Volume heatmap in candle blocks
    TPOProfile = 3,        // Letters stacked vertically
    VolumeProfile = 4,     // Horizontal volume bars
    FootprintNumbers = 5,  // Numbers overlaid (hybrid)
};

struct ChartModeConfig {
    ChartRenderMode mode;
    bool showGrid = true;
    bool showLabels = true;
    bool showPOC = true;
    bool showValueArea = true;
    float cellPadding = 1.0f;
    int timeframeSec = 60;  // For footprint candle width
};
```

#### 4.2 Unified Material with Mode Support
**File:** `libs/gui/render/ProfileIntensityNode.hpp` (NEW)

```cpp
class ProfileIntensityMaterial final : public QSGMaterial {
public:
    void setRenderMode(ChartRenderMode mode);
    void setDataTexture(QSGTexture* texture);   // RGBA data
    void setPaletteTexture(QSGTexture* texture);
    void setCellSize(float width, float height);
    void setDeltaScale(float scale);

    void enqueueColumn(int x, QByteArray data);

private:
    ChartRenderMode m_mode = ChartRenderMode::Heatmap;
    QSGTexture* m_dataTexture = nullptr;
    QSGTexture* m_paletteTexture = nullptr;
    float m_cellWidth = 1.0f;
    float m_cellHeight = 1.0f;
    float m_deltaScale = 1.0f;
};

class ProfileIntensityNode final : public QSGGeometryNode {
    // Same quad geometry as HeatmapIntensityNode
    // Different material for multi-mode rendering
};
```

### Phase 5: Label/Overlay System for Footprint Numbers

#### 5.1 Footprint Label Renderer
**File:** `libs/gui/render/FootprintLabelRenderer.hpp` (NEW)

```cpp
// Follows same pattern as HeatmapLabelRenderer — consumes TimeAxisMapping for positioning.
class FootprintLabelRenderer {
public:
    using GlyphQuad = MsdfGlyphNode::GlyphQuad;

    struct CellLabel {
        int gridX, gridY;
        QString bidText;   // e.g., "152"
        QString askText;   // e.g., "89"
        QString deltaText; // e.g., "+63"
    };

    // Uses TimeAxisMapping for all cell positioning (no manual fracX/fracY math).
    // timeToScreenX(bucketTimeMs) for X, priceToScreenY(price) for Y.
    static void buildFootprintQuads(
        const TimeAxisMapping& mapping,
        const FootprintAggregator::FootprintCandle& candle,
        const MsdfAtlas& atlas,
        float scale,
        std::vector<GlyphQuad>& bidQuads,
        std::vector<GlyphQuad>& askQuads,
        std::vector<GlyphQuad>& deltaQuads);
};
```

### Phase 6: Data Flow Integration

#### 6.1 Server-Side Footprint Streamer
**File:** `libs/core/servermodel/FootprintStreamer.hpp` (NEW)

```cpp
class FootprintStreamer : public QObject {
    Q_OBJECT
public:
    void setSymbol(const QString& symbol);
    void setTimeframe(int seconds);
    void attachToHotData(SymbolHotData& hotData);

signals:
    void footprintColumnReady(
        int64_t candleStartMs,
        int timeframeSec,
        const QByteArray& rgbaColumn,  // RGBA encoded
        double maxVolume,
        double pocPrice);

private slots:
    void onSampleTimer();
    void onTradeReceived(const Trade& trade);

private:
    FootprintAggregator m_aggregator;
    QTimer m_sampleTimer;
};
```

#### 6.2 Protocol Extensions
**File:** `libs/core/protocol/SentinelStreamProtocol.hpp` (MODIFY)

```cpp
// Add new message types
enum class MessageType : uint8_t {
    // ... existing ...
    HeatmapSlice = 0x10,

    // NEW
    FootprintSlice = 0x20,
    TPOProfileSlice = 0x21,
    VolumeProfileSlice = 0x22,
};

struct FootprintSliceMessage {
    int64_t candleStartMs;
    int32_t timeframeSec;
    float minPrice;
    float maxPrice;
    float tickSize;
    float pocPrice;
    float maxVolume;
    uint16_t levelCount;
    // Followed by: levelCount x (bidVol:u16, askVol:u16, bidCount:u8, askCount:u8)
};
```

---

## How to Connect

### Integration Sequence

**Step 1: Texture Format Upgrade**
1. Create `ProfileStreamState` extending `HeatmapStreamState` pattern
2. Add RGBA texture creation in `UnifiedGridRenderer::ensureHeatmapImage()`
3. Modify `HeatmapIntensityShader::updateSampledImage()` to handle RGBA

**Step 2: Shader Pipeline**
1. Add new `.frag` files to `libs/gui/render/shaders/`
2. Create `.qsb` compiled shader resources
3. Add `ProfileIntensityShader` class selecting shader by mode
4. Register new materials in `CMakeLists.txt` resource files

**Step 3: Data Aggregation**
1. Add `FootprintAggregator` to server model
2. Wire to `LiveOrderBook` trade callbacks
3. Implement `encodeCurrentColumn()` using RGBA packing
4. Add `FootprintStreamer` parallel to `HeatmapTwapStreamer`

**Step 4: Renderer Mode Support**
1. Add `Q_PROPERTY(int chartMode)` to `UnifiedGridRenderer`
2. Switch between `HeatmapIntensityNode` and `ProfileIntensityNode` in `updatePaintNode()`
3. Route data to appropriate aggregator based on mode
4. **`TimeAxisMapping` is already populated** — new overlays just consume `lastTimeAxisMapping()`, same as candles do now.

**Step 5: QML Integration**
1. Expose `chartMode` property
2. Add mode selector UI in QML
3. Wire timeframe controls to footprint candle width

### Entry Points

| Component | File | Hook |
|-----------|------|------|
| Coordinate mapping | `render/TimeAxisMapping.hpp` | Consumed via `lastTimeAxisMapping()` |
| Mode selection | `UnifiedGridRenderer.h` | `setGridMode(int mode)` |
| Data ingestion | `HeatmapStreamState.cpp` | `ingestSlice()` |
| Shader creation | `HeatmapIntensityNode.cpp` | `createShader()` |
| Trade aggregation | `HeatmapTwapStreamer.cpp` | `onSampleTimer()` |
| Protocol messages | `SentinelStreamProtocol.hpp` | Add message types |
| Candle overlay pattern | `render/CandlestickOverlayItem.cpp` | Reference implementation for TimeAxisMapping consumption |

### Control Flow

```
User selects "Footprint" mode in QML
    |
UnifiedGridRenderer::setGridMode(ChartRenderMode::FootprintDelta)
    |
DataProcessor notified -> switches to FootprintStreamer
    |
Trades arrive -> FootprintAggregator::onTrade()
    |
Candle finalizes -> FootprintStreamer::footprintColumnReady()
    |
ProfileStreamState::ingestSlice() (RGBA column)
    |
updatePaintNode():
  1. Populate TimeAxisMapping (already done for heatmap — same code path)
  2. Create ProfileIntensityNode (shader selects footprint.frag)
  3. FootprintLabelRenderer::buildFootprintQuads(mapping, ...) uses timeToScreenX/priceToScreenY
    |
GPU renders with delta coloring + MSDF text labels
```

---

## Tradeoffs

### Performance
- **RGBA vs R8**: 4x memory (256MB vs 64MB for 8192^2), but enables multi-channel data
- **Column uploads unchanged**: Still 1px wide, just 4x bytes per column
- **Shader branching**: Mode switch in shader adds ~1-2 cycles per fragment, negligible

### Complexity
- **Separate aggregators**: FootprintAggregator vs HeatmapTwapStreamer increases code, but cleaner separation
- **Protocol messages**: New message types require server-side support
- **Label rendering**: Footprint numbers (bid x ask) require hybrid GPU+CPU approach

### Technical Debt
- Shader variants could be unified with uber-shader, but branching acceptable for 5 modes
- Ring buffer duplication between HeatmapStreamState and ProfileStreamState could be refactored to generic template

---

## Hard Parts / Risks

### 1. TPO Letter Stacking (Vertical Time)
- Traditional TPO stacks 30-min periods as letters (A-Z) within each session
- Requires different UV mapping: X=session, Y encodes both price AND time-within-session
- May need 2D array per price level: `letterCount[price][period]`
- **Solution**: Use texture's B channel for letter ID, render letters as colored blocks

### 2. Footprint Cell Alignment
- ~~Footprint shows bid x ask numbers in each candle x price cell~~
- ~~Cell boundaries must align with candle times and price ticks~~
- ~~Screen-space cell size changes with zoom~~
- **SOLVED by TimeAxisMapping.** `timeToScreenX(bucketTime)` and `priceToScreenY(price)` give exact cell boundaries. `cellW` and `cellH` give pixel dimensions. Same pattern as `CandlestickOverlayItem` and `HeatmapLabelRenderer`. No manual column math needed.

### 3. Numbers Overlay Performance
- Rendering 1000s of text labels is expensive
- Current `HeatmapLabelRenderer` shows 1 number per cell (liquidity)
- Footprint needs 2-3 numbers per cell (bid, ask, delta)
- **Solution**:
  - GPU mode: No numbers, just colors (fast)
  - Hybrid mode: Zoom-dependent label density, reuse MSDF atlas + `TimeAxisMapping` for positioning
  - Follow `HeatmapLabelRenderer::buildLabelQuads(mapping, ...)` pattern — already proven at scale

### 4. Real-time Delta Calculation
- Delta requires tracking buys vs sells accurately
- Trade side from exchange may be unreliable (aggressor inference needed)
- **Solution**: Use trade direction heuristic: compare to best bid/ask at time of trade

### 5. Session Boundaries for TPO
- TPO profiles are per-session (typically RTH: 9:30 AM - 4:00 PM)
- Need session configuration: start time, timezone, holidays
- **Solution**: Add `SessionConfig` struct, default to 24/7 crypto mode

---

## File Changes Summary

### New Files
| File | LOC Est. | Purpose |
|------|----------|---------|
| `ProfileStreamState.hpp/cpp` | 400 | Multi-channel ring buffer |
| `ProfileTextureEncoder.hpp/cpp` | 200 | RGBA encoding utilities |
| `ProfileIntensityNode.hpp/cpp` | 250 | Multi-mode QSG node |
| `FootprintAggregator.hpp/cpp` | 350 | Trade -> candle aggregation |
| `TPOProfileBuilder.hpp/cpp` | 300 | TPO letter accumulation |
| `FootprintStreamer.hpp/cpp` | 200 | Server-side streaming |
| `FootprintLabelRenderer.hpp/cpp` | 250 | Text overlay (consumes TimeAxisMapping) |
| `ChartRenderMode.hpp` | 50 | Enum + config |
| `shaders/footprint.frag` | 80 | Footprint shader |
| `shaders/tpo_profile.frag` | 60 | TPO shader |

### Modified Files
| File | Changes |
|------|---------|
| `UnifiedGridRenderer.h/cpp` | Add mode property, switch node types |
| `SentinelStreamProtocol.hpp` | Add message types |
| `HeatmapIntensityNode.cpp` | Extract common shader pattern |
| `CMakeLists.txt (gui)` | Add new shader resources |

### Existing (no changes needed)
| File | Why |
|------|-----|
| `TimeAxisMapping.hpp` | Already has all helpers needed (timeToScreenX, priceToScreenY, bucketStartMsForTime, visibleDataStartMs/EndMs) |
| `MsdfAtlas.hpp/cpp` | Glyph atlas reused as-is for footprint labels |
| `MsdfGlyphNode.hpp/cpp` | Glyph rendering reused as-is |

---

## Implementation Priority

### MVP (Minimum Viable)
1. **FootprintDelta mode** — Single shader change, reuse HeatmapStreamState with reinterpreted encoding
2. **Mode switching** — Q_PROPERTY in renderer, shader selection

### Phase 2
3. **RGBA texture** — Full multi-channel support
4. **FootprintAggregator** — Proper bid/ask/delta tracking
5. **Protocol extension** — Server-side footprint streaming

### Phase 3
6. **TPO mode** — Letter stacking, session boundaries
7. **VolumeProfile mode** — Horizontal bars
8. **FootprintNumbers** — Text overlay hybrid (uses TimeAxisMapping + MSDF atlas)

---

## Quick Win: Footprint Colors from Existing Heatmap

Before full implementation, can prototype footprint look by:
1. Modifying `heatmap_intensity.frag` to color by bid/ask difference instead of side
2. Adjust encoding in `HeatmapTwapStreamer::toIntensityColumnSigned()` to encode delta
3. Result: Delta-colored heatmap without new textures

```glsl
// Quick prototype in heatmap_intensity.frag
float bidMag = mix(encoded * 2.0, 0.0, isAsk);
float askMag = mix(0.0, (encoded - 0.5) * 2.0, isAsk);
float delta = askMag - bidMag;  // Positive = more asks

vec3 color = mix(
    vec3(0.2, 0.6, 0.3),  // Green for bid-heavy
    vec3(0.6, 0.2, 0.2),  // Red for ask-heavy
    step(0.0, delta) * abs(delta)
);
```

---

## Conclusion

The existing heatmap architecture is well-suited for extension. Key insight: **same single-quad pipeline, different shader logic and data encoding**. The ring buffer, column upload, and viewport mapping all transfer directly.

**The TimeAxisMapping refactor (Feb 2026) eliminated the hardest integration problem** — cell alignment between overlays. Any new chart type (footprint, TPO, volume profile) just consumes `TimeAxisMapping` via `lastTimeAxisMapping()` and calls `timeToScreenX()`/`priceToScreenY()`. The pattern is proven by `CandlestickOverlayItem`.

Recommended approach:
1. Start with shader-only prototype (reinterpret existing data)
2. Add RGBA texture support
3. Build proper aggregators
4. Add TPO as final mode (most complex due to time stacking)

All rendering stays GPU-bound. No CPU per-pixel work. Maximum performance maintained.
