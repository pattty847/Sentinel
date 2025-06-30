#include "candlestickbatched.h"
#include <QSGGeometry>
#include <QSGFlatColorMaterial>
#include <QSGVertexColorMaterial>
#include "Log.hpp"
#include "gpudataadapter.h"
#include <QDateTime>
#include <algorithm>
#include <chrono>
#include <cmath>

static constexpr auto CAT = "Candles";

CandlestickBatched::CandlestickBatched(QQuickItem* parent)
    : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);

    LOG_I(CAT, "🕯️ CandlestickBatched INITIALIZED - Professional Trading Terminal Candles!");
    LOG_I(CAT, "🎯 LOD System: Enabled | Max Candles:{} | Auto-scaling:{}", m_maxCandles, (m_volumeScaling ? "ON" : "OFF"));
}

void CandlestickBatched::onCandlesReady(const std::vector<CandleUpdate>& candles) {
    if (candles.empty()) return;

    {
        std::lock_guard<std::mutex> lock(m_dataMutex);
        for (const auto& update : candles) {
            auto& vec = m_candles[static_cast<size_t>(update.timeframe)];
            if (!vec.empty() && vec.back().timestamp_ms == update.candle.timestamp_ms) {
                vec.back() = update.candle;
            } else {
                vec.push_back(update.candle);
            }
        }

        m_geometryDirty.store(true);
    }

    update();
}

void CandlestickBatched::onViewChanged(int64_t startTimeMs, int64_t endTimeMs, 
                                       double minPrice, double maxPrice) {
    // 🔥 COORDINATE SYNCHRONIZATION: Match with GPUChartWidget
    bool coordsChanged = (m_viewStartTime_ms != startTimeMs || 
                         m_viewEndTime_ms != endTimeMs ||
                         m_minPrice != minPrice || 
                         m_maxPrice != maxPrice);
    
    if (coordsChanged) {
        m_viewStartTime_ms = startTimeMs;
        m_viewEndTime_ms = endTimeMs;
        m_minPrice = minPrice;
        m_maxPrice = maxPrice;
        m_coordinatesValid = true;
        
        // Update LOD if zoom level changed
        updateLODIfNeeded();
        
        m_geometryDirty.store(true);
        update();
        
        static int coordUpdateCount = 0;
        if (++coordUpdateCount <= 5) {
            LOG_D(CAT, "🕯️ CANDLE COORDINATES UPDATED #{} Time:{}-{} Price:{}-{}",
                  coordUpdateCount, startTimeMs, endTimeMs, minPrice, maxPrice);
        }
    }
}

QSGNode* CandlestickBatched::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) {
    auto startTime = std::chrono::high_resolution_clock::now();
    
    // Require valid widget size and coordinates
    if (width() <= 0 || height() <= 0 || !m_coordinatesValid) {
        delete oldNode;
        return nullptr;
    }
    
    // Get optimal LOD level and candle data
    CandleLOD::TimeFrame activeTimeFrame = selectOptimalTimeFrame();
    const auto& candles = m_candles[static_cast<size_t>(activeTimeFrame)];
    
    if (candles.empty()) {
        static int emptyCount = 0;
        if (++emptyCount <= 5 || emptyCount % 100 == 0) {
            LOG_D(CAT, "🕯️ NO CANDLES TO RENDER #{} TimeFrame:{} View valid:{}",
                  emptyCount, CandleUtils::timeFrameName(activeTimeFrame), m_coordinatesValid);
        }
        delete oldNode;
        return nullptr;
    } else {
        static int renderCount = 0;
        if (++renderCount <= 5 || renderCount % 100 == 0) {
            LOG_D(CAT, "🕯️ RENDERING CANDLES #{} Count:{} TimeFrame:{} ViewRange:{}-{}",
                  renderCount, candles.size(), CandleUtils::timeFrameName(activeTimeFrame),
                  m_viewStartTime_ms, m_viewEndTime_ms);
        }
    }
    
    // Create or reuse root transform node
    auto* rootNode = static_cast<QSGTransformNode*>(oldNode);
    if (!rootNode) {
        rootNode = new QSGTransformNode;
        
        // 🔥 TWO-DRAW-CALL ARCHITECTURE: Separate nodes for green/red candles
        auto* bullishNode = new QSGGeometryNode;
        auto* bearishNode = new QSGGeometryNode;
        
        rootNode->appendChildNode(bullishNode);
        rootNode->appendChildNode(bearishNode);
        
        LOG_D(CAT, "🕯️ CREATED CANDLE SCENE GRAPH: Two-draw-call architecture ready!");
    }
    
    if (m_geometryDirty.load()) {
        // Update render batch with current candle data
        separateAndUpdateCandles(candles);
        
        // Update GPU geometry for both candle types
        auto* bullishNode = static_cast<QSGGeometryNode*>(rootNode->childAtIndex(0));
        auto* bearishNode = static_cast<QSGGeometryNode*>(rootNode->childAtIndex(1));
        
        createCandleGeometry(bullishNode, m_renderBatch.bullishCandles, m_bullishColor, true);
        createCandleGeometry(bearishNode, m_renderBatch.bearishCandles, m_bearishColor, true);
        
        m_geometryDirty.store(false);
        
        // Emit performance signals
        int totalCandles = m_renderBatch.bullishCandles.size() + m_renderBatch.bearishCandles.size();
        emit candleCountChanged(totalCandles);
        emit lodLevelChanged(static_cast<int>(activeTimeFrame));
        
        LOG_D(CAT,
              "🕯️ CANDLE RENDER UPDATE: LOD:{} Bullish:{} Bearish:{} Total:{}",
              CandleUtils::timeFrameName(activeTimeFrame),
              m_renderBatch.bullishCandles.size(),
              m_renderBatch.bearishCandles.size(),
              totalCandles);
    }
    
    // Track render performance
    auto endTime = std::chrono::high_resolution_clock::now();
    m_lastRenderDuration_ms = std::chrono::duration<double, std::milli>(endTime - startTime).count();
    emit renderTimeChanged(m_lastRenderDuration_ms);
    
    return rootNode;
}

void CandlestickBatched::separateAndUpdateCandles(const std::vector<OHLC>& candles) {
    std::lock_guard<std::mutex> lock(m_dataMutex);
    
    // Clear previous batch
    m_renderBatch.bullishCandles.clear();
    m_renderBatch.bearishCandles.clear();
    
    // Filter candles to visible time window and separate by color
    for (const auto& candle : candles) {
        // Skip candles outside visible time window
        if (candle.timestamp_ms < m_viewStartTime_ms || candle.timestamp_ms > m_viewEndTime_ms) {
            continue;
        }
        
        // Convert to screen coordinates
        CandleInstance instance;
        instance.candle = candle;
        
        QPointF screenPos = worldToScreen(candle.timestamp_ms, 
                                         (candle.open + candle.close) / 2.0);
        instance.screenX = screenPos.x();
        
        // Calculate body coordinates
        QPointF topPos = worldToScreen(candle.timestamp_ms, std::max(candle.open, candle.close));
        QPointF bottomPos = worldToScreen(candle.timestamp_ms, std::min(candle.open, candle.close));
        instance.bodyTop = topPos.y();
        instance.bodyBottom = bottomPos.y();
        
        // Calculate wick coordinates  
        QPointF wickTopPos = worldToScreen(candle.timestamp_ms, candle.high);
        QPointF wickBottomPos = worldToScreen(candle.timestamp_ms, candle.low);
        instance.wickTop = wickTopPos.y();
        instance.wickBottom = wickBottomPos.y();
        
        // Calculate width (with optional volume scaling)
        instance.width = m_candleWidth;
        if (m_volumeScaling) {
            instance.width *= candle.volumeScale();
        }
        
        // Separate by bullish/bearish
        if (candle.isBullish()) {
            instance.color = m_bullishColor;
            m_renderBatch.bullishCandles.push_back(instance);
        } else {
            instance.color = m_bearishColor;
            m_renderBatch.bearishCandles.push_back(instance);
        }
    }
    
    // Limit to max candles for performance
    if (m_renderBatch.bullishCandles.size() + m_renderBatch.bearishCandles.size() > 
        static_cast<size_t>(m_maxCandles)) {
        
        // Keep most recent candles (simple truncation for now)
        size_t maxPerType = m_maxCandles / 2;
        if (m_renderBatch.bullishCandles.size() > maxPerType) {
            m_renderBatch.bullishCandles.resize(maxPerType);
        }
        if (m_renderBatch.bearishCandles.size() > maxPerType) {
            m_renderBatch.bearishCandles.resize(maxPerType);
        }
    }
}

void CandlestickBatched::createCandleGeometry(QSGGeometryNode* node, 
                                             const std::vector<CandleInstance>& candles,
                                             const QColor& color, bool isBody) {
    if (candles.empty()) {
        // Clear geometry if no candles
        if (node->geometry()) {
            node->geometry()->allocate(0);
            node->markDirty(QSGNode::DirtyGeometry);
        }
        return;
    }
    
    // 🔥 SIMPLIFIED APPROACH: Just render candle bodies for now to avoid segfault
    // Each candle = 1 rectangle = 2 triangles = 6 vertices
    const int verticesPerCandle = 6;
    
    // Create or update geometry
    if (!node->geometry()) {
        QSGGeometry* geometry = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 
                                               candles.size() * verticesPerCandle);
        geometry->setDrawingMode(QSGGeometry::DrawTriangles);
        node->setGeometry(geometry);
        node->setFlag(QSGNode::OwnsGeometry);
        
        // Set material for vertex colors
        QSGVertexColorMaterial* material = new QSGVertexColorMaterial;
        material->setFlag(QSGMaterial::Blending);
        node->setMaterial(material);
        node->setFlag(QSGNode::OwnsMaterial);
    }
    
    // Allocate vertices for all candles
    QSGGeometry* geometry = node->geometry();
    const int totalVertices = candles.size() * verticesPerCandle;
    geometry->allocate(totalVertices);
    QSGGeometry::ColoredPoint2D* vertices = geometry->vertexDataAsColoredPoint2D();
    
    // Build candle bodies directly to GPU buffer
    int vertexIndex = 0;
    for (const auto& candle : candles) {
        float halfWidth = candle.width / 2.0f;
        float left = candle.screenX - halfWidth;
        float right = candle.screenX + halfWidth;
        float top = candle.bodyTop;
        float bottom = candle.bodyBottom;
        
        // Ensure minimum body height for visibility
        if (std::abs(top - bottom) < 2.0f) {
            float center = (top + bottom) / 2.0f;
            top = center - 1.0f;
            bottom = center + 1.0f;
        }
        
        // Color from candle instance
        uchar r = static_cast<uchar>(candle.color.redF() * 255);
        uchar g = static_cast<uchar>(candle.color.greenF() * 255);
        uchar b = static_cast<uchar>(candle.color.blueF() * 255);
        uchar a = static_cast<uchar>(candle.color.alphaF() * 255);
        
        // 🔥 RECTANGLE BODY: Two triangles forming a quad
        // Triangle 1: top-left, top-right, bottom-left
        if (vertexIndex + 5 < totalVertices) {
            vertices[vertexIndex++].set(left, top, r, g, b, a);
            vertices[vertexIndex++].set(right, top, r, g, b, a);
            vertices[vertexIndex++].set(left, bottom, r, g, b, a);
            
            // Triangle 2: top-right, bottom-right, bottom-left  
            vertices[vertexIndex++].set(right, top, r, g, b, a);
            vertices[vertexIndex++].set(right, bottom, r, g, b, a);
            vertices[vertexIndex++].set(left, bottom, r, g, b, a);
        }
    }
    
    node->markDirty(QSGNode::DirtyGeometry);
}

// 🔥 NOTE: Currently not used - simplified rendering approach above for stability
void CandlestickBatched::buildCandleBodies(const std::vector<CandleInstance>& candles,
                                          std::vector<CandleVertex>& vertices) const {
    for (const auto& candle : candles) {
        float halfWidth = candle.width / 2.0f;
        float left = candle.screenX - halfWidth;
        float right = candle.screenX + halfWidth;
        float top = candle.bodyTop;
        float bottom = candle.bodyBottom;
        
        // Ensure minimum body height for very small candles
        if (std::abs(top - bottom) < 2.0f) {
            float center = (top + bottom) / 2.0f;
            top = center - 1.0f;
            bottom = center + 1.0f;
        }
        
        // Color from candle instance
        float r = candle.color.redF();
        float g = candle.color.greenF();
        float b = candle.color.blueF();
        float a = candle.color.alphaF();
        
        // 🔥 RECTANGLE BODY: Two triangles forming a quad
        // Triangle 1: top-left, top-right, bottom-left
        vertices.push_back({left, top, r, g, b, a});
        vertices.push_back({right, top, r, g, b, a});
        vertices.push_back({left, bottom, r, g, b, a});
        
        // Triangle 2: top-right, bottom-right, bottom-left  
        vertices.push_back({right, top, r, g, b, a});
        vertices.push_back({right, bottom, r, g, b, a});
        vertices.push_back({left, bottom, r, g, b, a});
    }
}

// 🔥 NOTE: Currently not used - simplified rendering approach above for stability  
void CandlestickBatched::buildCandleWicks(const std::vector<CandleInstance>& candles,
                                         std::vector<CandleVertex>& vertices) const {
    for (const auto& candle : candles) {
        float centerX = candle.screenX;
        float wickWidth = 1.0f; // Thin wick lines
        float left = centerX - wickWidth;
        float right = centerX + wickWidth;
        
        // Upper wick (from body top to high)
        float upperWickTop = candle.wickTop;
        float upperWickBottom = candle.bodyTop;
        
        // Lower wick (from body bottom to low)
        float lowerWickTop = candle.bodyBottom;
        float lowerWickBottom = candle.wickBottom;
        
        // Wick color (usually same as body but can be different)
        float r = m_wickColor.redF();
        float g = m_wickColor.greenF(); 
        float b = m_wickColor.blueF();
        float a = m_wickColor.alphaF();
        
        // 🔥 UPPER WICK: Rectangle from body top to high
        if (upperWickTop < upperWickBottom) { // Only draw if there's a wick
            vertices.push_back({left, upperWickTop, r, g, b, a});
            vertices.push_back({right, upperWickTop, r, g, b, a});
            vertices.push_back({left, upperWickBottom, r, g, b, a});
            
            vertices.push_back({right, upperWickTop, r, g, b, a});
            vertices.push_back({right, upperWickBottom, r, g, b, a});
            vertices.push_back({left, upperWickBottom, r, g, b, a});
        } else {
            // Add dummy vertices to maintain vertex count
            for (int i = 0; i < 6; ++i) {
                vertices.push_back({centerX, centerX, 0, 0, 0, 0}); // Transparent
            }
        }
        
        // 🔥 LOWER WICK: Rectangle from body bottom to low
        if (lowerWickBottom > lowerWickTop) { // Only draw if there's a wick
            vertices.push_back({left, lowerWickTop, r, g, b, a});
            vertices.push_back({right, lowerWickTop, r, g, b, a});
            vertices.push_back({left, lowerWickBottom, r, g, b, a});
            
            vertices.push_back({right, lowerWickTop, r, g, b, a});
            vertices.push_back({right, lowerWickBottom, r, g, b, a});
            vertices.push_back({left, lowerWickBottom, r, g, b, a});
        } else {
            // Add dummy vertices to maintain vertex count
            for (int i = 0; i < 6; ++i) {
                vertices.push_back({centerX, centerX, 0, 0, 0, 0}); // Transparent
            }
        }
    }
}

QPointF CandlestickBatched::worldToScreen(int64_t timestamp_ms, double price) const {
    if (!m_coordinatesValid || m_viewEndTime_ms <= m_viewStartTime_ms || m_maxPrice <= m_minPrice) {
        return QPointF(0, 0);
    }
    
    // Time mapping (left = older, right = newer)
    double timeProgress = static_cast<double>(timestamp_ms - m_viewStartTime_ms) / 
                         static_cast<double>(m_viewEndTime_ms - m_viewStartTime_ms);
    double x = timeProgress * width();
    
    // Price mapping (top = higher price, bottom = lower price)
    double priceProgress = (price - m_minPrice) / (m_maxPrice - m_minPrice);
    double y = (1.0 - priceProgress) * height();
    
    return QPointF(x, y);
}

CandleLOD::TimeFrame CandlestickBatched::selectOptimalTimeFrame() const {
    if (!m_autoLOD) {
        return m_forcedTimeFrame;
    }
    
    // Calculate pixels per candle based on current view
    double pixelsPerCandle = calculateCurrentPixelsPerCandle();
    if (pixelsPerCandle < 2.0)  return CandleLOD::TF_Daily;
    if (pixelsPerCandle < 5.0)  return CandleLOD::TF_60min;
    if (pixelsPerCandle < 10.0) return CandleLOD::TF_15min;
    if (pixelsPerCandle < 20.0) return CandleLOD::TF_5min;
    if (pixelsPerCandle < 40.0) return CandleLOD::TF_1min;
    if (pixelsPerCandle < 80.0) return CandleLOD::TF_1sec;
    if (pixelsPerCandle < 160.0) return CandleLOD::TF_500ms;
    return CandleLOD::TF_100ms;
}

double CandlestickBatched::calculateCurrentPixelsPerCandle() const {
    if (!m_coordinatesValid || m_viewEndTime_ms <= m_viewStartTime_ms) {
        return 20.0; // Default to 1min candles
    }
    
    CandleLOD::TimeFrame tf = m_autoLOD ? CandleLOD::TF_1min : m_forcedTimeFrame;
    int64_t viewSpan_ms = m_viewEndTime_ms - m_viewStartTime_ms;
    double ppc = CandleUtils::calculatePixelsPerCandle(width(), viewSpan_ms, tf);
    return ppc > 0 ? ppc : 20.0;
}

void CandlestickBatched::updateLODIfNeeded() {
    if (!m_autoLOD) return;
    
    CandleLOD::TimeFrame newTimeFrame = selectOptimalTimeFrame();
    static CandleLOD::TimeFrame lastTimeFrame = CandleLOD::TF_1min;
    
    if (newTimeFrame != lastTimeFrame) {
        LOG_I(CAT,
              "🕯️ LOD CHANGED: {} → {} Pixels per candle:{}",
              CandleUtils::timeFrameName(lastTimeFrame),
              CandleUtils::timeFrameName(newTimeFrame),
              calculateCurrentPixelsPerCandle());
        lastTimeFrame = newTimeFrame;
    }
}

// Property setters
void CandlestickBatched::setLodEnabled(bool enabled) {
    if (m_lodEnabled != enabled) {
        m_lodEnabled = enabled;
        m_geometryDirty.store(true);
        update();
        emit lodEnabledChanged();
    }
}

void CandlestickBatched::setCandleWidth(double width) {
    if (m_candleWidth != width) {
        m_candleWidth = width;
        m_geometryDirty.store(true);
        update();
        emit candleWidthChanged();
    }
}

void CandlestickBatched::setVolumeScaling(bool enabled) {
    if (m_volumeScaling != enabled) {
        m_volumeScaling = enabled;
        m_geometryDirty.store(true);
        update();
        emit volumeScalingChanged();
    }
}

void CandlestickBatched::setMaxCandles(int max) {
    if (m_maxCandles != max) {
        m_maxCandles = max;
        emit maxCandlesChanged();
    }
}

void CandlestickBatched::clearCandles() {
    std::lock_guard<std::mutex> lock(m_dataMutex);
    for (auto& v : m_candles) v.clear();
    m_geometryDirty.store(true);
    update();
}

// Removed geometryChanged - not needed for this implementation

// 🔥 MISSING FUNCTION IMPLEMENTATIONS: Required by Qt MOC
void CandlestickBatched::setAutoLOD(bool enabled) {
    if (m_autoLOD != enabled) {
        m_autoLOD = enabled;
        m_geometryDirty.store(true);
        update();
    }
}

void CandlestickBatched::forceTimeFrame(int timeframe) {
    if (timeframe >= 0 && timeframe < static_cast<int>(CandleLOD::NUM_TIMEFRAMES)) {
        m_forcedTimeFrame = static_cast<CandleLOD::TimeFrame>(timeframe);
        m_geometryDirty.store(true);
        update();
    }
}

void CandlestickBatched::setTimeWindow(int64_t startTime_ms, int64_t endTime_ms) {
    if (m_viewStartTime_ms != startTime_ms || m_viewEndTime_ms != endTime_ms) {
        m_viewStartTime_ms = startTime_ms;
        m_viewEndTime_ms = endTime_ms;
        m_coordinatesValid = true;
        m_geometryDirty.store(true);
        update();
    }
}

void CandlestickBatched::setBullishColor(const QColor& color) {
    if (m_bullishColor != color) {
        m_bullishColor = color;
        m_geometryDirty.store(true);
        update();
    }
}

void CandlestickBatched::setBearishColor(const QColor& color) {
    if (m_bearishColor != color) {
        m_bearishColor = color;
        m_geometryDirty.store(true);
        update();
    }
}

void CandlestickBatched::setWickColor(const QColor& color) {
    if (m_wickColor != color) {
        m_wickColor = color;
        m_geometryDirty.store(true);
        update();
    }
} 