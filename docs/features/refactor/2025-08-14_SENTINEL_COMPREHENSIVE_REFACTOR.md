# 🚀 SENTINEL COMPREHENSIVE REFACTOR - ULTIMATE BLUEPRINT

**TL;DR: Transform Sentinel from "works by magic" to production-grade professional architecture**

---

## 📊 **REFACTOR SCOPE & IMPACT**

### **Current State Analysis:**
- **UGR**: 900+ lines of mixed responsibilities (should be <200 slim adapter)
- **Data Pipeline**: 3× unnecessary conversions (Dense→Sparse→Dense→Sparse)  
- **Architecture**: "Magic" timing with retry logic and scattered setup
- **Performance**: 10 FPS crashes due to O(log N) maps + 8× redundant timeframe processing
- **LOD Systems**: Price & timeframe LOD disconnected, causing visual gaps

### **Target State:**
- **Professional Trading Terminal**: Bloomberg/Bookmap quality with 60+ FPS at 10K+ cells
- **Clean Architecture**: Single responsibility components with clear interfaces
- **Optimized Pipeline**: Direct dense→dense flow with O(1) tick-based processing
- **Coordinated LOD**: Unified time+price resolution system
- **Elimination**: Remove all "magic" timing, facades, and redundant systems

### **Expected Performance Gains:**
- **CPU**: 8-10× faster (eliminate redundant timeframe processing)
- **Memory**: 3× reduction (eliminate conversion overhead)  
- **FPS**: Stable 60+ FPS with 10K+ cells (vs current 10 FPS crashes)
- **Latency**: Sub-millisecond UI response (eliminate retry logic)

---

## 🎯 **PHASE 1: DATA PIPELINE CLEANUP** 
*Timeline: Tonight (2-3 hours)*
*Goal: Eliminate architectural redundancy and timing issues*

### **1.1 Eliminate CoinbaseStreamClient Facade**
**Problem**: Unnecessary wrapper around MarketDataCore
```cpp
// Current: CoinbaseStreamClient → MarketDataCore  
// Target: Direct MarketDataCore usage
```

**Changes:**
- **Delete**: `libs/core/CoinbaseStreamClient.{hpp,cpp}`
- **Update**: `MainWindowGpu.cpp` to use MarketDataCore directly
- **Remove**: `getLiveOrderBook()` facade method
- **Benefit**: Eliminate layer of indirection + reduce component count

### **1.2 Fix QML Loading Race Conditions**
**Problem**: Multiple async initialization flows with retry logic
```cpp
// Current: setupConnections() → connectToGPUChart() → retry logic
// onSubscribe() → findChild() → connect signals
// Target: Proper constructor-based initialization
```

**Changes:**
- **Move**: Data pipeline creation to constructor  
- **Remove**: `connectToGPUChart()` retry logic
- **Unify**: Component finding into single initialization method
- **Fix**: QML component lifecycle dependency

### **1.3 Consolidate Type Registrations**
**Problem**: Duplicate type registrations
```cpp
// main.cpp:31 - qRegisterMetaType<OrderBook>()  
// MainWindowGpu.cpp:24 - qRegisterMetaType<std::shared_ptr<const OrderBook>>()
```

**Changes:**
- **Remove**: Duplicate registrations
- **Standardize**: Single OrderBook type throughout pipeline
- **Update**: Signal signatures to use consistent types

### **1.4 Fix Exchange Timestamp Usage**
**Problem**: Using system time instead of exchange time
```cpp
// Current: book->timestamp = std::chrono::system_clock::now(); // WRONG!
// Target: book->timestamp = exchange_timestamp_from_websocket;
```

**Changes:**
- **DataCache.cpp**: Use exchange timestamps for bucketing
- **LTSE**: Exchange-time-based slice boundaries  
- **Fix**: Eliminates time-bucketing skew under jitter

---

## ⚡ **PHASE 2: LTSE PERFORMANCE OPTIMIZATION**
*Timeline: Tomorrow (4-6 hours)*
*Goal: Implement the tick-based optimization strategy*

### **2.1 Eliminate Dense→Sparse→Dense Conversion Hell**
**Problem**: Triple data format conversion per snapshot
```cpp
// Current: LiveOrderBook(dense) → OrderBook(sparse) → LTSE(dense again)
// Target: LiveOrderBook(dense) → LTSE(dense) direct
```

**Implementation:**
- **Direct Signal**: `MarketDataCore::liveOrderBookUpdated(LiveOrderBook&)`
- **Remove**: `getLiveOrderBook()` sparse conversion
- **Update**: LTSE to accept dense LiveOrderBook directly
- **Benefit**: 3× performance improvement, eliminate conversion overhead

### **2.2 Tick-Based Price Optimization**
**Problem**: `std::map<double, Metrics>` causing cache misses + tree overhead
```cpp
// Current: std::map<double, PriceLevelMetrics>  // O(log N) + float key drift
// Target: robin_map<Tick, PriceLevelMetrics>    // O(1) + integer keys
```

**Implementation:**
```cpp
using Tick = int32_t;  // Price index = round(price / tickSize)

inline Tick priceToTick(double price) const {
    return static_cast<Tick>(std::llround(price / m_priceResolution));
}

// Replace all price storage with Tick indices
robin_map<Tick, Metrics> bidMetrics, askMetrics;
```

**Benefits:**
- **Performance**: O(log N) → O(1) price lookups
- **Memory**: Eliminate floating-point key overhead
- **Cache**: Contiguous integer access patterns

### **2.3 Eliminate updateAllTimeframes() Redundancy**
**Problem**: Processing same snapshot 8× for different timeframes
```cpp
// Current: for (timeframe : m_timeframes) updateTimeframe(tf, snapshot); // 8× work!
// Target: Single base column + rolling sums for coarser timeframes
```

**Implementation:**
```cpp
// Only build base timeframe (100ms)
updateBaseTimeframe(m_baseTimeframe_ms, snapshot);

// Derive coarser timeframes via rolling sums:
// 250ms = sum of last 2.5 base columns
// 1000ms = sum of last 10 base columns  
struct RollingTimeframe {
    int K;  // base columns per coarse column
    std::deque<ColumnData> lastK;
    ColumnData rollingSum;
};
```

**Benefits:**
- **CPU**: 8× reduction in aggregation work
- **Memory**: Eliminate redundant timeframe storage
- **Latency**: Single base column computation per snapshot

### **2.4 Version Stamp Presence Detection**
**Problem**: O(L) scanning for "disappearing levels" every snapshot
```cpp
// Current: Loop all levels checking if present in new snapshot
// Target: Version stamp + time-based persistence
```

**Implementation:**
```cpp
struct PriceLevelMetrics {
    uint32_t lastSeenSeq = 0;  // Global sequence number
    int64_t firstSeen_ms;
    int64_t lastSeen_ms;
};

uint32_t gSeq = 0;
void addSnapshot(const Snapshot& s) {
    ++gSeq;
    for (auto& [tick, size] : s.bids) {
        auto& m = slice.bidMetrics[tick];
        updateMetrics(m, size, s.timestamp_ms);
        m.lastSeenSeq = gSeq;  // Mark as present this snapshot
    }
    // Persistence computed from time deltas, not O(L) scanning
}
```

**Benefits:**
- **Eliminate**: O(L) disappearing level detection
- **Time-based**: Persistence ratios from firstSeen/lastSeen timestamps
- **Performance**: No per-snapshot full-map scanning

---

## 🏗️ **PHASE 3: UGR SLIM ADAPTER REFACTOR**
*Timeline: Weekend (6-8 hours)*
*Goal: Transform 900-line monolith into <200 line QML adapter*

### **3.1 Extract DataPipeline Orchestrator**
**Current Problem**: UGR owns + manages LTSE, does data processing
```cpp
// Current: UGR has business logic mixed with QML interface
class UnifiedGridRenderer {
    std::unique_ptr<LiquidityTimeSeriesEngine> m_liquidityEngine;  // WRONG!
    void updateVisibleCells() { /* 50 lines of logic */ }          // WRONG!
};
```

**Target Architecture**:
```cpp
class DataPipeline {
    // Owns all data processing components
    std::unique_ptr<LiquidityTimeSeriesEngine> m_engine;
    std::unique_ptr<DataProcessor> m_processor;
    
    // Unified LOD calculation (time + price together)
    LODRecommendation calculateOptimalLOD(Viewport viewport, int maxCells);
    
    // Data processing pipeline
    std::vector<CellInstance> generateVisibleCells(Viewport viewport);
};
```

**Extraction Plan:**
- **Move**: `m_liquidityEngine` ownership to DataPipeline
- **Move**: `updateVisibleCells()` → `DataPipeline::generateVisibleCells()`
- **Move**: `createCellsFromLiquiditySlice()` → DataPipeline
- **Unify**: Price + timeframe LOD calculation in single method

### **3.2 Extract RenderOrchestrator**  
**Current Problem**: UGR mixes rendering strategies with QML interface
```cpp
// Current: UGR manages render strategies + does coordinate conversion
class UnifiedGridRenderer {
    std::unique_ptr<IRenderStrategy> m_heatmapStrategy;     // WRONG!
    QRectF timeSliceToScreenRect() { /* coordinate logic */ }  // WRONG!
};
```

**Target Architecture**:
```cpp
class RenderOrchestrator {
    // Strategy management
    std::unique_ptr<HeatmapStrategy> m_heatmapStrategy;
    std::unique_ptr<TradeFlowStrategy> m_tradeFlowStrategy;
    std::unique_ptr<CandleStrategy> m_candleStrategy;
    
    // Coordinate system integration
    CoordinateSystem m_coordinates;
    
    // Rendering pipeline
    QSGNode* renderCells(const std::vector<CellInstance>& cells, RenderMode mode);
};
```

**Extraction Plan:**
- **Move**: All render strategies to RenderOrchestrator
- **Move**: `timeSliceToScreenRect()` → `CoordinateSystem::cellToScreenRect()`
- **Move**: Geometry cache management to RenderOrchestrator
- **Consolidate**: All coordinate conversion in CoordinateSystem

### **3.3 UGR Becomes Pure QML Adapter**
**Target**: Sub-200 line slim QML interface adapter
```cpp
class UnifiedGridRenderer : public QQuickItem {
    // ONLY QML properties + forwarding methods
    Q_PROPERTY(RenderMode renderMode WRITE setRenderMode ...)
    Q_INVOKABLE void zoomIn() { m_viewport->zoomIn(); }
    Q_INVOKABLE void addTrade(Trade t) { m_dataPipeline->addTrade(t); }
    
private:
    // V3 Clean Architecture (only 3 components!)
    std::unique_ptr<DataPipeline> m_dataPipeline;
    std::unique_ptr<RenderOrchestrator> m_renderOrchestrator;  
    std::unique_ptr<ViewportController> m_viewportController;
    
    // Pure delegation - no business logic!
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) override {
        auto cells = m_dataPipeline->generateVisibleCells(m_viewportController->getViewport());
        return m_renderOrchestrator->renderCells(cells, m_renderMode);
    }
};
```

**Cleanup Plan:**
- **Remove**: All business logic from UGR
- **Keep**: Only Q_PROPERTY + Q_INVOKABLE forwarding methods
- **Simplify**: Constructor to just create 3 components + wire signals
- **Result**: <200 lines of pure QML adapter code

---

## 🔄 **PHASE 4: COORDINATED LOD SYSTEM**
*Timeline: Weekend evening (2-3 hours)*
*Goal: Unified time + price LOD preventing visual gaps*

### **4.1 Unified LOD Calculation**
**Problem**: Separate time/price LOD causing mismatched cell sizes
```cpp
// Current: GridViewState calculates price LOD, LTSE calculates time LOD separately
// Target: Single coordinated LOD calculation
```

**Implementation:**
```cpp
struct LODRecommendation {
    int64_t timeframe_ms;      // 100ms, 250ms, 1000ms...
    Tick    priceResolution;   // 1-tick, 5-tick, 25-tick...  
    QString reasoning;         // For debugging
    
    // Ensure cell count within limits
    int estimatedCellCount() const {
        return (timeSpan_ms / timeframe_ms) * (priceSpan_ticks / priceResolution);
    }
};

LODRecommendation DataPipeline::calculateOptimalLOD(Viewport viewport, int maxCells) {
    // Consider BOTH dimensions together to prevent gaps
    auto timeSpan = viewport.timeEnd_ms - viewport.timeStart_ms;
    auto priceSpan = viewport.priceMax - viewport.priceMin;
    
    // Find finest LOD that keeps cell count < maxCells
    for (auto timeframe : {100, 250, 500, 1000, 2000, 5000}) {
        for (auto priceRes : {0.25, 0.5, 1.0, 5.0, 25.0}) {
            auto cellCount = (timeSpan / timeframe) * (priceSpan / priceRes);
            if (cellCount <= maxCells) {
                return {timeframe, priceToTick(priceRes), "Optimal balance"};
            }
        }
    }
}
```

### **4.2 Visual Rendering Coordination**
**Problem**: Rectangle size hardcoded instead of matching data resolution
```cpp
// Current: timeSliceToScreenRect() uses hardcoded $1 resolution
// Target: Dynamic rectangle size matching current LOD
```

**Fix:**
```cpp
QRectF CoordinateSystem::cellToScreenRect(CellInstance cell, LODRecommendation lod, Viewport viewport) {
    // Rectangle size MATCHES current data resolution
    double timeWidth = lod.timeframe_ms;
    double priceHeight = tickToPrice(lod.priceResolution);
    
    QPointF topLeft = worldToScreen(cell.timeStart, cell.price + priceHeight/2, viewport);
    QPointF bottomRight = worldToScreen(cell.timeEnd, cell.price - priceHeight/2, viewport);
    
    return QRectF(topLeft, bottomRight);  // Perfect Bookmap-style blocks!
}
```

---

## 📋 **IMPLEMENTATION EXECUTION PLAN**

### **Phase Dependencies:**
```
Phase 1 (Data Pipeline) → Phase 2 (LTSE Optimization) → Phase 3 (UGR Refactor) → Phase 4 (LOD Coordination)
```

### **Risk Mitigation:**
- ✅ **Incremental**: Each phase is independently testable
- ✅ **Rollback**: Git branches for each phase  
- ✅ **Validation**: Performance benchmarks after each phase
- ✅ **Fallback**: Keep existing code until new code proven working

### **Success Metrics:**
- **Phase 1**: Eliminate retry logic, reduce component count by 2
- **Phase 2**: 8× CPU performance improvement, stable FPS
- **Phase 3**: UGR <200 lines, clean responsibility separation  
- **Phase 4**: Visual gaps eliminated, coordinated LOD working

### **Testing Strategy:**
- **Unit Tests**: Each extracted component independently testable
- **Integration Tests**: End-to-end data pipeline validation
- **Performance Tests**: FPS benchmarks with 10K+ cells
- **Visual Tests**: Screenshot comparisons for regression detection

---

## 🎯 **POST-REFACTOR ARCHITECTURE**

### **Final Component Hierarchy:**
```
├── DataPipeline (owns LTSE + data processing)
├── RenderOrchestrator (owns strategies + GPU rendering)  
├── ViewportController (owns GridViewState + user interaction)
└── UnifiedGridRenderer (pure QML adapter - <200 lines)
```

### **Data Flow (Optimized):**
```
WebSocket → MarketDataCore → DataPipeline.process() → RenderOrchestrator.render() → GPU
                                    ↓
                           ViewportController ← Mouse/Touch Events ← QML
```

### **Performance Characteristics:**
- **FPS**: Stable 60+ at 10K+ cells
- **Memory**: ~200MB total (vs current ~400MB)
- **CPU**: Sub-millisecond aggregation (vs current 50ms+)
- **Latency**: Direct response to user input (no retry delays)

---

## 🚀 **EXECUTION NOTES**

### **For AI Implementation:**
- **Scope**: Each phase section is self-contained for AI handoff
- **Context**: Use Gemini 1M context window for cross-file analysis
- **Validation**: Build + test after each sub-section
- **Documentation**: Update this doc with actual implementation details

### **For Human Review:**
- **Progress Tracking**: Mark phases complete as implemented
- **Performance Validation**: Benchmark after each phase
- **Architecture Review**: Ensure clean separation maintained
- **Visual Testing**: Verify no regressions in chart appearance

**This refactor transforms Sentinel from "works by magic" to production-grade Bloomberg Terminal quality architecture!** 🎯