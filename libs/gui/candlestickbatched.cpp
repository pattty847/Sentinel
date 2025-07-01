#include "candlestickbatched.h"
#include "../core/SentinelLogging.hpp"
#include <QSGGeometry>
#include <QSGFlatColorMaterial>
#include <QSGVertexColorMaterial>
#include <QDebug>
#include "gpudataadapter.h"
#include <QDateTime>
#include <algorithm>
#include <chrono>
#include <cmath>

CandlestickBatched::CandlestickBatched(QQuickItem* parent)
    : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);

    sLog_Init("🕯️ CandlestickBatched INITIALIZED - Professional Trading Terminal Candles!");
    sLog_Init("🎯 LOD System: Enabled | Max Candles:" << m_maxCandles
             << "| Auto-scaling: " << (m_volumeScaling ? "ON" : "OFF"));
}

void CandlestickBatched::onCandlesReady(const std::vector<CandleUpdate>& candles) {
    // 🔍 DEBUG: Log candle data reception
    static int candleReceptionCount = 0;
    if (++candleReceptionCount <= 10) {
        sLog_Candles("📦 CANDLES RECEIVED #" << candleReceptionCount
                 << "Count:" << candles.size());
                 
        if (!candles.empty()) {
            const auto& first = candles[0];
            sLog_Candles("🕯️ FIRST CANDLE UPDATE: timeframe=" << static_cast<int>(first.timeframe)
                     << "timestamp=" << first.candle.timestamp_ms
                     << "OHLC:" << first.candle.open << first.candle.high 
                     << first.candle.low << first.candle.close);
        }
    }
    
    if (candles.empty()) {
        if (candleReceptionCount <= 10) {
            sLog_Candles("⚠️ EMPTY CANDLES RECEIVED - returning early");
        }
        return;
    }

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
        
        // 🔍 DEBUG: Show candle storage status
        if (candleReceptionCount <= 10) {
            int totalCandles = 0;
            for (const auto& vec : m_candles) {
                totalCandles += vec.size();
            }
            sLog_Candles("📊 CANDLE STORAGE: Total candles across all timeframes:" << totalCandles);
        }
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
                         
    // 🔍 DEBUG: Always log first 20 coordinate updates for visibility debugging
    static int coordUpdateCount = 0;
    coordUpdateCount++;
    
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
    }
    
    if (coordUpdateCount <= 20) { // Increased debugging
        sLog_DebugCoords("🕯️ CANDLE COORDINATES UPDATE #" << coordUpdateCount
                 << "CHANGED:" << (coordsChanged ? "YES" : "NO")
                 << "Time window:" << startTimeMs << "-" << endTimeMs 
                 << "Duration:" << ((endTimeMs - startTimeMs) / 1000.0) << "seconds"
                 << "Price window:" << minPrice << "-" << maxPrice
                 << "Price range:" << (maxPrice - minPrice)
                 << "Widget size:" << width() << "x" << height()
                 << "Coords valid:" << m_coordinatesValid);
                 
        // Check if the coordinate ranges look reasonable for BTC
        bool reasonableTime = (endTimeMs - startTimeMs) > 1000 && (endTimeMs - startTimeMs) < 86400000; // 1s to 1 day
        bool reasonablePrice = minPrice > 50000 && maxPrice < 200000 && (maxPrice - minPrice) > 1.0; // BTC price range
        
        sLog_DebugCoords("🔍 COORDINATE SANITY CHECK:"
                 << "Time range reasonable:" << (reasonableTime ? "YES" : "NO")
                 << "Price range reasonable:" << (reasonablePrice ? "YES" : "NO")
                 << "Total candles available:" << m_candles[static_cast<size_t>(CandleLOD::TF_1sec)].size());
    }
}

QSGNode* CandlestickBatched::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) {
    auto startTime = std::chrono::high_resolution_clock::now();
    
    // 🚨 TEMPORARY: Disable candle rendering to isolate Qt scene graph issue
    // This proves our timing frequency fix works perfectly and the rest of the app is stable
    // sLog_Render() << "🕯️ CANDLE RENDERING TEMPORARILY DISABLED - Timing frequency fix proven successful!";
    // delete oldNode;
    // return nullptr;
    
    // 🔍 DEBUG: Enhanced checks for widget state and coordinate validity
    static int paintNodeCount = 0;
    paintNodeCount++;
    
    if (paintNodeCount <= 10) {
        sLog_RenderDetail("🎨 PAINT NODE UPDATE #" << paintNodeCount
                 << "Widget size:" << width() << "x" << height()
                 << "Coordinates valid:" << m_coordinatesValid
                 << "Time range:" << m_viewStartTime_ms << "-" << m_viewEndTime_ms
                 << "Price range:" << m_minPrice << "-" << m_maxPrice
                 << "Geometry dirty:" << m_geometryDirty.load());
    }
    
    // CRITICAL: Ensure we're on the render thread and ready to paint
    if (width() <= 0 || height() <= 0 || !m_coordinatesValid) {
        if (paintNodeCount <= 10) {
            sLog_RenderDetail("🕯️ SKIPPING PAINT: Invalid state - width:" << width() 
                     << "height:" << height() << "coordsValid:" << m_coordinatesValid
                     << "Reason:" << (width() <= 0 ? "ZERO_WIDTH" : 
                                    height() <= 0 ? "ZERO_HEIGHT" : "INVALID_COORDS"));
        }
        delete oldNode;
        return nullptr;
    }
    
    // Additional safety: Ensure geometry dirty flag is stable
    if (!m_geometryDirty.load()) {
        // No updates needed, return existing node
        return oldNode;
    }
    
    // Get optimal LOD level and candle data
    CandleLOD::TimeFrame activeTimeFrame = selectOptimalTimeFrame();
    const auto& candles = m_candles[static_cast<size_t>(activeTimeFrame)];
    
    if (candles.empty()) {
        static int emptyCount = 0;
        if (++emptyCount <= 5 || emptyCount % 100 == 0) {
            sLog_Render("🕯️ NO CANDLES TO RENDER #" << emptyCount 
                     << "TimeFrame:" << CandleUtils::timeFrameName(activeTimeFrame)
                     << "View valid:" << m_coordinatesValid);
        }
        delete oldNode;
        return nullptr;
    } else {
        static int renderCount = 0;
        if (++renderCount <= 5 || renderCount % 100 == 0) {
            sLog_Render("🕯️ RENDERING CANDLES #" << renderCount 
                     << "Count:" << candles.size()
                     << "TimeFrame:" << CandleUtils::timeFrameName(activeTimeFrame)
                     << "ViewRange:" << m_viewStartTime_ms << "-" << m_viewEndTime_ms);
        }
    }
    
    // Create or reuse root transform node
    auto* rootNode = static_cast<QSGTransformNode*>(oldNode);
    QSGGeometryNode* bullishNode = nullptr;
    QSGGeometryNode* bearishNode = nullptr;
    
    if (!rootNode) {
        rootNode = new QSGTransformNode();
        bullishNode = new QSGGeometryNode();
        bearishNode = new QSGGeometryNode();
        rootNode->appendChildNode(bullishNode);
        rootNode->appendChildNode(bearishNode);
        sLog_RenderDetail("🕯️ CREATED NEW ROOT NODE: Setting up candle scene graph");
    } else {
        bullishNode = static_cast<QSGGeometryNode*>(rootNode->childAtIndex(0));
        bearishNode = static_cast<QSGGeometryNode*>(rootNode->childAtIndex(1));
    }
    
    if (m_geometryDirty.load()) {
        // Store the active timeframe so downstream helpers know the candle span
        m_activeTimeFrame = activeTimeFrame;

        separateAndUpdateCandles(candles);
        
        // 🚀 PHASE 4: GPU UPLOAD PROFILER - Reset frame counter
        m_bytesUploadedThisFrame = 0;
        
        // Update GPU geometry for both candle types
        createCandleGeometry(bullishNode, m_renderBatch.bullishCandles, m_bullishColor, true);
        createCandleGeometry(bearishNode, m_renderBatch.bearishCandles, m_bearishColor, true);
        
        m_geometryDirty.store(false);
        
        emit candleCountChanged(m_renderBatch.bullishCandles.size() + m_renderBatch.bearishCandles.size());
        emit lodLevelChanged(static_cast<int>(activeTimeFrame));
        
        sLog_Render("🕯️ CANDLE RENDER UPDATE:"
                 << "LOD:" << CandleUtils::timeFrameName(activeTimeFrame)
                 << "Bullish:" << m_renderBatch.bullishCandles.size()
                 << "Bearish:" << m_renderBatch.bearishCandles.size()
                 << "Total:" << (m_renderBatch.bullishCandles.size() + m_renderBatch.bearishCandles.size()));
    }
    
    // Track render performance
    auto endTime = std::chrono::high_resolution_clock::now();
    m_lastRenderDuration_ms = std::chrono::duration<double, std::milli>(endTime - startTime).count();
    emit renderTimeChanged(m_lastRenderDuration_ms);
    
    // 🚀 PHASE 4: GPU UPLOAD BANDWIDTH MONITORING
    if (m_geometryDirty.load() == false) { // Only track when we actually uploaded data
        m_mbPerFrame = static_cast<double>(m_bytesUploadedThisFrame) / 1'000'000.0;
        m_totalBytesUploaded.fetch_add(m_bytesUploadedThisFrame, std::memory_order_relaxed);
        
        // Calculate bandwidth: MB per frame * frames per second = MB/s
        double estimatedFPS = m_lastRenderDuration_ms > 0 ? (1000.0 / m_lastRenderDuration_ms) : 60.0;
        double bandwidthMBps = m_mbPerFrame * estimatedFPS;
        
        // Warn if exceeding PCIe budget
        if (bandwidthMBps > PCIE_BUDGET_MB_PER_SECOND) {
            m_bandwidthWarnings.fetch_add(1, std::memory_order_relaxed);
            static int warningCount = 0;
            if (++warningCount <= 5) { // Limit warning spam
                sLog_Performance("⚠️ PCIe BANDWIDTH WARNING #" << warningCount
                          << "Current:" << QString::number(bandwidthMBps, 'f', 1) << "MB/s"
                          << "Budget:" << PCIE_BUDGET_MB_PER_SECOND << "MB/s"
                          << "Frame:" << QString::number(m_mbPerFrame, 'f', 3) << "MB"
                          << "FPS:" << QString::number(estimatedFPS, 'f', 1));
            }
        }
        
        // Debug logging for first few frames
        static int profileCount = 0;
        if (++profileCount <= 10 || profileCount % 100 == 0) {
            sLog_DebugTiming("📊 GPU UPLOAD PROFILER #" << profileCount
                     << "Frame:" << QString::number(m_mbPerFrame, 'f', 3) << "MB"
                     << "Bandwidth:" << QString::number(bandwidthMBps, 'f', 1) << "MB/s"
                     << "Total uploaded:" << (m_totalBytesUploaded.load() / 1'000'000) << "MB"
                     << "Warnings:" << m_bandwidthWarnings.load());
        }
    }
    
    // FINAL VALIDATION: Ensure scene graph is in valid state before returning
    if (!rootNode || rootNode->childCount() != 2) {
        qCritical() << "🚨 SCENE GRAPH CORRUPTION: Invalid node structure before return!"
                   << "rootNode:" << (void*)rootNode 
                   << "childCount:" << (rootNode ? rootNode->childCount() : -1);
        delete rootNode;
        return nullptr;
    }
    
    // Validate child nodes exist and are correct type
    auto* child0 = rootNode->childAtIndex(0);
    auto* child1 = rootNode->childAtIndex(1);
    if (!child0 || !child1 || 
        child0->type() != QSGNode::GeometryNodeType || 
        child1->type() != QSGNode::GeometryNodeType) {
        qCritical() << "🚨 SCENE GRAPH CORRUPTION: Invalid child node types before return!";
        delete rootNode;
        return nullptr;
    }
    
    sLog_RenderDetail("✅ SCENE GRAPH VALIDATION PASSED: Returning valid candle node structure");

    // ────────────────────────────────────────────────────────────
    // MICRO-VIEW CLEAN-UP: Hide candle layer when the visible window
    // is tighter than 10 seconds.  Trade dots + heat-map give better
    // visual fidelity here; candles would be zero-height noise.
    // ────────────────────────────────────────────────────────────
    const int64_t kHideCandleThresholdMs = 10'000; // 10 s
    if (m_coordinatesValid) {
        int64_t viewSpanMs = m_viewEndTime_ms - m_viewStartTime_ms;
        if (viewSpanMs > 0 && viewSpanMs < kHideCandleThresholdMs) {
            if (paintNodeCount <= 10 || paintNodeCount % 200 == 0) {
                sLog_RenderDetail("🕯️ MICRO VIEW: Candle layer hidden (view span" << viewSpanMs << "ms)");
            }
            // Clean up previous node if it exists, then skip rendering.
            delete oldNode;
            return nullptr;
        }
    }

    return rootNode;
}

void CandlestickBatched::separateAndUpdateCandles(const std::vector<OHLC>& candles) {
    std::lock_guard<std::mutex> lock(m_dataMutex);
    
    // Clear previous batch
    m_renderBatch.bullishCandles.clear();
    m_renderBatch.bearishCandles.clear();
    
    // 🔍 DEBUG: Log candle processing details
    static int separateCount = 0;
    if (++separateCount <= 5) {
        sLog_DebugGeometry("🕯️ SEPARATING CANDLES #" << separateCount
                 << "Input candles:" << candles.size()
                 << "View window: time[" << m_viewStartTime_ms << "-" << m_viewEndTime_ms << "]"
                 << "View window: price[" << m_minPrice << "-" << m_maxPrice << "]");
    }
    
    int visibleCandleCount = 0;
    int outsideTimeWindow = 0;
    int outsidePriceWindow = 0;
    
    // Filter candles to visible time window and separate by color
    for (const auto& candle : candles) {
        // Skip candles outside visible time window
        if (candle.timestamp_ms < m_viewStartTime_ms || candle.timestamp_ms > m_viewEndTime_ms) {
            outsideTimeWindow++;
            continue;
        }
        
        // 🔍 DEBUG: Check if candle prices are in reasonable range
        if (candle.open < m_minPrice - 1000 || candle.open > m_maxPrice + 1000) {
            outsidePriceWindow++;
            if (separateCount <= 5) { // Only log first few batches
                sLog_DebugGeometry("🚨 CANDLE OUTSIDE PRICE RANGE:" 
                         << "timestamp:" << candle.timestamp_ms
                         << "OHLC:" << candle.open << candle.high << candle.low << candle.close
                         << "vs range:" << m_minPrice << "-" << m_maxPrice);
            }
            // Continue processing even if outside price range - it might be visible
        }
        
        visibleCandleCount++;
        
        // Align candle on its start timestamp (OHLC bucket boundary) so it
        // appears immediately when the first trade arrives.
        int64_t candlePivotTime = candle.timestamp_ms;

        // Prepare render instance
        CandleInstance instance;
        instance.candle = candle;
        
        QPointF screenPos = worldToScreen(candlePivotTime, 
                                         (candle.open + candle.close) / 2.0);
        instance.screenX = screenPos.x();
        
        // Calculate body coordinates
        QPointF topPos = worldToScreen(candlePivotTime, std::max(candle.open, candle.close));
        QPointF bottomPos = worldToScreen(candlePivotTime, std::min(candle.open, candle.close));
        instance.bodyTop = topPos.y();
        instance.bodyBottom = bottomPos.y();
        
        // Calculate wick coordinates  
        QPointF wickTopPos = worldToScreen(candlePivotTime, candle.high);
        QPointF wickBottomPos = worldToScreen(candlePivotTime, candle.low);
        instance.wickTop = wickTopPos.y();
        instance.wickBottom = wickBottomPos.y();
        
        // Calculate width (with optional volume scaling)
        instance.width = m_candleWidth;
        if (m_volumeScaling) {
            instance.width *= candle.volumeScale();
        }
        
        // 🚀 VISIBILITY FIX: Ensure candles are always at least a few pixels wide so
        // they remain visible even when volume is extremely low. Without this
        // clamp, widths can drop below a pixel, effectively rendering the body
        // invisible despite correct coordinates.
        const float MIN_CANDLE_PX = 2.0f; // Hard-floor in device pixels
        if (instance.width < MIN_CANDLE_PX) {
            instance.width = MIN_CANDLE_PX;
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
    
    // 🔍 DEBUG: Summary of candle processing results
    if (separateCount <= 5) {
        sLog_DebugGeometry("📊 CANDLE PROCESSING SUMMARY #" << separateCount
                 << "Total input:" << candles.size()
                 << "Visible:" << visibleCandleCount
                 << "Outside time:" << outsideTimeWindow
                 << "Outside price:" << outsidePriceWindow  
                 << "Final bullish:" << m_renderBatch.bullishCandles.size()
                 << "Final bearish:" << m_renderBatch.bearishCandles.size());
                 
        // Show first few candle details if any were processed
        if (!m_renderBatch.bullishCandles.empty()) {
            const auto& first = m_renderBatch.bullishCandles[0];
            sLog_DebugGeometry("🟢 FIRST BULLISH CANDLE: x=" << first.screenX 
                     << "bodyTop=" << first.bodyTop << "bodyBottom=" << first.bodyBottom
                     << "wickTop=" << first.wickTop << "wickBottom=" << first.wickBottom);
        }
        if (!m_renderBatch.bearishCandles.empty()) {
            const auto& first = m_renderBatch.bearishCandles[0];
            sLog_DebugGeometry("🔴 FIRST BEARISH CANDLE: x=" << first.screenX
                     << "bodyTop=" << first.bodyTop << "bodyBottom=" << first.bodyBottom
                     << "wickTop=" << first.wickTop << "wickBottom=" << first.wickBottom);
        }
    }
}

void CandlestickBatched::createCandleGeometry(QSGGeometryNode* node, const std::vector<CandleInstance>& candles, 
                                             const QColor& color, bool isBody) {
    int vertexCount = candles.empty() ? 0 : (isBody ? candles.size() * 6 : candles.size() * 8);

    // 🔍 DEBUG: Log geometry creation details + check if candles are in viewport
    static int geometryCreateCount = 0;
    if (++geometryCreateCount <= 15) {
        int visibleCandles = 0;
        if (!candles.empty()) {
            // Count how many candles are actually in the visible viewport
            for (const auto& candle : candles) {
                if (candle.screenX >= 0 && candle.screenX <= width() &&
                    candle.bodyTop >= 0 && candle.bodyBottom <= height()) {
                    visibleCandles++;
                }
            }
        }
        
        sLog_DebugGeometry("🏗️ CREATING GEOMETRY #" << geometryCreateCount
                 << "Total candles:" << candles.size() 
                 << "VISIBLE candles:" << visibleCandles
                 << "VertexCount:" << vertexCount
                 << "IsBody:" << isBody
                 << "Color:" << color.name() << "Alpha:" << color.alpha()
                 << "Widget size:" << width() << "x" << height());
                 
        // Log first few candle positions
        if (!candles.empty() && geometryCreateCount <= 5) {
            for (size_t i = 0; i < std::min(size_t(3), candles.size()); ++i) {
                const auto& c = candles[i];
                sLog_DebugGeometry("  🕯️ Candle" << i << ":"
                         << "x=" << c.screenX
                         << "bodyTop=" << c.bodyTop << "bodyBottom=" << c.bodyBottom
                         << "wickTop=" << c.wickTop << "wickBottom=" << c.wickBottom
                         << "width=" << c.width);
            }
        }
    }

    // --------------------------------------------------------------------
    // 1.  MATERIAL – every QSGGeometryNode submitted to the scene graph MUST
    //     have a valid material pointer.  Missing materials are a very common
    //     cause of crashes deep inside QSGBatchRenderer (null-ptr deref while
    //     the renderer inspects material flags).
    // --------------------------------------------------------------------

    QSGFlatColorMaterial* mat = static_cast<QSGFlatColorMaterial*>(node->material());
    if (!mat) {
        mat = new QSGFlatColorMaterial();
        node->setMaterial(mat);
        node->setFlag(QSGNode::OwnsMaterial);
    }
    if (mat->color() != color)
        mat->setColor(color);

    // --------------------------------------------------------------------
    // 2. GEOMETRY – ensure vertex buffer has the right size.
    // --------------------------------------------------------------------

    // Ensure geometry buffer is correctly sized for the current vertex count.
    // We now allow 0-vertex allocations so that geometry that should be invisible
    // is genuinely empty rather than containing a dummy vertex that may confuse
    // Qt's renderer.
    ensureCapacity(node, vertexCount, isBody ? QSGGeometry::DrawTriangles : QSGGeometry::DrawLines);

    QSGGeometry* geometry = node->geometry();

    if (vertexCount == 0) {
        // Nothing to render – make sure the scene graph knows the geometry has
        // changed (it might have been non-empty last frame).
        node->markDirty(QSGNode::DirtyGeometry);
        return;
    }

    if (vertexCount > 0) {
        auto* vertices = geometry->vertexDataAsPoint2D();
        if (!vertices) {
            qCritical() << "🚨 FAILED to get vertex data pointer";
            return;
        }
        
        int currentVertex = 0;
        for (const auto& candle : candles) {
            if (isBody) {
                float left = candle.screenX - candle.width / 2.0f;
                float right = candle.screenX + candle.width / 2.0f;
                
                float top = candle.bodyTop;
                float bottom = candle.bodyBottom;

                // Guarantee a minimum visible body height (otherwise completely flat candles disappear)
                if (std::fabs(top - bottom) < 2.0f) {
                    float center = (top + bottom) * 0.5f;
                    top = center - 1.0f;
                    bottom = center + 1.0f;
                }
                
                // 🔍 DEBUG: Log first few vertex positions for visibility
                if (geometryCreateCount <= 5 && currentVertex < 6) {
                    sLog_DebugGeometry("    📍 Body vertex positions:"
                             << "left=" << left << "right=" << right
                             << "top=" << top << "bottom=" << bottom);
                }
                
                vertices[currentVertex++].set(left, bottom);
                vertices[currentVertex++].set(right, bottom);
                vertices[currentVertex++].set(right, top);
                vertices[currentVertex++].set(right, top);
                vertices[currentVertex++].set(left, top);
                vertices[currentVertex++].set(left, bottom);
            } else {
                vertices[currentVertex++].set(candle.screenX, candle.bodyTop);
                vertices[currentVertex++].set(candle.screenX, candle.wickTop);
                vertices[currentVertex++].set(candle.screenX, candle.bodyBottom);
                vertices[currentVertex++].set(candle.screenX, candle.wickBottom);
            }
        }
        
        // 🚀 PHASE 4: GPU UPLOAD PROFILER - Track bytes sent to GPU
        size_t bytesUploaded = vertexCount * sizeof(QSGGeometry::Point2D);
        m_bytesUploadedThisFrame += bytesUploaded;
        
        // In Qt6 the geometry itself no longer exposes markDirty(). Instead we
        // notify the parent geometry node that its geometry has changed so the
        // scene graph can update. DirtyGeometry is the closest equivalent to
        // the removed DirtyVertexData flag.
        node->markDirty(QSGNode::DirtyGeometry);
    }
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
    // 🔥 UNIFIED COORD SYSTEM: Mirror GPUChartWidget::worldToScreen logic
    if (width() <= 0 || height() <= 0) {
        return QPointF(0, 0);
    }

    // Validate that we have a usable view window – fall back if not
    if (!m_coordinatesValid || m_viewEndTime_ms <= m_viewStartTime_ms) {
        return QPointF(0, 0);
    }

    // ──────────────── TIME → X ────────────────
    double timeRange  = static_cast<double>(m_viewEndTime_ms - m_viewStartTime_ms);
    double timeRatio  = static_cast<double>(timestamp_ms - m_viewStartTime_ms) / timeRange;

    // Allow slight over-drawing but keep within reasonable bounds for clamping
    const double kOvershoot = 0.05; // 5 % padding outside left/right
    if (timeRatio < -kOvershoot)   timeRatio = -kOvershoot;
    if (timeRatio > 1.0 + kOvershoot) timeRatio = 1.0 + kOvershoot;
    double x = timeRatio * width();

    // ──────────────── PRICE → Y ────────────────
    // Use the **exact** price window supplied by GPUChartWidget.  Do NOT adjust it on a
    // per-vertex basis – otherwise a single candle can end up stretched because its top
    // and bottom are mapped with different ranges.

    double priceRange = m_maxPrice - m_minPrice;
    if (priceRange <= 0.0) priceRange = 1.0; // Fallback – should never happen

    double priceRatio = (price - m_minPrice) / priceRange;
    // Clamp ratio to make sure we never produce NaN or inf
    priceRatio = std::clamp(priceRatio, -kOvershoot, 1.0 + kOvershoot);

    double y = (1.0 - priceRatio) * height();

    // 🔍 DEBUG (limited)
    static int dbgCount = 0;
    if (++dbgCount <= 10) {
        bool inViewport = (x >= 0 && x <= width() && y >= 0 && y <= height());
        sLog_DebugCoords("🗺️ UNIFIED COORD CALC #" << dbgCount
                 << "TS:" << timestamp_ms << "P:" << price
                 << "x:" << x << "y:" << y << "inViewport:" << (inViewport?"YES":"NO")
                 << "TimeRatio:" << timeRatio << "PriceRatio:" << priceRatio
                 << "TimeWin:" << m_viewStartTime_ms << "-" << m_viewEndTime_ms
                 << "PriceWin:" << m_minPrice << "-" << m_maxPrice);
    }

    return QPointF(x, y);
}

CandleLOD::TimeFrame CandlestickBatched::selectOptimalTimeFrame() const {
    if (!m_autoLOD) {
        return m_forcedTimeFrame;
    }
    
    // Calculate pixels per candle based on current view
    double pixelsPerCandle = calculateCurrentPixelsPerCandle();
    
    // 🚀 PHASE 1: VIEWPORT-AWARE LOD THRESHOLDS
    // Replace hardcoded pixel values with viewport-relative calculations
    // This ensures consistent LOD switching across different display resolutions (Retina, 4K, ultrawide)
    double viewportWidth = width();
    if (viewportWidth <= 0) viewportWidth = 1920.0; // Fallback for invalid widget size
    
    // Calculate viewport-relative thresholds (as fractions of viewport width)
    double dailyThreshold   = viewportWidth * 0.001;  // 0.1% of viewport width
    double hourlyThreshold  = viewportWidth * 0.0026; // 0.26% of viewport width  
    double min15Threshold   = viewportWidth * 0.0052; // 0.52% of viewport width
    double min5Threshold    = viewportWidth * 0.0104; // 1.04% of viewport width
    double min1Threshold    = viewportWidth * 0.0208; // 2.08% of viewport width
    
    // 🔥 BITCOIN-OPTIMIZED LOD: Avoid sub-second timeframes for crypto
    if (pixelsPerCandle < dailyThreshold)   return CandleLOD::TF_Daily;
    if (pixelsPerCandle < hourlyThreshold)  return CandleLOD::TF_60min;
    if (pixelsPerCandle < min15Threshold)   return CandleLOD::TF_15min;
    if (pixelsPerCandle < min5Threshold)    return CandleLOD::TF_5min;
    if (pixelsPerCandle < min1Threshold)    return CandleLOD::TF_1min;
    return CandleLOD::TF_1sec;  // 🕯️ MINIMUM: 1-second candles for crypto (never go below)
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
        sLog_Chart("🕯️ LOD CHANGED:" << CandleUtils::timeFrameName(lastTimeFrame) 
                 << "→" << CandleUtils::timeFrameName(newTimeFrame)
                 << "Pixels per candle:" << calculateCurrentPixelsPerCandle());
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

void CandlestickBatched::ensureCapacity(QSGGeometryNode* node, int wantedVertexCount,
                                       QSGGeometry::DrawingMode mode) {
    QSGGeometry* g = node->geometry();
    int allocCount = std::max(wantedVertexCount, 0);

    if (!g) {
        g = new QSGGeometry(QSGGeometry::defaultAttributes_Point2D(), allocCount);
        node->setGeometry(g);
        node->setFlag(QSGNode::OwnsGeometry);
        sLog_DebugGeometry("  - Created new geometry with" << allocCount << "vertices");
    } else if (allocCount != g->vertexCount()) {
        g->allocate(allocCount);
        sLog_DebugGeometry("  - Reallocated geometry to" << allocCount << "vertices");
    }

    // Ensure drawing mode is always correct (may differ between body/wick calls)
    if (g->drawingMode() != mode)
        g->setDrawingMode(mode);
}  