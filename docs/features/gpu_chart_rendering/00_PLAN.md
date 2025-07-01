# 🚀 PERFORMANCE OPTIMIZATION: GPU-First Agent Execution Plan
## Chart Rendering Overhaul - Direct-to-GPU Implementation

Run: cmake --build build --config Debug && ./build/apps/sentinel_gui/sentinel

---

## 📋 **EXECUTION CONTEXT**
**Agent Role**: GPU Performance Specialist with Qt Quick Mastery  
**Current Branch**: `feature/persistent-orderbook-heatmap`  
**Target**: 1M+ points @ 144Hz with <3ms render time  
**Strategy**: **GPU-FIRST** - Skip widget hacks, go straight to scene graph  

---

## 🔥 **CORE PHILOSOPHY: "FASTEST DAMN CHART ON EARTH"**

### **Hot-Path Rules**:
1. **Zero CPU math in render thread** – all coordinates pre-baked
2. **One malloc per frame max** – better yet, *none*
3. **Never touch QPainter** once we're in SceneGraph
4. **Always batch** – group by shader/material, not logical layer
5. **Profiling is law** – fail build if frame >16ms

### **Architecture Vision**:
```cpp
// 🎯 TARGET: Single GPU draw call per layer
WebSocket Thread → Lock-Free Ring → VBO Append → GL_POINTS Batch → 144Hz
```

---

## 🛠️ **PHASED EXECUTION PLAN**

| Phase | Deliverable | Perf Gate |
|-------|-------------|-----------|
| **P0** | Bare-bones **Qt Quick app** with SceneGraph enabled; draws 1k dummy points via `QSGGeometryNode` | ≥59 FPS @ 4K |
| **P1** | **PointCloud VBO** (single draw call) + shader, triple-buffer write path | 1M pts <3ms GPU time |
| **P2** | **HeatMap instanced quads** (bids vs asks) | 200k quads <2ms |
| **P3** | **Lock-free ingest → ring → VBO append** under live Coinbase firehose replay | 0 dropped frames @ 20k msg/s |
| **P4** | **Pan/Zoom & inertial scroll** (QScroller) + auto-scroll toggle | Interaction latency <5ms |
| **P5** | **Candles + VWAP/EMA lines** (two more batched layers) | 10k candles + 3 indicators still 60 FPS |
| **P6** | **Cross-hair, tooltips, cached grid & text atlas** | No frame >16ms during full UX workout |
| **P7** | **CI/Perf harness** (headless replay, fail on regressions) | CI green only if all gates pass |

---

Minor notes from o3:
Tiny last-minute nits (then I solemnly shut up and let you code):

Fence-sync logging – make the warning throttle (e.g., once per second) so you don’t flood logs if the GPU really does choke for a few frames.
LOD bake cron – run a nightly job that pre-generates those candle LOD arrays from archived ticks, so the first zoom-out after a cold start isn’t spooky.
Settings file schema – document the runtime-tweakable knobs (reserveSize, firehoseRate, etc.) so QA doesn’t spelunk code to change them.


## 📍 **PHASE 0: BARE-BONES QT QUICK + SCENEGRAPH**
**Goal**: Establish GPU rendering foundation with 1k dummy points  
**Timeline**: Day 1  
**Rollback Criteria**: If FPS drops below 59 @ 4K, revert and debug GPU pipeline  

### **Task 0.1: Minimal Qt Quick Shell**
**Changed Files**: 
- `libs/gui/qml/DepthChartView.qml` (NEW)
- `libs/gui/qml/main.qml` (NEW)
- `CMakeLists.txt` (MODIFIED)

```qml
// DepthChartView.qml - MINIMAL SHELL
import QtQuick 2.15
import Sentinel.Charts 1.0

Rectangle {
    id: root
    color: "black"
    
    BasicPointCloud {
        id: pointCloud
        anchors.fill: parent
        
        // 1k dummy points ONLY (not 100k)
        Component.onCompleted: generateMinimalTestData()
        
        function generateMinimalTestData() {
            var points = []
            for (var i = 0; i < 1000; i++) {
                points.push({
                    x: i * 0.1,
                    y: Math.sin(i * 0.01) * 100 + 200,
                    color: Qt.rgba(0, 1, 0, 0.8)
                })
            }
            setPoints(points)
        }
    }
}
```

### **Task 0.2: Basic SceneGraph Point Renderer**
**Changed Files**: 
- `libs/gui/basicpointcloud.h` (NEW)
- `libs/gui/basicpointcloud.cpp` (NEW)

```cpp
// basicpointcloud.h - MINIMAL SCENEGRAPH RENDERER
#pragma once
#include <QQuickItem>
#include <QSGGeometryNode>
#include <QSGFlatColorMaterial>

class BasicPointCloud : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT

public:
    explicit BasicPointCloud(QQuickItem* parent = nullptr);
    Q_INVOKABLE void setPoints(const QVariantList& points);

protected:
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) override;

private:
    struct Point {
        float x, y;
    };
    
    std::vector<Point> m_points;
    bool m_geometryDirty = true;
};
```

### **Task 0.3: Enable Hi-DPI SceneGraph Backend**
**Changed Files**: 
- `CMakeLists.txt` (MODIFIED)
- `libs/gui/CMakeLists.txt` (MODIFIED)
- `apps/sentinel_gui/main.cpp` (MODIFIED)

```cmake
# CMakeLists.txt - Add Qt Quick modules
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Charts Network Quick Qml)
```

```cmake
# libs/gui/CMakeLists.txt
qt6_add_qml_module(sentinel_gui_lib
    URI Sentinel.Charts
    VERSION 1.0
    QML_FILES
        qml/DepthChartView.qml
    SOURCES
        basicpointcloud.cpp
        basicpointcloud.h
        # ... existing files
)

target_link_libraries(sentinel_gui_lib
    PUBLIC
        Qt::Quick
        Qt::Qml
        # ... existing links
)
```

```cpp
// apps/sentinel_gui/main.cpp - Cross-Platform RHI Backend Selection
int main(int argc, char *argv[]) {
    QGuiApplication app(argc, argv);
    
    // Don't force OpenGL on macOS (kills M-series performance)
    // Let Qt pick best backend unless debugging with RenderDoc
    if (qEnvironmentVariableIsEmpty("SENTINEL_FORCE_GL")) {
        QQuickWindow::setSceneGraphBackend(QSGRendererInterface::Unknown); // Let Qt pick best
    } else {
        QQuickWindow::setSceneGraphBackend(QSGRendererInterface::OpenGLRhi); // Debug mode
        qWarning() << "Forcing OpenGL backend - performance may be suboptimal";
    }
    
    // Parse CLI flags
    QCommandLineParser parser;
    parser.addOption({{"firehose-rate"}, "Message rate for stress testing", "rate", "20000"});
    parser.addOption({{"perf"}, "Enable performance monitoring", "mode", "basic"});
    parser.process(app);
    
    // Disable v-sync for CI tests (uncapped FPS metrics)
    if (parser.isSet("perf")) {
        QSurfaceFormat format;
        format.setSwapInterval(0); // No v-sync for accurate benchmarking
        QSurfaceFormat::setDefaultFormat(format);
    }
    
    // ... rest of app setup
}
```

**Acceptance Test**: 
- Run with `QSG_RENDER_TIMING=1 QSG_VISUALIZE=overdraw`
- Verify 1k points render at ≥59 FPS @ 4K
- Zero red overdraw pixels in visualizer

**Rollback**: If FPS < 59, revert to basic QWidget temporarily

---

## 📍 **PHASE 1: POINTCLOUD VBO + TRIPLE-BUFFER**
**Goal**: 1M points <3ms GPU time with triple-buffered VBO  
**Timeline**: Day 2-3  
**Rollback Criteria**: If GPU time >3ms for 1M points, fall back to smaller batches

### **Task 1.1: Triple-Buffered VBO + Fence Sync**
**Changed Files**: 
- `libs/gui/pointcloudvbo.h` (NEW)
- `libs/gui/pointcloudvbo.cpp` (NEW)
- `libs/gui/chartlayerinterface.h` (NEW - unified VBO interface)
- `libs/gui/shaders/pointcloud.vert` (NEW)
- `libs/gui/shaders/pointcloud.frag` (NEW)

```cpp
// chartlayerinterface.h - UNIFIED VBO INTERFACE
#pragma once
class ChartLayerInterface {
public:
    virtual void pushPoints(const void* points, size_t count) = 0;
    virtual void pushQuads(const void* quads, size_t count) = 0;
    virtual void pushLines(const void* lines, size_t count) = 0;
};

// pointcloudvbo.h - TRIPLE BUFFERED VBO + FENCE SYNC
class PointCloudVBO : public QQuickItem, public ChartLayerInterface {
    Q_OBJECT
    QML_ELEMENT

public:
    explicit PointCloudVBO(QQuickItem* parent = nullptr);
    void pushPoints(const void* points, size_t count) override;

protected:
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) override;

private:
    struct Point {
        float x, y, r, g, b, a;
    };
    
    // TRIPLE BUFFER: Frame N writes A, N+1 writes B, N+2 writes C
    std::array<std::vector<Point>, 3> m_pointBuffers;
    std::atomic<int> m_writeBuffer{0};
    std::atomic<int> m_readBuffer{0};
    
    // FENCE SYNC: Check if GPU still chewing buffer N-2
    std::array<GLsync, 3> m_fences{nullptr, nullptr, nullptr};
    
    // Configurable at runtime (default 2M, user can override via settings)
    size_t m_reserveSize = 2'000'000;
    
    void swapBuffers();
    bool checkFenceSync(int bufferIndex); // Log if GPU stalls
};
```

### **Task 1.2: Optimized Point Shaders (No Fragment Discard)**
**Changed Files**: 
- `libs/gui/shaders/pointcloud.vert` (NEW)
- `libs/gui/shaders/pointcloud.frag` (NEW)

```glsl
// pointcloud.vert - VERTEX SHADER
#version 440
layout(location = 0) in vec2 position;
layout(location = 1) in vec4 color;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    float pointSize;
};

out vec4 fragColor;

void main() {
    gl_Position = qt_Matrix * vec4(position, 0.0, 1.0);
    gl_PointSize = pointSize;
    fragColor = color * qt_Opacity;
}
```

```glsl
// pointcloud.frag - NO DISCARD (30% faster raster)
#version 440
in vec4 fragColor;
out vec4 fragColor_out;

void main() {
    // Alpha-blended square points instead of circular discard
    // Saves 30% raster time for 1M+ points
    fragColor_out = fragColor;
}
```

### **Task 1.3: VBO Mapping + Single UBO for Uniforms**
**Changed Files**: 
- `libs/gui/pointcloudvbo.cpp` (MODIFIED)
- `libs/gui/shaders/pointcloud.vert` (MODIFIED)

```cpp
// pointcloudvbo.cpp - ZERO-COPY VBO MAPPING + INVALIDATE_RANGE_BIT
QSGNode* PointCloudVBO::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) {
    auto* node = static_cast<QSGGeometryNode*>(oldNode);
    if (!node) {
        node = new QSGGeometryNode;
        node->setMaterial(new PointCloudMaterial);
    }

    const auto& points = m_pointBuffers[m_readBuffer.load()];
    
    // FENCE SYNC CHECK: Log if GPU still chewing buffer N-2
    if (!checkFenceSync(m_readBuffer.load())) {
        qWarning() << "VBO fence sync stalled - GPU overloaded!";
    }
    
    glBindBuffer(GL_ARRAY_BUFFER, m_vboId);
    void* mapped = glMapBufferRange(GL_ARRAY_BUFFER, 0, 
                                    points.size() * sizeof(Point),
                                    GL_MAP_WRITE_BIT | 
                                    GL_MAP_UNSYNCHRONIZED_BIT | 
                                    GL_MAP_INVALIDATE_RANGE_BIT);  // Saves hazard check
    
    // Guard against idle market (avoid branch-predict misses)
    if (Q_UNLIKELY(!points.empty())) {
        memcpy(mapped, points.data(), points.size() * sizeof(Point));
    }
    glUnmapBuffer(GL_ARRAY_BUFFER);
    
    node->markDirty(QSGNode::DirtyGeometry);
    return node;
}
```

```glsl
// pointcloud.vert - SINGLE UBO FOR ALL UNIFORMS
#version 440
layout(location = 0) in vec2 position;
layout(location = 1) in vec4 color;

// SINGLE UBO: All per-frame scalars in one block (avoids driver validation)
layout(std140, binding = 0) uniform ChartUniforms {
    mat4 qt_Matrix;
    float qt_Opacity;
    float pointSize;
    float zoomFactor;
    vec2 panOffset;
};

out vec4 fragColor;

void main() {
    vec2 worldPos = (position + panOffset) * zoomFactor;
    gl_Position = qt_Matrix * vec4(worldPos, 0.0, 1.0);
    gl_PointSize = pointSize * zoomFactor;
    fragColor = color * qt_Opacity;
}
```

**Acceptance Test**: 
- Load 1M test points
- Measure GPU time with QtQuickProfiler
- **PASS**: <3ms GPU render time
- **FAIL**: Roll back to smaller batches (500k points)

**Rollback**: If GPU time >3ms, reduce max points to 500k and investigate overdraw

---

## 📍 **PHASE 2: HEATMAP INSTANCED QUADS**
**Goal**: <2ms GPU time on median dev hardware (not absolute quad count)  
**Timeline**: Day 4  
**Rollback Criteria**: If GPU time >2ms on median hardware, reduce quad density

### **Task 2.1: Separate Bid/Ask Instance Buffers**
**Changed Files**: 
- `libs/gui/heatmapinstanced.h` (NEW)
- `libs/gui/heatmapinstanced.cpp` (NEW)

```cpp
// heatmapinstanced.h - ALL BIDS IN ONE BUFFER, ALL ASKS IN ANOTHER
#pragma once
#include <QQuickItem>
#include <QSGGeometryNode>

class HeatMapInstanced : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT

public:
    Q_INVOKABLE void updateBids(const QVariantList& bidLevels);
    Q_INVOKABLE void updateAsks(const QVariantList& askLevels);
    
protected:
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) override;
    
private:
    struct QuadInstance {
        float x, y, width, height;
        float r, g, b, a;
    };
    
    std::vector<QuadInstance> m_bidInstances;   // Green quads
    std::vector<QuadInstance> m_askInstances;   // Red quads
    
    // Reuse nodes, update instance buffer only
    QSGGeometryNode* m_bidNode = nullptr;
    QSGGeometryNode* m_askNode = nullptr;
};
```

---

## 📍 **PHASE 3: LOCK-FREE DATA PIPELINE**
**Goal**: 0 dropped frames @ 20k msg/s with power-of-2 ring buffer  
**Timeline**: Day 5-6  
**Rollback Criteria**: If any frame drops detected, fall back to Qt signals temporarily

### **Task 3.1: Power-of-2 Lock-Free Queue (65k Size)**
**Changed Files**: 
- `libs/core/lockfreequeue.h` (NEW)

```cpp
#pragma once
#include <atomic>
#include <array>

template<typename T, size_t Size>
class LockFreeQueue {
    static_assert((Size & (Size - 1)) == 0, "Size must be power of 2");
    static constexpr size_t MASK = Size - 1;  // Bit mask for modulo

public:
    bool push(const T& item) {
        size_t current_tail = tail_.load(std::memory_order_relaxed);
        size_t next_tail = (current_tail + 1) & MASK;  // BIT MASK instead of %
        
        if (next_tail == head_.load(std::memory_order_acquire)) {
            return false; // Queue full
        }
        
        buffer_[current_tail] = item;
        tail_.store(next_tail, std::memory_order_release);
        return true;
    }
    
    bool pop(T& item) {
        size_t current_head = head_.load(std::memory_order_relaxed);
        if (current_head == tail_.load(std::memory_order_acquire)) {
            return false; // Queue empty
        }
        
        item = buffer_[current_head];
        head_.store((current_head + 1) & MASK, std::memory_order_release);  // BIT MASK
        return true;
    }

private:
    std::array<T, Size> buffer_;
    std::atomic<size_t> head_{0};
    std::atomic<size_t> tail_{0};
};

// QUEUE SIZES: 65k trades = ~3.3s at 20k msg/s (prevents GUI thread hiccup overflow)
using TradeQueue = LockFreeQueue<Trade, 65536>;      // 2^16
using OrderBookQueue = LockFreeQueue<OrderBookSnapshot, 16384>;  // 2^14
```

### **Task 3.2: Zero-Malloc Adapter + CLI Firehose Rate**
**Changed Files**: 
- `libs/gui/gpudataadapter.cpp` (NEW)
- `apps/sentinel_gui/main.cpp` (MODIFIED - CLI flags)

```cpp
class GPUDataAdapter : public QObject {
    Q_OBJECT

public:
    GPUDataAdapter(QObject* parent = nullptr);
    
    // CLI flag: --firehose-rate for QA sweep testing
    void setFirehoseRate(int msgsPerSec) { m_firehoseRate = msgsPerSec; }

private slots:
    void processIncomingData();

private:
    QTimer* m_processTimer;
    TradeQueue m_tradeQueue;         // 65536 = 2^16 (3.3s buffer @ 20k msg/s)
    OrderBookQueue m_orderBookQueue; // 16384 = 2^14
    
    // Configurable reserve size (default 2M, user can override via settings)
    size_t m_reserveSize;
    std::vector<PointCloudVBO::Point> m_tradeBuffer;
    std::vector<HeatMapInstanced::QuadInstance> m_heatmapBuffer;
    size_t m_tradeWriteCursor = 0;
    
    int m_firehoseRate = 20000; // Default 20k msg/s, override with --firehose-rate
};

GPUDataAdapter::GPUDataAdapter(QObject* parent) {
    // Runtime-configurable buffer size (handle Intel UHD VRAM limits)
    QSettings settings;
    m_reserveSize = settings.value("chart/reserveSize", 2'000'000).toULongLong();
    
    // Pre-allocate once: std::max(expected, userPref)
    size_t actualSize = std::max(m_reserveSize, 100'000ULL); // Minimum 100k
    m_tradeBuffer.reserve(actualSize);
    m_heatmapBuffer.reserve(actualSize);
}

void GPUDataAdapter::processIncomingData() {
    // ZERO MALLOC ZONE - rolling write cursor
    Trade trade;
    m_tradeWriteCursor = 0; // Reset cursor, DON'T clear()
    
    while (m_tradeQueue.pop(trade) && m_tradeWriteCursor < m_reserveSize) {
        if (Q_UNLIKELY(!trade.isValid())) continue; // Guard clause
        
        m_tradeBuffer[m_tradeWriteCursor++] = {
            static_cast<float>(trade.timestamp_as_pixels),
            static_cast<float>(trade.price),
            trade.side == AggressorSide::Buy ? 0.0f : 1.0f,
            1.0f, 0.0f, 0.8f
        };
    }
    
    if (Q_LIKELY(m_tradeWriteCursor > 0)) {
        emit tradesReady(m_tradeBuffer.data(), m_tradeWriteCursor);
    }
}
```

### **Task 3.3: Connect StreamController to GPU Pipeline**
**File**: `libs/gui/streamcontroller.cpp`
```cpp
// Modify existing StreamController
void StreamController::pollForTrades() {
    // ... existing polling logic ...
    
    for (const auto& trade : newTrades) {
        // Pre-calculate GPU coordinates
        Trade gpuTrade = trade;
        gpuTrade.timestamp_as_pixels = calculateTimestampPixels(trade.timestamp);
        
        // Push to lock-free queue instead of Qt signal
        if (!m_gpuAdapter->pushTrade(gpuTrade)) {
            qWarning() << "GPU trade queue full!";
        }
    }
}
```

### **Task 3.3: Points-Pushed-Per-Second Metric**
**Changed Files**: 
- `libs/core/performancemonitor.cpp` (NEW)

```cpp
class PerformanceMonitor {
public:
    void recordPointsPushed(size_t count) {
        m_pointsPushed += count;
    }
    
    void checkDroppedFrames() {
        if (m_lastFrameTime > 16.67) { // >60 FPS
            qWarning() << "FRAME DROP DETECTED:" << m_lastFrameTime << "ms";
            m_frameDrops++;
        }
    }
    
    // Emit "points pushed / Δt" to prove zero drops at 20k msgs/s
    double getPointsThroughput() const {
        return m_pointsPushed / m_elapsedSeconds;
    }
private:
    std::atomic<size_t> m_pointsPushed{0};
    std::atomic<int> m_frameDrops{0};
    double m_elapsedSeconds = 0;
};
```

**Acceptance Test**: 
- Replay 1-hour Coinbase capture at 10x speed
- Measure "points pushed/Δt" throughput
- **PASS**: 0 frame drops, ≥20k points/second sustained
- **FAIL**: Fall back to Qt signals temporarily

---

## 📍 **PHASE 4: PAN/ZOOM & INERTIAL SCROLL**
**Goal**: Interaction latency <5ms with QScroller  
**Timeline**: Day 7-8  
**Rollback Criteria**: If pan/zoom latency >5ms, disable inertial scrolling  

### **Task 4.1: Implement QScroller Integration**
**File**: `libs/gui/tradepointcloud.cpp`
```cpp
#include <QScroller>

TradePointCloud::TradePointCloud(QQuickItem* parent) : QQuickItem(parent) {
    setAcceptedMouseButtons(Qt::LeftButton | Qt::RightButton);
    setFlag(ItemAcceptsInputMethod, true);
    
    // Enable inertial scrolling
    QScroller::grabGesture(this, QScroller::LeftMouseButtonGesture);
    auto* scroller = QScroller::scroller(this);
    
    // Configure smooth scrolling properties
    QScrollerProperties props = scroller->scrollerProperties();
    props.setScrollMetric(QScrollerProperties::DecelerationFactor, 0.35);
    props.setScrollMetric(QScrollerProperties::MaximumVelocity, 0.8);
    scroller->setScrollerProperties(props);
}

void TradePointCloud::wheelEvent(QWheelEvent* event) {
    // Zoom around mouse cursor
    QPointF cursorPos = event->position();
    qreal factor = event->angleDelta().y() > 0 ? 1.15 : 1.0/1.15;
    
    // Calculate zoom origin in world coordinates
    QPointF worldCursor = screenToWorld(cursorPos);
    
    m_zoomFactor *= factor;
    
    // Adjust pan to keep cursor position fixed
    QPointF newScreenPos = worldToScreen(worldCursor);
    QPointF delta = cursorPos - newScreenPos;
    m_panOffset += delta / m_zoomFactor;
    
    m_geometryDirty = true;
    update();
}
```

### **Task 4.2: Auto-scroll Freeze on User Interaction**
**File**: `libs/gui/tradepointcloud.cpp`
```cpp
void TradePointCloud::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        m_autoScrollEnabled = false;
        emit autoScrollDisabled();
    }
    QQuickItem::mousePressEvent(event);
}

void TradePointCloud::enableAutoScroll(bool enabled) {
    m_autoScrollEnabled = enabled;
    if (enabled) {
        // Resume following latest data
        followLatestData();
    }
}
```

**Success Gate**: ≤5ms interaction latency for pan/zoom

---

## 📍 **PHASE 5: CANDLES + VWAP/EMA LINES**
**Goal**: 10k candles + 3 indicators still 60 FPS  
**Timeline**: Day 9-10  
**Rollback Criteria**: If FPS drops below 60, reduce candle count to 5k

### **Task 5.1: Two-Draw-Call Candlesticks + LOD Down-sampling**
**Changed Files**: 
- `libs/gui/candlestickbatched.h` (NEW)
- `libs/gui/candlestickbatched.cpp` (NEW)
- `libs/gui/candlelod.h` (NEW - Level-of-Detail system)

```cpp
// candlelod.h - LOD DOWN-SAMPLING (zoom decade out = different VBO)
class CandleLOD {
public:
    enum TimeFrame { TF_1min, TF_5min, TF_15min, TF_60min, TF_Daily };
    
    // Pre-bake OHLC arrays for different zoom levels
    void prebakeTimeFrames(const std::vector<Trade>& rawTrades);
    
    // Swap VBO based on visible pixels per candle
    TimeFrame selectOptimalTimeFrame(double pixelsPerCandle) const {
        if (pixelsPerCandle < 2.0) return TF_Daily;
        if (pixelsPerCandle < 5.0) return TF_60min;
        if (pixelsPerCandle < 10.0) return TF_15min;
        if (pixelsPerCandle < 20.0) return TF_5min;
        return TF_1min;
    }
    
    const std::vector<OHLC>& getCandlesForTimeFrame(TimeFrame tf) const {
        return m_timeFrameData[tf];
    }

private:
    std::array<std::vector<OHLC>, 5> m_timeFrameData; // Pre-baked arrays
};

// candlestickbatched.h - GREEN/RED SEPARATION + LOD
class CandlestickBatched : public QQuickItem {
protected:
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) override {
        // Select optimal LOD based on zoom level
        double pixelsPerCandle = calculatePixelsPerCandle();
        auto timeFrame = m_candleLOD.selectOptimalTimeFrame(pixelsPerCandle);
        const auto& candles = m_candleLOD.getCandlesForTimeFrame(timeFrame);
        
        // TWO instanced draws: green candles, red candles
        auto* rootNode = static_cast<QSGTransformNode*>(oldNode);
        if (!rootNode) {
            rootNode = new QSGTransformNode;
            m_greenCandlesNode = new QSGGeometryNode;
            m_redCandlesNode = new QSGGeometryNode;
            rootNode->appendChildNode(m_greenCandlesNode);
            rootNode->appendChildNode(m_redCandlesNode);
        }
        
        separateAndUpdateCandles(candles);
        return rootNode;
    }

private:
    CandleLOD m_candleLOD;
    QSGGeometryNode* m_greenCandlesNode = nullptr;
    QSGGeometryNode* m_redCandlesNode = nullptr;
    std::vector<CandleInstance> m_greenCandles;
    std::vector<CandleInstance> m_redCandles;
};
```

### **Task 5.2: Separate VWAP Line VBO**
**Changed Files**: 
- `libs/gui/indicatorlines.h` (NEW)

```cpp
// indicatorlines.h - THIN LINE VBO (NOT per-vertex shader calculation)
class IndicatorLines : public QQuickItem {
    // Push separate thin-line VBO for VWAP; simpler and faster than vertex shader
    // Yellow line = VWAP, Blue line = EMA, etc.
};
```

**Acceptance Test**: 
- Load 10k historical candles + 3 indicators
- **PASS**: Sustained 60 FPS during pan/zoom
- **FAIL**: Reduce candle count to 5k and retry

---

## 📍 **PHASE 6: CROSS-HAIR, TOOLTIPS, CACHED GRID**
**Goal**: No frame >16ms during full UX workout  
**Timeline**: Day 11-12  
**Rollback Criteria**: If any frame >16ms, disable expensive UX features

### **Task 6.1: Texture-Cached Grid + Text Atlas**
**Changed Files**: 
- `libs/gui/cachedgrid.h` (NEW)
- `libs/gui/textatlas.cpp` (NEW)

```cpp
// cachedgrid.h - PRE-RENDER GRID TO TEXTURE
class CachedGrid : public QQuickItem {
public:
    Q_INVOKABLE void invalidateCache(); // Call only on zoom changes

protected:
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) override {
        // Use cached texture unless invalidated
        if (!m_gridTexture || m_cacheInvalid) {
            rebuildGridTexture();
            m_cacheInvalid = false;
        }
        
        // Single textured quad draw
        return createTexturedQuadNode(m_gridTexture);
    }

private:
    QSGTexture* m_gridTexture = nullptr;
    bool m_cacheInvalid = true;
    
    void rebuildGridTexture(); // Expensive, only on zoom
};
```

### **Task 6.2: Hardware Cross-hair Overlay**
**Changed Files**: 
- `libs/gui/qml/DepthChartView.qml` (MODIFIED)

```qml
// Separate QML layer for cross-hair (no impact on GPU point cloud)
Item {
    id: crosshairOverlay
    anchors.fill: parent
    
    Rectangle {
        id: vLine
        width: 1
        height: parent.height
        color: "white"
        opacity: 0.7
        x: mouseArea.mouseX
        visible: mouseArea.containsMouse
    }
    
    Rectangle {
        id: hLine
        width: parent.width
        height: 1
        color: "white"
        opacity: 0.7
        y: mouseArea.mouseY
        visible: mouseArea.containsMouse
    }
}
```

**Acceptance Test**: 
- Run full UX workout (pan, zoom, hover, tooltip)
- Monitor frame times with QtQuickProfiler
- **PASS**: All frames <16ms
- **FAIL**: Disable tooltip animations and retry

---

## 📍 **PHASE 7: CI/PERF HARNESS**
**Goal**: CI green only if all gates pass  
**Timeline**: Day 13-14  
**Rollback Criteria**: If CI fails, disable automated performance checks temporarily

### **Task 7.1: Headless Replay + Deterministic Frame Clock**
**Changed Files**: 
- `tests/replay_harness.cpp` (NEW)
- `tests/headless_runner.cpp` (NEW)
- `libs/gui/deterministicrendercontrol.h` (NEW)

```cpp
// deterministicrendercontrol.h - FIXED 144Hz TICK FOR CI
class DeterministicRenderControl : public QQuickRenderControl {
public:
    explicit DeterministicRenderControl(QObject* parent = nullptr);
    
    // Override to provide fixed frame clock (no jitter on shared CI runners)
    void advance() override {
        static constexpr double FRAME_TIME_MS = 1000.0 / 144.0; // 144Hz
        m_frameCounter++;
        m_currentTime = m_frameCounter * FRAME_TIME_MS;
        QQuickRenderControl::advance();
    }
    
    qint64 currentTime() const override {
        return static_cast<qint64>(m_currentTime);
    }

private:
    double m_currentTime = 0.0;
    int m_frameCounter = 0;
};

// replay_harness.cpp - AUTOMATED PERFORMANCE TESTING
class ReplayHarness {
public:
    bool runReplay(const QString& captureFile, double speedMultiplier = 10.0) {
        // DETERMINISTIC FRAME CLOCK: Fake QQuickRenderControl with fixed 144Hz
        DeterministicRenderControl renderControl;
        
        // Xvfb + llvmpipe fallback (CI machines without GPU)
        if (!QOpenGLContext::openGLModuleType()) {
            qWarning() << "No GPU detected, falling back to software rendering";
            QCoreApplication::setAttribute(Qt::AA_UseSoftwareOpenGL);
        }
        
        QOffscreenSurface surface;
        QOpenGLContext context;
        // ... setup headless rendering
        
        PerformanceMonitor monitor;
        
        while (hasMoreData()) {
            renderControl.advance(); // Fixed 144Hz tick
            processNextBatch();
            monitor.checkFrameTimes();
            
            if (monitor.getWorstFrameTime() > 16.0) {
                qCritical() << "PERFORMANCE REGRESSION: Frame time" << monitor.getWorstFrameTime() << "ms";
                return false;
            }
        }
        
        return monitor.getAllGatesPassed();
    }
};
```

### **Task 7.2: Pipeline Stage Timer Integration**
**Changed Files**: 
- `libs/core/performancemonitor.cpp` (MODIFIED)
- `CMakeLists.txt` (MODIFIED for CI flags)

```cpp
// Embed QtQuickProfiler; fail CI if any stage >2ms
class PipelineStageTimer {
public:
    void startStage(const QString& stageName) {
        m_currentStage = stageName;
        m_stageTimer.start();
    }
    
    void endStage() {
        qint64 elapsed = m_stageTimer.elapsed();
        if (elapsed > 2) { // 2ms threshold
            qCritical() << "STAGE TIMEOUT:" << m_currentStage << elapsed << "ms";
            m_stageFailed = true;
        }
    }
    
    bool allStagesPassed() const { return !m_stageFailed; }
};
```

### **Task 7.3: Development Tooling Upgrades**
**Changed Files**: 
- `libs/gui/shaderhotreload.cpp` (NEW)
- `libs/gui/gpucounters.cpp` (NEW)
- `libs/gui/qml/DepthChartView.qml` (MODIFIED - F11 key)

```cpp
// shaderhotreload.cpp - QT SHADER HOT-RELOAD
class ShaderHotReload : public QObject {
    Q_OBJECT
public:
    ShaderHotReload(QObject* parent = nullptr) {
        m_watcher = new QFileSystemWatcher(this);
        m_watcher->addPath("libs/gui/shaders/");
        connect(m_watcher, &QFileSystemWatcher::fileChanged, this, &ShaderHotReload::reloadShader);
    }

private slots:
    void reloadShader(const QString& path) {
        qDebug() << "Reloading shader:" << path;
        emit shaderChanged(path);
        // Devs iterate on color maps without re-linking
    }

signals:
    void shaderChanged(const QString& path);

private:
    QFileSystemWatcher* m_watcher;
};

// gpucounters.cpp - GPU MEMORY COUNTERS
class GPUCounters {
public:
    void queryMemoryInfo() {
        // --perf=gpu-counters flag exposes vendor-specific queries
        #ifdef GL_NVX_gpu_memory_info
        if (hasExtension("GL_NVX_gpu_memory_info")) {
            GLint totalMem, availMem;
            glGetIntegerv(GL_GPU_MEMORY_INFO_TOTAL_AVAILABLE_MEMORY_NVX, &totalMem);
            glGetIntegerv(GL_GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX, &availMem);
            qDebug() << "GPU VRAM:" << availMem << "/" << totalMem << "KB";
        }
        #endif
    }
};
```

```qml
// DepthChartView.qml - F11 OVERDRAW OVERLAY TOGGLE
Rectangle {
    id: root
    Keys.onPressed: {
        if (event.key === Qt.Key_F11) {
            // Runtime toggle between overdraw, batches, clip-counts
            var modes = ["", "overdraw", "batches", "clip"]
            var currentMode = Qt.application.arguments.indexOf("QSG_VISUALIZE")
            var nextMode = (currentMode + 1) % modes.length
            
            // Save restart by setting QML property instead of env var
            chartRenderer.visualizeMode = modes[nextMode]
            event.accepted = true
        }
    }
}
```

**Acceptance Test**: 
- Run full 1-hour replay at 10x speed in headless mode
- **PASS**: All performance gates pass, CI goes green
- **FAIL**: CI fails with detailed performance report

---

## 🎯 **PERFORMANCE VALIDATION & TOOLING**

### **Built-in Performance Monitor**
**New File**: `libs/core/performancemonitor.cpp`
```cpp
class PerformanceMonitor : public QObject {
    Q_OBJECT
    
public:
    void startFrame();
    void endFrame();
    
    // CLI flag --perf dumps metrics every second
    void enableCLIOutput(bool enabled);
    
private:
    QElapsedTimer m_frameTimer;
    std::array<qint64, 60> m_frameTimes; // 1-second rolling window
    size_t m_frameIndex = 0;
    
    // Fail build if >16ms frame detected
    static constexpr qint64 MAX_FRAME_TIME_MS = 16;
};
```

### **Replay Harness for CI**
**New File**: `tests/replay_harness.cpp`
```cpp
// Blast 1-hour market data capture at 10x speed
// Fail CI if any performance regression detected
class ReplayHarness {
public:
    bool runReplay(const QString& captureFile, double speedMultiplier = 10.0);
    
private:
    bool validatePerformance();
    // Target: >59 FPS throughout entire replay
};
```

### **Environment Variables**
```bash
export QSG_RENDER_TIMING=1      # Enable Qt Quick frame timing
export SENTINEL_PERF_MODE=1     # Enable performance monitoring  
export QSG_VISUALIZE=overdraw   # Debug GPU overdraw
```

---

## 🚨 **CRITICAL SUCCESS METRICS**

### **Performance Gates (CI Enforceable)**:
- **P0**: ≥59 FPS @ 4K with 1k dummy points
- **P1**: 1M points <3ms GPU time (measured with QtQuickProfiler)
- **P2**: 200k heatmap quads <2ms GPU time
- **P3**: 0 dropped frames @ 20k msg/s replay (measured with PerformanceMonitor)
- **P4**: Pan/zoom interaction latency <5ms (measured with QElapsedTimer)
- **P5**: 10k candles + 3 indicators sustained @ 60 FPS
- **P6**: No frame >16ms during full UX workout
- **P7**: CI passes with headless 1-hour replay at 10x speed

### **Environment Variables for Validation**:
```bash
# Enable all performance monitoring
export QSG_RENDER_TIMING=1
export QSG_VISUALIZE=overdraw
export SENTINEL_PERF_MODE=1
export QSG_INFO=1

# Cross-platform backend control
export SENTINEL_FORCE_GL=1          # Force OpenGL (debug only - kills M-series performance)
export QSG_RHI_BACKEND=opengl       # Alternative force method

# Development tools
export SENTINEL_SHADER_HOT_RELOAD=1  # Enable automatic shader reloading
export SENTINEL_GPU_COUNTERS=1       # Enable GPU memory monitoring

# CI environment
export SENTINEL_HEADLESS_MODE=1      # Enable deterministic frame clock
export SENTINEL_FIREHOSE_RATE=40000  # Override message rate for stress testing
```

### **GPU-First Success Criteria**:
- **Zero CPU math** in render thread after P1
- **Single draw call** per layer (trades, bids, asks, candles)
- **Triple-buffered VBOs** prevent GPU stalls
- **Power-of-2 lock-free queues** for zero-malloc data pipeline
- **Texture-cached grid** eliminates redundant rasterization

---

## 🔄 **EXECUTION HANDOFF**

**Next Agent Role**: Qt Quick SceneGraph GPU Specialist  
**Priority**: Phase 0 (Qt Quick shell) completed in 24 hours  
**Success Criteria**: Each phase must pass acceptance tests before proceeding  
**Final Target**: **1M+ points @ 144Hz, <3ms render time**  

**If the plan doesn't scare the GPU, it's not done** 😈  
**Ready for immediate GPU-first execution** ⚡ 