# Multi-Mode Chart Architecture Implementation Plan

**Date**: 2025-06-28  
**Context**: Resolving architectural timing mismatches and adding chart mode switching  
**Priority**: HIGH - Fixes fundamental rendering frequency issues  

## 🎯 EXECUTIVE SUMMARY

The current Sentinel GPU chart system has a **fundamental timing mismatch**: candlesticks are rendered at 60 FPS (16ms intervals) but represent 1-minute timeframes, meaning incomplete candles are rendered 3,750 times before completion. This plan implements:

1. **Multi-Mode Chart System** with toggleable visualization modes
2. **Proper Batching Integration** for candlesticks 
3. **High-Frequency Candles** (100ms, 500ms) that align with 16ms rendering
4. **Order Book Resampling** for temporal analysis

## 📊 CURRENT ARCHITECTURE STATE

### Multi-Layer Rendering Components
```
┌─ GPUChartWidget ──────────────────────────────┐ ✅ PROPERLY BATCHED
│  • Trade scatter points                       │    via GPUDataAdapter
│  • Real-time individual trades                │    16ms batching
│  • VBO triple buffering                       │
└────────────────────────────────────────────────┘

┌─ HeatmapInstanced ────────────────────────────┐ ✅ PROPERLY BATCHED  
│  • Order book depth visualization             │    via GPUDataAdapter
│  • Live L2 updates                            │    16ms batching
│  • Instanced quad rendering                   │
└────────────────────────────────────────────────┘

┌─ CandlestickBatched ──────────────────────────┐ ❌ BYPASSES BATCHING
│  • OHLC candle visualization                  │    Direct trade signals
│  • Multi-timeframe (1min, 5min, 15min, etc.) │    Immediate GPU updates
│  • LOD (Level of Detail) system               │    Timing mismatch!
└────────────────────────────────────────────────┘
```

### Current Data Flow Issues
```
WebSocket → MarketDataCore → GPUDataAdapter (16ms batching)
   ├─ Trade Queue → tradesReady signal → GPUChartWidget    ✅
   ├─ OrderBook Queue → heatmapReady signal → HeatmapInstanced  ✅  
   └─ BYPASS! StreamController → individual trades → CandlestickBatched  ❌
```

### Key Files and Locations

#### Core Components
- **`libs/gui/gpudataadapter.h/cpp`**: Batching pipeline (16ms timer)
- **`libs/gui/gpuchartwidget.h/cpp`**: Trade scatter chart (881 lines)
- **`libs/gui/heatmapinstanced.h/cpp`**: Order book heatmap (512 lines)
- **`libs/gui/candlestickbatched.h/cpp`**: OHLC candles (614 lines)

#### Integration Points
- **`libs/gui/MainWindowGpu.h/cpp`**: Component wiring (297 lines)
- **`libs/gui/StreamController.h/cpp`**: Data bridge (136 lines)
- **`libs/gui/qml/DepthChartView.qml`**: Main QML view (328 lines)
- **`libs/gui/qml/CandleChartView.qml`**: Candle QML wrapper (102 lines)

#### Supporting Architecture
- **`libs/gui/candlelod.h/cpp`**: Multi-timeframe candle management (241 lines)
- **`libs/core/TradeData.h`**: Trade/OrderBook data structures
- **`docs/02_ARCHITECTURE.md`**: Current architectural documentation

## 🚨 IDENTIFIED PROBLEMS

### 1. Timing Frequency Mismatch
```cpp
// Current Reality:
16ms rendering frequency = 62.5 FPS
1-minute candle frequency = 0.0167 FPS (once per 60 seconds)
Ratio: 1 complete candle per 3,750 render frames

// Problem: Drawing incomplete candles 3,749 times out of 3,750!
```

### 2. Bypassed Batching Architecture
```cpp
// In candlestickbatched.cpp:44
void CandlestickBatched::addTrade(const Trade& trade) {
    // ... process trade ...
    update(); // 🚨 IMMEDIATE GPU UPDATE - BYPASSES BATCHING!
}
```

### 3. Missing Mode Switching
- No UI controls to switch between visualization modes
- All three rendering layers always active simultaneously
- No way to toggle between trade scatter vs. candles vs. order book

### 4. Hard-Coded Connection Pattern
```cpp
// In mainwindow_gpu.cpp:240 - Direct individual trade signals
connect(m_streamController, &StreamController::tradeReceived,
        candleChart, &CandlestickBatched::onTradeReceived,
        Qt::QueuedConnection);
```

## 🎯 PROPOSED SOLUTION: MULTI-MODE ARCHITECTURE

### Vision: Chart Mode System
```
┌─ MODE 1: TRADE SCATTER ───────────────────────┐
│  • Individual trade points                    │
│  • Real-time price action                     │
│  • Best for: Tick analysis, execution timing  │
└────────────────────────────────────────────────┘

┌─ MODE 2: HIGH-FREQ CANDLES ───────────────────┐
│  • 100ms, 500ms, 1s candles                   │
│  • Progressive candle building                │
│  • Best for: Short-term patterns, scalping    │
└────────────────────────────────────────────────┘

┌─ MODE 3: TRADITIONAL CANDLES ─────────────────┐
│  • 1min, 5min, 15min candles                  │
│  • Time-based updates only                    │
│  • Best for: Swing trading, trend analysis    │
└────────────────────────────────────────────────┘

┌─ MODE 4: ORDER BOOK FOCUS ────────────────────┐
│  • Dense liquidity heatmap                    │
│  • Order book resampling                      │
│  • Best for: Market depth, support/resistance │
└────────────────────────────────────────────────┘

┌─ MODE 5: HYBRID MODES ────────────────────────┐
│  • Candles + Trade overlay                    │
│  • Order book + Price action                  │
│  • Multi-timeframe candles                    │
└────────────────────────────────────────────────┘
```

## 🔧 IMPLEMENTATION PLAN

### Phase 1: Candle Batching Integration (CRITICAL)

**Goal**: Fix the immediate GPU update problem by integrating candlesticks into the batching pipeline.

#### 1.1 Modify GPUDataAdapter to Support Candle Data
```cpp
// Add to libs/gui/gpudataadapter.h
struct CandleUpdate {
    std::string symbol;
    int64_t timestamp_ms;
    CandleLOD::TimeFrame timeframe;
    OHLC candle;
    bool isNewCandle;
};

class GPUDataAdapter : public QObject {
    // ... existing code ...
    
signals:
    void candlesReady(const std::vector<CandleUpdate>& candles);
    
private:
    CandleQueue m_candleQueue;  // New lock-free queue
    std::vector<CandleUpdate> m_candleBuffer;
    size_t m_candleWriteCursor = 0;
};
```

#### 1.2 Update GPUDataAdapter::processIncomingData()
```cpp
// Add candle processing to 16ms batching loop
void GPUDataAdapter::processIncomingData() {
    // ... existing trade/orderbook processing ...
    
    // 🔥 NEW: Process candle updates
    CandleUpdate candleUpdate;
    m_candleWriteCursor = 0;
    
    while (m_candleQueue.pop(candleUpdate) && m_candleWriteCursor < m_reserveSize) {
        m_candleBuffer[m_candleWriteCursor++] = candleUpdate;
    }
    
    if (m_candleWriteCursor > 0) {
        emit candlesReady(m_candleBuffer.data(), m_candleWriteCursor);
    }
}
```

#### 1.3 Modify CandlestickBatched Connection
```cpp
// Change from individual trades to batched candles
// REMOVE in mainwindow_gpu.cpp:
connect(m_streamController, &StreamController::tradeReceived,
        candleChart, &CandlestickBatched::onTradeReceived);

// ADD in mainwindow_gpu.cpp:
connect(gpuDataAdapter, &GPUDataAdapter::candlesReady,
        candleChart, &CandlestickBatched::onCandlesReady);
```

#### 1.4 Update Trade-to-Candle Conversion
```cpp
// Move candle generation from CandlestickBatched to GPUDataAdapter
// This ensures candles are built in the batching pipeline, not in the renderer
```

### Phase 2: High-Frequency Candle Support

**Goal**: Add 100ms, 500ms, 1s candles that work well with 16ms rendering.

#### 2.1 Extend CandleLOD TimeFrame Enum
```cpp
// In libs/gui/candlelod.h
enum TimeFrame {
    TF_100ms = 0,    // 🔥 NEW: 100ms candles
    TF_500ms = 1,    // 🔥 NEW: 500ms candles  
    TF_1sec = 2,     // 🔥 NEW: 1 second candles
    TF_1min = 3,     // Existing
    TF_5min = 4,     // Existing
    TF_15min = 5,    // Existing
    TF_60min = 6,    // Existing
    TF_Daily = 7     // Existing
};

const int64_t TIMEFRAME_INTERVALS[] = {
    100,      // 100ms
    500,      // 500ms  
    1000,     // 1 second
    60000,    // 1 minute
    300000,   // 5 minutes
    900000,   // 15 minutes
    3600000,  // 1 hour
    86400000  // 1 day
};
```

#### 2.2 Update Candle Generation Logic
```cpp
// High-frequency candles get updated multiple times per completion
// 100ms candle with 16ms rendering = ~6 updates per candle
// This creates smooth "progressive candle building" effect
```

### Phase 3: Chart Mode Switching System

**Goal**: Add UI controls and backend logic for switching between chart modes.

#### 3.1 Add Mode Enumeration
```cpp
// In libs/gui/chartmode.h (NEW FILE)
enum class ChartMode {
    TRADE_SCATTER,           // Individual trade points
    HIGH_FREQ_CANDLES,       // 100ms-1s candles
    TRADITIONAL_CANDLES,     // 1min+ candles  
    ORDER_BOOK_HEATMAP,      // Dense liquidity view
    HYBRID_CANDLES_TRADES,   // Candles + trade overlay
    HYBRID_DEPTH_PRICE,      // Order book + price action
    MULTI_TIMEFRAME          // Multiple candle timeframes
};
```

#### 3.2 Add Mode Controller
```cpp
// In libs/gui/ChartModeController.h/cpp (NEW FILES)
class ChartModeController : public QObject {
    Q_OBJECT
    
public:
    void setMode(ChartMode mode);
    ChartMode getCurrentMode() const { return m_currentMode; }
    
signals:
    void modeChanged(ChartMode newMode);
    void componentVisibilityChanged(const QString& component, bool visible);
    
private:
    ChartMode m_currentMode = ChartMode::TRADE_SCATTER;
    void updateComponentVisibility();
};
```

#### 3.3 Update QML UI with Mode Controls
```qml
// Add to libs/gui/qml/DepthChartView.qml
Row {
    id: modeControls
    spacing: 10
    
    Button {
        text: "Trades"
        checked: chartModeController.currentMode === ChartMode.TRADE_SCATTER
        onClicked: chartModeController.setMode(ChartMode.TRADE_SCATTER)
    }
    
    Button {
        text: "Fast Candles"  
        checked: chartModeController.currentMode === ChartMode.HIGH_FREQ_CANDLES
        onClicked: chartModeController.setMode(ChartMode.HIGH_FREQ_CANDLES)
    }
    
    Button {
        text: "Traditional"
        checked: chartModeController.currentMode === ChartMode.TRADITIONAL_CANDLES  
        onClicked: chartModeController.setMode(ChartMode.TRADITIONAL_CANDLES)
    }
    
    Button {
        text: "Order Book"
        checked: chartModeController.currentMode === ChartMode.ORDER_BOOK_HEATMAP
        onClicked: chartModeController.setMode(ChartMode.ORDER_BOOK_HEATMAP)
    }
}
```

### Phase 4: Order Book Resampling

**Goal**: Add temporal order book analysis for aggregated liquidity views.

#### 4.1 Create Order Book Sampler
```cpp
// In libs/core/orderbooksampler.h/cpp (NEW FILES)
struct OrderBookSample {
    int64_t timestamp_ms;
    double weightedBidPrice;    // Volume-weighted average
    double weightedAskPrice;
    double totalBidVolume;      // Aggregated over time bucket
    double totalAskVolume;
    double averageSpread;       // Average spread in bucket
    size_t updateCount;         // Number of L2 updates in bucket
};

class OrderBookSampler {
public:
    void addOrderBook(const OrderBook& book);
    std::vector<OrderBookSample> getSamples(int64_t timeframe_ms);
    void setResolution(int64_t resolution_ms) { m_resolution = resolution_ms; }
    
private:
    int64_t m_resolution = 1000; // Default 1s buckets
    std::vector<OrderBookSample> m_samples;
};
```

#### 4.2 Integrate with Batching Pipeline
```cpp
// Add to GPUDataAdapter
void processOrderBookForResampling(const OrderBook& book) {
    m_orderBookSampler.addOrderBook(book);
    
    // Every 16ms, emit resampled data if available
    auto samples = m_orderBookSampler.getSamples(m_resamplingTimeframe);
    if (!samples.empty()) {
        emit orderBookSamplesReady(samples);
    }
}
```

### Phase 5: Performance Optimization

**Goal**: Ensure the multi-mode system maintains sub-millisecond performance.

#### 5.1 Conditional Rendering
```cpp
// Only render active components based on current mode
// Disable unused GPU components to save VRAM and draw calls
```

#### 5.2 GPU Memory Management
```cpp
// Implement GPU buffer switching when modes change
// Pre-allocate buffers for each mode to avoid runtime allocation
```

#### 5.3 Update Frequency Optimization
```cpp
// High-frequency candles: Update on every batch (16ms)
// Traditional candles: Update only when candle period completes
// Order book heatmap: Update on every L2 change
// Trade scatter: Update on every trade batch
```

## 📁 FILE MODIFICATION CHECKLIST

### New Files to Create
- [ ] `libs/gui/chartmode.h` - Mode enumeration and constants
- [ ] `libs/gui/ChartModeController.h/cpp` - Mode switching logic
- [ ] `libs/core/orderbooksampler.h/cpp` - Temporal order book analysis
- [ ] `docs/features/gpu_chart_rendering/multi_mode_architecture.md` - Documentation

### Existing Files to Modify

#### Critical Changes (Phase 1)
- [ ] `libs/gui/gpudataadapter.h/cpp` - Add candle batching support
- [ ] `libs/gui/candlestickbatched.h/cpp` - Remove immediate updates, add batch processing  
- [ ] `libs/gui/mainwindow_gpu.cpp` - Change candle connection pattern
- [ ] `libs/gui/candlelod.h/cpp` - Add high-frequency timeframes

#### UI Changes (Phase 3)  
- [ ] `libs/gui/qml/DepthChartView.qml` - Add mode switching controls
- [ ] `libs/gui/qml/CandleChartView.qml` - Add mode-aware visibility
- [ ] `libs/gui/MainWindowGpu.h/cpp` - Integrate ChartModeController

#### Build System
- [ ] `libs/gui/CMakeLists.txt` - Add new source files
- [ ] `libs/core/CMakeLists.txt` - Add OrderBookSampler if placed in core

## 🧪 TESTING STRATEGY

### Unit Tests
- [ ] Test candle batching performance vs. immediate updates
- [ ] Test high-frequency candle generation accuracy
- [ ] Test mode switching without memory leaks
- [ ] Test order book resampling correctness

### Integration Tests  
- [ ] Test full data pipeline: WebSocket → Batching → Rendering
- [ ] Test mode switching during live data streaming
- [ ] Test coordinate synchronization across all modes
- [ ] Test GPU memory usage across mode switches

### Performance Tests
- [ ] Measure rendering frequency consistency in each mode
- [ ] Measure GPU memory usage per mode  
- [ ] Measure CPU usage during mode switches
- [ ] Stress test with high-frequency data in each mode

## 🚀 ROLLOUT STRATEGY

### Phase 1: Critical Fix (Week 1)
- Fix immediate candle batching issue
- Maintain current functionality while fixing performance

### Phase 2: High-Frequency Support (Week 2)  
- Add 100ms-1s candle support
- Test progressive candle building

### Phase 3: Mode Switching (Week 3)
- Add UI controls and mode logic
- Implement component visibility toggling

### Phase 4: Advanced Features (Week 4)
- Order book resampling  
- Hybrid modes
- Performance optimization

### Phase 5: Polish & Documentation (Week 5)
- Comprehensive testing
- Performance tuning
- User documentation

## 🎯 SUCCESS METRICS

### Performance Targets
- [ ] Maintain <1ms average latency across all modes
- [ ] Consistent 60 FPS rendering in all modes
- [ ] <100MB GPU memory usage per mode
- [ ] <5ms mode switching time

### Functionality Targets  
- [ ] 7 distinct chart modes working flawlessly
- [ ] Smooth progressive candle building for high-frequency timeframes
- [ ] Accurate order book temporal analysis
- [ ] Intuitive mode switching UI

### Code Quality Targets
- [ ] Zero memory leaks during mode switching
- [ ] 100% test coverage for new components
- [ ] Clean separation of concerns between modes
- [ ] Maintainable, documented codebase

## 🔄 NEXT STEPS FOR IMPLEMENTATION AGENT

1. **Read this entire plan** to understand the architectural vision
2. **Start with Phase 1** - Fix the immediate candle batching issue first
3. **Test each phase thoroughly** before moving to the next
4. **Follow the file modification checklist** systematically
5. **Run performance tests** after each major change
6. **Document any deviations** from this plan with reasoning

This plan provides a roadmap from the current problematic architecture to a robust multi-mode chart system that properly handles different rendering frequencies and visualization needs. 