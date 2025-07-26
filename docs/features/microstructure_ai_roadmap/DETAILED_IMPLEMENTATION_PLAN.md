# Sentinel Microstructure & AI Implementation Plan (2025-2026)
## Detailed Codebase Integration Guide

*Owner*: Patrick (Lead Dev)  
*Co-author*: Claude Implementation Agent  
*Status*: Ready for Implementation

> This document provides **step-by-step implementation instructions** for each phase, tailored specifically to Sentinel's current architecture. Each phase is self-contained and builds upon the existing `LiquidityTimeSeriesEngine` and `UnifiedGridRenderer` foundation.

---

## 🎯 **Implementation Difficulty Ranking**

| Phase | Difficulty | Time Estimate | Dependencies | Key Files |
|-------|------------|---------------|--------------|-----------|
| **Phase 1** | 🟢 **EASY** | 1 week | None | `LiquidityTimeSeriesEngine.{h,cpp}` |
| **Phase 2** | 🟡 **MEDIUM** | 1 week | Phase 1 | `DataCache.cpp`, `TradeData.h` |
| **Phase 3** | 🟡 **MEDIUM** | 1 week | Phase 1 | `LiquidityTimeSeriesEngine.cpp` |
| **Phase 4** | 🔴 **HARD** | 1.5 weeks | Phases 1-3 | New: `MomentumEngine.{h,cpp}` |
| **Phase 5** | 🟢 **EASY** | 1 week | None | `UnifiedGridRenderer.{h,cpp}` |
| **Phase 6** | 🟡 **MEDIUM** | 2 weeks | Phase 5 | `UnifiedGridRenderer.cpp` |
| **Phase 7** | 🔴 **VERY HARD** | 2 weeks | None | New: `CopeNetService.{h,cpp}` |
| **Phase 8** | 🟡 **MEDIUM** | 0.5 weeks | Phase 7 | New: `AlertManager.{h,cpp}` |
| **Phase 9** | 🟢 **EASY** | 1 week | None | QML files, `MainWindowGpu.cpp` |

---

## 🟢 **PHASE 1: Spoof-Pull Detection (EASY)**

### **Current State Analysis**
Your `LiquidityTimeSeriesEngine` already has:
- ✅ `firstSeen_ms` and `lastSeen_ms` tracking
- ✅ `persistenceRatio()` calculation
- ✅ `finalizeLiquiditySlice()` method
- ✅ Qt signal infrastructure

### **Implementation Steps**

#### **Step 1.1: Extend PriceLevelMetrics**
```cpp
// In libs/gui/LiquidityTimeSeriesEngine.h, add to PriceLevelMetrics struct:
struct PriceLevelMetrics {
    // ... existing fields ...
    double sizeAtFirstSeen = 0.0;    // 🚧 SP1: Add this field
    double sizeAtLastSeen = 0.0;     // Track size changes
    double pulledLiquidity = 0.0;    // Calculated: first - last
    bool spoofDetected = false;      // Flag for GUI rendering
};
```

#### **Step 1.2: Create SpoofEvent Structure**
```cpp
// Add to libs/gui/LiquidityTimeSeriesEngine.h:
struct SpoofEvent {
    int64_t timestamp_ms;
    double price;
    double pulledLiquidity;
    double persistenceRatio;
    bool isBid;
    double confidence;  // 0.0-1.0 based on thresholds
};

// Add to LiquidityTimeSeriesEngine class:
signals:
    void spoofEventDetected(const SpoofEvent& event);  // 🚧 SP3: New signal
```

#### **Step 1.3: Implement Detection Algorithm**
```cpp
// In libs/gui/LiquidityTimeSeriesEngine.cpp, modify updatePriceLevelMetrics():
void LiquidityTimeSeriesEngine::updatePriceLevelMetrics(
    LiquidityTimeSlice::PriceLevelMetrics& metrics, 
    double liquidity, 
    int64_t timestamp_ms,
    const LiquidityTimeSlice& slice) {
    
    if (metrics.snapshotCount == 0) {
        metrics.firstSeen_ms = timestamp_ms;
        metrics.sizeAtFirstSeen = liquidity;  // 🚧 SP1: Track initial size
        metrics.minLiquidity = liquidity;
    }
    
    // ... existing logic ...
    
    metrics.sizeAtLastSeen = liquidity;  // Track current size
    metrics.lastSeen_ms = timestamp_ms;
}

// Add new method to detect spoofing:
void LiquidityTimeSeriesEngine::detectSpoofEvents(LiquidityTimeSlice& slice) {
    const double SPOOF_THRESHOLD = 1000.0;  // Configurable
    const double PERSISTENCE_THRESHOLD = 0.3;
    
    // Check bid levels for spoofing
    for (auto& [price, metrics] : slice.bidMetrics) {
        if (metrics.snapshotCount > 2) {  // Minimum observations
            metrics.pulledLiquidity = metrics.sizeAtFirstSeen - metrics.sizeAtLastSeen;
            double persistence = metrics.persistenceRatio(slice.duration_ms);
            
            if (metrics.pulledLiquidity > SPOOF_THRESHOLD && 
                persistence < PERSISTENCE_THRESHOLD) {
                
                SpoofEvent event;
                event.timestamp_ms = slice.endTime_ms;
                event.price = price;
                event.pulledLiquidity = metrics.pulledLiquidity;
                event.persistenceRatio = persistence;
                event.isBid = true;
                event.confidence = std::min(metrics.pulledLiquidity / SPOOF_THRESHOLD, 1.0);
                
                metrics.spoofDetected = true;
                emit spoofEventDetected(event);
            }
        }
    }
    
    // Similar logic for ask levels...
}
```

#### **Step 1.4: Integrate Detection into Pipeline**
```cpp
// In finalizeLiquiditySlice(), add at the end:
void LiquidityTimeSeriesEngine::finalizeLiquiditySlice(LiquidityTimeSlice& slice) {
    // ... existing finalization logic ...
    
    // 🚧 SP2: Add spoof detection
    detectSpoofEvents(slice);
}
```

#### **Step 1.5: GUI Integration**
```cpp
// In libs/gui/UnifiedGridRenderer.cpp, add spoof event handling:
void UnifiedGridRenderer::onSpoofEventDetected(const SpoofEvent& event) {
    // Create visual indicator
    CellInstance spoofCell;
    spoofCell.screenRect = timePriceToScreenRect(event.timestamp_ms, event.price);
    spoofCell.color = QColor(255, 165, 0, 200);  // Orange warning color
    spoofCell.intensity = event.confidence;
    spoofCell.isBid = event.isBid;
    
    // Add to special spoof layer
    m_spoofCells.push_back(spoofCell);
    m_geometryDirty.store(true);
    update();
}

// In createHeatmapNode(), add spoof cell rendering:
QSGNode* UnifiedGridRenderer::createHeatmapNode(const std::vector<CellInstance>& cells) {
    // ... existing heatmap rendering ...
    
    // 🎨 SP6: Add spoof warning icons
    for (const auto& spoofCell : m_spoofCells) {
        // Render warning triangle or icon at spoof location
        // Use QSGGeometry for triangle rendering
    }
}
```

#### **Step 1.6: Unit Testing**
```cpp
// Create tests/test_spoof_detection.cpp:
TEST_F(SpoofDetectionTest, SyntheticSpoofEvent) {
    auto engine = std::make_unique<LiquidityTimeSeriesEngine>();
    
    // Create synthetic order book with spoofing pattern
    OrderBook book;
    book.product_id = "BTC-USD";
    book.bids.push_back({50000.0, 5000.0});  // Large initial liquidity
    
    // Add snapshots with liquidity pull
    engine->addOrderBookSnapshot(book);
    
    // Modify book to show liquidity pull
    book.bids[0].size = 100.0;  // Dramatic reduction
    engine->addOrderBookSnapshot(book);
    
    // Verify spoof event was detected
    QSignalSpy spy(engine.get(), &LiquidityTimeSeriesEngine::spoofEventDetected);
    EXPECT_GT(spy.count(), 0);
}
```

### **Acceptance Criteria**
- ✅ Unit test passes with synthetic spoof data
- ✅ Orange warning icons appear on live feed during spoofing
- ✅ CPU usage < 15ms @ 10k msg/s on M1 Pro
- ✅ Configuration thresholds exposed in settings

---

## 🟡 **PHASE 2: Iceberg Order Detection (MEDIUM)**

### **Current State Analysis**
Your `DataCache` has:
- ✅ `TradeRing` for trade buffering
- ✅ `LiveOrderBook` for current state
- ✅ Trade aggregation capabilities

### **Implementation Steps**

#### **Step 2.1: Extend DataCache for Iceberg Detection**
```cpp
// In libs/core/DataCache.cpp, add iceberg tracking:
struct IcebergTracker {
    std::map<double, double> cumulativeTradeVolume;  // price -> total volume
    std::map<double, int64_t> lastTradeTime;         // price -> last trade timestamp
    std::map<double, int> tradeCount;                // price -> number of trades
    
    void addTrade(double price, double size, int64_t timestamp) {
        cumulativeTradeVolume[price] += size;
        lastTradeTime[price] = timestamp;
        tradeCount[price]++;
    }
    
    double getCumulativeVolume(double price) const {
        auto it = cumulativeTradeVolume.find(price);
        return it != cumulativeTradeVolume.end() ? it->second : 0.0;
    }
};

// Add to DataCache class:
private:
    IcebergTracker m_icebergTracker;
    std::deque<IcebergEvent> m_icebergEvents;
```

#### **Step 2.2: Create IcebergEvent Structure**
```cpp
// In libs/core/TradeData.h, add:
struct IcebergEvent {
    int64_t timestamp_ms;
    double price;
    double revealedSize;      // Cumulative trade volume
    double visibleLiquidity;  // Current book size at price
    double hiddenSize;        // Inferred hidden size
    AggressorSide aggressorSide;
    double confidence;        // 0.0-1.0 based on pattern strength
    int tradeCount;          // Number of small trades
};
```

#### **Step 2.3: Implement Iceberg Detection Algorithm**
```cpp
// In libs/core/DataCache.cpp, add to addTrade():
void DataCache::addTrade(const Trade& trade) {
    // ... existing trade processing ...
    
    // 🚧 ICEBERG DETECTION: Track cumulative volume per price
    m_icebergTracker.addTrade(trade.price, trade.size, 
        std::chrono::duration_cast<std::chrono::milliseconds>(
            trade.timestamp.time_since_epoch()).count());
    
    // Check for iceberg pattern
    checkForIcebergOrder(trade);
}

void DataCache::checkForIcebergOrder(const Trade& trade) {
    const double ICEBERG_THRESHOLD = 0.8;  // 80% of visible liquidity
    const int MIN_TRADE_COUNT = 5;         // Minimum small trades
    const int64_t TIME_WINDOW_MS = 30000;  // 30 second window
    
    double price = trade.price;
    double cumulativeVolume = m_icebergTracker.getCumulativeVolume(price);
    int tradeCount = m_icebergTracker.tradeCount[price];
    
    // Get current visible liquidity at this price
    double visibleLiquidity = 0.0;
    auto liveBook = getLiveOrderBook(trade.product_id);
    
    for (const auto& level : liveBook.bids) {
        if (std::abs(level.price - price) < 0.01) {
            visibleLiquidity = level.size;
            break;
        }
    }
    for (const auto& level : liveBook.asks) {
        if (std::abs(level.price - price) < 0.01) {
            visibleLiquidity = level.size;
            break;
        }
    }
    
    // Detect iceberg if cumulative volume >> visible liquidity
    if (cumulativeVolume > visibleLiquidity * ICEBERG_THRESHOLD && 
        tradeCount >= MIN_TRADE_COUNT) {
        
        IcebergEvent event;
        event.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            trade.timestamp.time_since_epoch()).count();
        event.price = price;
        event.revealedSize = cumulativeVolume;
        event.visibleLiquidity = visibleLiquidity;
        event.hiddenSize = cumulativeVolume - visibleLiquidity;
        event.aggressorSide = trade.side;
        event.confidence = std::min(cumulativeVolume / visibleLiquidity, 1.0);
        event.tradeCount = tradeCount;
        
        m_icebergEvents.push_back(event);
        
        // Emit signal for GUI
        emit icebergEventDetected(event);
    }
}
```

#### **Step 2.4: GUI Integration**
```cpp
// In libs/gui/UnifiedGridRenderer.cpp, add iceberg rendering:
void UnifiedGridRenderer::onIcebergEventDetected(const IcebergEvent& event) {
    // Create iceberg indicator
    CellInstance icebergCell;
    icebergCell.screenRect = timePriceToScreenRect(event.timestamp_ms, event.price);
    icebergCell.color = QColor(0, 191, 255, 180);  // Deep sky blue
    icebergCell.intensity = event.confidence;
    icebergCell.isBid = (event.aggressorSide == AggressorSide::Buy);
    
    // Add size overlay text
    QString sizeText = QString("🧊 %1").arg(event.hiddenSize, 0, 'f', 2);
    
    m_icebergCells.push_back({icebergCell, sizeText});
    m_geometryDirty.store(true);
    update();
}
```

### **Acceptance Criteria**
- ✅ Detects repetitive small trades at same price level
- ✅ Blue iceberg icons appear with size overlays
- ✅ Confidence calculation based on volume ratio
- ✅ Performance impact < 5% CPU overhead

---

## 🟡 **PHASE 3: Absorption Zones (MEDIUM)**

### **Implementation Steps**

#### **Step 3.1: Extend LiquidityTimeSeriesEngine**
```cpp
// In libs/gui/LiquidityTimeSeriesEngine.h, add:
struct AbsorptionZone {
    int64_t startTime_ms;
    int64_t endTime_ms;
    double price;
    double absorbedVolume;
    double midPriceChange;
    double persistenceRatio;
    bool isActive;
};

// Add to LiquidityTimeSeriesEngine class:
private:
    std::vector<AbsorptionZone> m_activeAbsorptionZones;
    std::deque<AbsorptionZone> m_completedAbsorptionZones;

signals:
    void absorptionZoneDetected(const AbsorptionZone& zone);
    void absorptionZoneCompleted(const AbsorptionZone& zone);
```

#### **Step 3.2: Implement Absorption Detection**
```cpp
// In libs/gui/LiquidityTimeSeriesEngine.cpp:
void LiquidityTimeSeriesEngine::detectAbsorptionZones(const LiquidityTimeSlice& slice) {
    const double ABSORPTION_THRESHOLD = 0.8;  // 80% persistence
    const double MAX_MID_PRICE_CHANGE = 0.001; // 0.1% max change
    
    for (const auto& [price, metrics] : slice.bidMetrics) {
        if (metrics.persistenceRatio(slice.duration_ms) > ABSORPTION_THRESHOLD) {
            // Check if this is an absorption zone
            double midPriceChange = calculateMidPriceChange(slice);
            
            if (std::abs(midPriceChange) < MAX_MID_PRICE_CHANGE) {
                // This is an absorption zone
                AbsorptionZone zone;
                zone.startTime_ms = slice.startTime_ms;
                zone.endTime_ms = slice.endTime_ms;
                zone.price = price;
                zone.absorbedVolume = metrics.totalLiquidity;
                zone.midPriceChange = midPriceChange;
                zone.persistenceRatio = metrics.persistenceRatio(slice.duration_ms);
                zone.isActive = true;
                
                m_activeAbsorptionZones.push_back(zone);
                emit absorptionZoneDetected(zone);
            }
        }
    }
}
```

#### **Step 3.3: GUI Rendering**
```cpp
// In libs/gui/UnifiedGridRenderer.cpp:
void UnifiedGridRenderer::renderAbsorptionZones() {
    for (const auto& zone : m_activeAbsorptionZones) {
        // Create semi-transparent purple rectangle
        QRectF zoneRect = timePriceRangeToScreenRect(
            zone.startTime_ms, zone.endTime_ms,
            zone.price - 1.0, zone.price + 1.0);
        
        // Add to absorption layer
        m_absorptionZones.push_back({zoneRect, zone});
    }
}
```

---

## 🔴 **PHASE 4: Momentum-Ignition Chain (HARD)**

### **Implementation Steps**

#### **Step 4.1: Create Momentum Engine**
```cpp
// New file: libs/core/MomentumEngine.h
class MomentumEngine : public QObject {
    Q_OBJECT
    
public:
    enum class IgnitionState {
        Idle,
        LiquidityPullDetected,
        SpreadWidening,
        MomentumBurst,
        IgnitionComplete
    };
    
private:
    IgnitionState m_currentState = IgnitionState::Idle;
    std::deque<SpoofEvent> m_recentSpoofs;
    std::deque<AbsorptionZone> m_recentAbsorptions;
    std::deque<IcebergEvent> m_recentIcebergs;
    
    // State machine parameters
    int64_t m_stateStartTime_ms = 0;
    double m_spreadAtStart = 0.0;
    double m_momentumThreshold = 1000.0;
    
signals:
    void ignitionAlert(const QString& message, double confidence);
};
```

#### **Step 4.2: Implement State Machine**
```cpp
// In libs/core/MomentumEngine.cpp:
void MomentumEngine::onSpoofEventDetected(const SpoofEvent& event) {
    m_recentSpoofs.push_back(event);
    
    // Clean old events (keep last 60 seconds)
    int64_t cutoffTime = event.timestamp_ms - 60000;
    while (!m_recentSpoofs.empty() && m_recentSpoofs.front().timestamp_ms < cutoffTime) {
        m_recentSpoofs.pop_front();
    }
    
    // Check for ignition pattern
    checkIgnitionPattern();
}

void MomentumEngine::checkIgnitionPattern() {
    switch (m_currentState) {
        case IgnitionState::Idle:
            if (hasSignificantLiquidityPull()) {
                m_currentState = IgnitionState::LiquidityPullDetected;
                m_stateStartTime_ms = getCurrentTimeMs();
                m_spreadAtStart = getCurrentSpread();
            }
            break;
            
        case IgnitionState::LiquidityPullDetected:
            if (hasSpreadWidening()) {
                m_currentState = IgnitionState::SpreadWidening;
            } else if (getCurrentTimeMs() - m_stateStartTime_ms > 5000) {
                // Timeout - reset to idle
                m_currentState = IgnitionState::Idle;
            }
            break;
            
        case IgnitionState::SpreadWidening:
            if (hasMomentumBurst()) {
                m_currentState = IgnitionState::MomentumBurst;
                emit ignitionAlert("Momentum ignition detected!", 0.85);
            }
            break;
    }
}
```

---

## 🟢 **PHASE 5: Dynamic Grid Controls (EASY)**

### **Implementation Steps**

#### **Step 5.1: Add Runtime Controls**
```cpp
// In libs/gui/UnifiedGridRenderer.h, add properties:
Q_PROPERTY(double priceResolution READ priceResolution WRITE setPriceResolution NOTIFY priceResolutionChanged)
Q_PROPERTY(QStringList availableTimeframes READ availableTimeframes NOTIFY timeframesChanged)
Q_PROPERTY(double volumeFilterMin READ volumeFilterMin WRITE setVolumeFilterMin NOTIFY volumeFilterChanged)
Q_PROPERTY(double volumeFilterMax READ volumeFilterMax WRITE setVolumeFilterMax NOTIFY volumeFilterChanged)
Q_PROPERTY(bool logScaling READ logScaling WRITE setLogScaling NOTIFY logScalingChanged)
```

#### **Step 5.2: Create GridControls.qml**
```qml
// New file: libs/gui/qml/GridControls.qml
import QtQuick 2.15
import QtQuick.Controls 2.15

Rectangle {
    id: gridControls
    width: 300
    height: 400
    color: "#2b2b2b"
    
    Column {
        spacing: 10
        padding: 15
        
        Text {
            text: "Grid Controls"
            color: "white"
            font.pixelSize: 16
            font.bold: true
        }
        
        // Price Resolution Slider
        Text { text: "Price Resolution"; color: "white" }
        Slider {
            from: 0.01
            to: 10.0
            value: gridRenderer.priceResolution
            onValueChanged: gridRenderer.priceResolution = value
        }
        
        // Timeframe Selection
        Text { text: "Timeframe"; color: "white" }
        ComboBox {
            model: gridRenderer.availableTimeframes
            onCurrentTextChanged: gridRenderer.setTimeframe(parseInt(currentText))
        }
        
        // Volume Filters
        Text { text: "Volume Filter"; color: "white" }
        Row {
            Slider {
                from: 0
                to: 10000
                value: gridRenderer.volumeFilterMin
                onValueChanged: gridRenderer.volumeFilterMin = value
            }
            Slider {
                from: 0
                to: 10000
                value: gridRenderer.volumeFilterMax
                onValueChanged: gridRenderer.volumeFilterMax = value
            }
        }
    }
}
```

---

## 🔴 **PHASE 7: CopeNet v1 (VERY HARD)**

### **Implementation Steps**

#### **Step 7.1: Create CopeNet Service**
```cpp
// New file: libs/core/CopeNetService.h
class CopeNetService : public QObject {
    Q_OBJECT
    
public:
    struct MarketSnapshot {
        std::vector<SpoofEvent> recentSpoofs;
        std::vector<IcebergEvent> recentIcebergs;
        std::vector<AbsorptionZone> activeAbsorptions;
        LiquidityTimeSlice currentSlice;
        double riskScore;
        QString analysis;
    };
    
private:
    QNetworkAccessManager* m_networkManager;
    QString m_apiKey;
    QString m_apiEndpoint;
    
    // Local fallback
    std::unique_ptr<LocalGPT> m_localGPT;
    
signals:
    void analysisReady(const MarketSnapshot& snapshot);
    void errorOccurred(const QString& error);
};
```

#### **Step 7.2: Implement API Integration**
```cpp
// In libs/core/CopeNetService.cpp:
void CopeNetService::analyzeMarketData() {
    // Prepare market snapshot
    MarketSnapshot snapshot;
    snapshot.recentSpoofs = getRecentSpoofs(60);  // Last 60 seconds
    snapshot.recentIcebergs = getRecentIcebergs(60);
    snapshot.activeAbsorptions = getActiveAbsorptions();
    snapshot.currentSlice = getCurrentLiquiditySlice();
    
    // Create JSON payload
    QJsonObject payload;
    payload["timestamp"] = QDateTime::currentMSecsSinceEpoch();
    payload["market_data"] = serializeMarketSnapshot(snapshot);
    payload["signature"] = createHMACSignature(payload);
    
    // Send to CopeNet API
    QNetworkRequest request(QUrl(m_apiEndpoint));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Authorization", "Bearer " + m_apiKey.toUtf8());
    
    QNetworkReply* reply = m_networkManager->post(request, QJsonDocument(payload).toJson());
    
    connect(reply, &QNetworkReply::finished, [this, reply]() {
        if (reply->error() == QNetworkReply::NoError) {
            processAnalysisResponse(reply->readAll());
        } else {
            // Fallback to local analysis
            performLocalAnalysis();
        }
        reply->deleteLater();
    });
}
```

#### **Step 7.3: Local Fallback Implementation**
```cpp
// In libs/core/CopeNetService.cpp:
void CopeNetService::performLocalAnalysis() {
    MarketSnapshot snapshot = getCurrentMarketSnapshot();
    
    // Simple rule-based analysis
    double riskScore = 0.0;
    QString analysis;
    
    // Spoofing risk
    if (!snapshot.recentSpoofs.empty()) {
        riskScore += 0.3;
        analysis += "⚠️ Spoofing activity detected. ";
    }
    
    // Iceberg risk
    if (!snapshot.recentIcebergs.empty()) {
        riskScore += 0.2;
        analysis += "🧊 Hidden liquidity detected. ";
    }
    
    // Absorption risk
    if (!snapshot.activeAbsorptions.empty()) {
        riskScore += 0.2;
        analysis += "🛡️ Absorption zones active. ";
    }
    
    // Liquidity imbalance
    double bidLiquidity = calculateTotalBidLiquidity(snapshot.currentSlice);
    double askLiquidity = calculateTotalAskLiquidity(snapshot.currentSlice);
    double imbalance = std::abs(bidLiquidity - askLiquidity) / std::max(bidLiquidity, askLiquidity);
    
    if (imbalance > 0.3) {
        riskScore += 0.3;
        analysis += QString("⚖️ Liquidity imbalance: %1%").arg(imbalance * 100, 0, 'f', 1);
    }
    
    snapshot.riskScore = std::min(riskScore, 1.0);
    snapshot.analysis = analysis.isEmpty() ? "Market conditions normal." : analysis;
    
    emit analysisReady(snapshot);
}
```

---

## 🎯 **Implementation Workflow**

### **For Each Phase:**

1. **Create Feature Branch**
   ```bash
   git checkout -b feature/phase-X-spoof-detection
   ```

2. **Implement Core Logic**
   - Follow the step-by-step code examples above
   - Add unit tests for each component
   - Ensure performance targets are met

3. **Integration Testing**
   ```bash
   cd build
   cmake --build . --config Debug
   ./apps/sentinel_gui/sentinel
   ```

4. **Performance Validation**
   ```bash
   # Run performance tests
   ./tests/test_stress_performance
   # Verify < 15ms CPU per slice
   ```

5. **Documentation Update**
   - Update relevant architecture docs
   - Add configuration examples
   - Document new signals and properties

6. **PR Submission**
   ```bash
   git add .
   git commit -m "feat: Implement Phase X - [Feature Name]"
   git push origin feature/phase-X-spoof-detection
   ```

### **Testing Strategy**

#### **Unit Tests**
- Each phase includes comprehensive unit tests
- Test synthetic market data patterns
- Verify performance constraints

#### **Integration Tests**
- Test with live Coinbase data
- Verify GUI responsiveness
- Check memory usage patterns

#### **Performance Gates**
- CPU usage < 15ms per slice
- Memory growth < 10% per hour
- Zero frame drops at 20k msg/s

---

## 🚀 **Quick Start Implementation**

### **Begin with Phase 1 (Easiest)**
```bash
# 1. Checkout feature branch
git checkout -b feature/phase-1-spoof-detection

# 2. Implement core changes
# - Add sizeAtFirstSeen to PriceLevelMetrics
# - Create SpoofEvent structure
# - Implement detectSpoofEvents()
# - Add GUI integration

# 3. Test implementation
cd build && cmake --build . --config Debug
./apps/sentinel_gui/sentinel

# 4. Verify spoof detection works
# - Watch for orange warning icons
# - Check console for spoof events
# - Validate performance metrics
```

### **Success Criteria for Each Phase**
- ✅ All unit tests pass
- ✅ Performance targets met
- ✅ GUI integration working
- ✅ Documentation updated
- ✅ No regression in existing functionality

---

## 📚 **Additional Resources**

### **Key Files to Study**
- `libs/gui/LiquidityTimeSeriesEngine.{h,cpp}` - Core aggregation engine
- `libs/gui/UnifiedGridRenderer.{h,cpp}` - GPU rendering system
- `libs/core/DataCache.cpp` - Trade and order book caching
- `libs/core/RuleEngine.{h,cpp}` - Existing rule system (reference)

### **Performance Monitoring**
- Use `SentinelLogging.hpp` for performance tracking
- Monitor GPU render times in `UnifiedGridRenderer`
- Track memory usage in `LiquidityTimeSeriesEngine`

### **Configuration**
- Add configuration options to `config.yaml`
- Expose thresholds as QML properties
- Provide runtime adjustment capabilities

This implementation plan provides a clear roadmap for building professional-grade market microstructure analysis capabilities into Sentinel, with each phase building upon the existing architecture and maintaining the high-performance standards already established. 