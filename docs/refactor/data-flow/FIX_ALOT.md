# Sentinel Performance Overhaul - Complete Implementation Guide

## 🎯 Mission: Stop Rebuilding The Universe On Every Update

**Current State:** Full geometry rebuild on every data update, pan, zoom, and parameter change  
**Target State:** Incremental append-only updates with transform-based viewport changes  
**Expected Improvement:** 10-100x additional speedup on top of your 2000x BlockingQueuedConnection fix

---

## 📋 Overview of Changes

You have **3 major systems** to refactor:

1. **UnifiedGridRenderer** - Add 4 dirty flags, stop marking geometry dirty for everything
2. **DataProcessor** - Stop clearing cells, implement incremental append, remove mutexes
3. **CellInstance/Rendering** - Store world coords only, compute screen coords in renderer

---

## Part 1: UnifiedGridRenderer - The 4 Dirty Flags System

### 🎯 Goal
Stop triggering full geometry rebuilds for operations that should only update transforms or materials.

### 📍 File: `UnifiedGridRenderer.h`

**ADD these member variables:**
```cpp
// Replace the single m_geometryDirty with 4 flags
std::atomic<bool> m_geometryDirty{false};     // Topology/LOD/mode changed (RARE)
std::atomic<bool> m_appendPending{false};     // New data arrived (COMMON)
std::atomic<bool> m_transformDirty{false};    // Pan/zoom/follow (VERY COMMON)
std::atomic<bool> m_materialDirty{false};     // Visual params changed (OCCASIONAL)
```

### 📍 File: `UnifiedGridRenderer.cpp`

**CHANGE 1: Fix onViewportChanged**
```cpp
// BEFORE:
void UnifiedGridRenderer::onViewportChanged() {
    m_geometryDirty.store(true);  // ❌ Wrong
    update();
}

// AFTER:
void UnifiedGridRenderer::onViewportChanged() {
    m_transformDirty.store(true);  // ✅ Just update transform
    update();
}
```

**CHANGE 2: Fix all pan/zoom methods**
```cpp
// BEFORE (ALL of these):
void UnifiedGridRenderer::panLeft() { 
    // ...
    m_geometryDirty.store(true);  // ❌ Wrong
    update(); 
}

// AFTER (apply to panLeft, panRight, panUp, panDown):
void UnifiedGridRenderer::panLeft() { 
    if (m_viewState) { 
        m_viewState->panLeft(); 
        m_transformDirty.store(true);  // ✅ Transform only
        update(); 
    } 
}

// AFTER (apply to zoomIn, zoomOut):
void UnifiedGridRenderer::zoomIn() { 
    if (m_viewState) { 
        float oldScale = m_viewState->getZoomLevel();
        m_viewState->handleZoomWithViewport(0.1, QPointF(width()/2, height()/2), QSizeF(width(), height()));
        float newScale = m_viewState->getZoomLevel();
        
        // Only rebuild if zoom crosses LOD threshold (you'll implement this check later)
        // For now, just use transform
        m_transformDirty.store(true);  // ✅ Transform only
        update(); 
    } 
}

// AFTER (wheelEvent):
void UnifiedGridRenderer::wheelEvent(QWheelEvent* event) { 
    if (m_viewState && isVisible() && m_viewState->isTimeWindowValid()) { 
        m_viewState->handleZoomWithSensitivity(event->angleDelta().y(), event->position(), QSizeF(width(), height())); 
        m_transformDirty.store(true);  // ✅ Transform only
        update(); 
        event->accept(); 
    } else event->ignore(); 
}
```

**CHANGE 3: Fix visual parameter setters**
```cpp
// BEFORE:
void UnifiedGridRenderer::setIntensityScale(double scale) {
    if (m_intensityScale != scale) {
        m_intensityScale = scale;
        m_geometryDirty.store(true);  // ❌ Wrong
        emit intensityScaleChanged();
        update();
    }
}

// AFTER:
void UnifiedGridRenderer::setIntensityScale(double scale) {
    if (m_intensityScale != scale) {
        m_intensityScale = scale;
        m_materialDirty.store(true);  // ✅ Material only
        emit intensityScaleChanged();
        update();
    }
}

// Apply same pattern to:
// - setShowVolumeProfile
// - setMinVolumeFilter
```

**CHANGE 4: Fix render mode changes (these SHOULD rebuild geometry)**
```cpp
// CORRECT - these should set geometryDirty:
void UnifiedGridRenderer::setRenderMode(RenderMode mode) {
    if (m_renderMode != mode) {
        m_renderMode = mode;
        m_geometryDirty.store(true);  // ✅ Correct - topology changed
        update();
        emit renderModeChanged();
    }
}

void UnifiedGridRenderer::setTimeframe(int timeframe_ms) {
    if (m_currentTimeframe_ms != timeframe_ms) {
        m_currentTimeframe_ms = timeframe_ms;
        m_manualTimeframeSet = true;
        m_manualTimeframeTimer.start();
        if (m_dataProcessor) m_dataProcessor->addTimeframe(timeframe_ms);
        m_geometryDirty.store(true);  // ✅ Correct - LOD changed
        update();
        emit timeframeChanged();
    }
}
```

**CHANGE 5: Fix the data update connection**
```cpp
// BEFORE (in initializeV2Architecture):
connect(m_dataProcessor.get(), &DataProcessor::dataUpdated, 
        this, [this]() { 
            m_geometryDirty.store(true);  // ❌ Wrong
            update();
        }, Qt::QueuedConnection);

// AFTER:
connect(m_dataProcessor.get(), &DataProcessor::dataUpdated, 
        this, [this]() { 
            m_appendPending.store(true);  // ✅ Append new data
            update();
        }, Qt::QueuedConnection);
```

**CHANGE 6: Rewrite updatePaintNode with 4-flag logic**
```cpp
// BEFORE:
QSGNode* UnifiedGridRenderer::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData* data) {
    // ...
    bool needsRebuild = m_geometryDirty.load() || isNewNode;
    
    if (needsRebuild) {
        updateVisibleCells();
        sceneNode->updateContent(batch, strategy);
        m_geometryDirty.store(false);
    }
    
    // Apply transform
    QMatrix4x4 transform;
    if (m_viewState) {
        QPointF pan = m_viewState->getPanVisualOffset();
        transform.translate(pan.x(), pan.y());
    }
    sceneNode->updateTransform(transform);
    
    return sceneNode;
}

// AFTER:
QSGNode* UnifiedGridRenderer::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData* data) {
    Q_UNUSED(data)
    if (width() <= 0 || height() <= 0) return oldNode;

    QElapsedTimer timer;
    timer.start();

    auto* sceneNode = static_cast<GridSceneNode*>(oldNode);
    bool isNewNode = !sceneNode;
    if (isNewNode) {
        sceneNode = new GridSceneNode();
        if (m_sentinelMonitor) m_sentinelMonitor->recordGeometryRebuild();
    }
    
    // Handle each dirty flag separately
    if (m_geometryDirty.exchange(false) || isNewNode) {
        // FULL REBUILD - rare, expensive
        sLog_Render("⚠️ Full geometry rebuild");
        updateVisibleCells();
        GridSliceBatch batch{m_visibleCells, m_intensityScale, m_minVolumeFilter, m_maxCells};
        IRenderStrategy* strategy = getCurrentStrategy();
        sceneNode->updateContent(batch, strategy);
        sceneNode->markDirty(QSGNode::DirtyGeometry);
        
        if (m_showVolumeProfile) {
            updateVolumeProfile();
            sceneNode->updateVolumeProfile(m_volumeProfile);
        }
        sceneNode->setShowVolumeProfile(m_showVolumeProfile);
    }
    
    if (m_appendPending.exchange(false)) {
        // INCREMENTAL UPDATE - common, cheap
        // TODO: Implement appendNewCells() in DataProcessor
        // For now, fallback to full update
        sLog_Render("📝 Append pending (fallback to update for now)");
        updateVisibleCells();
        GridSliceBatch batch{m_visibleCells, m_intensityScale, m_minVolumeFilter, m_maxCells};
        IRenderStrategy* strategy = getCurrentStrategy();
        sceneNode->updateContent(batch, strategy);
        sceneNode->markDirty(QSGNode::DirtyGeometry);
    }
    
    if (m_transformDirty.exchange(false) || isNewNode) {
        // UPDATE TRANSFORM - very cheap
        QMatrix4x4 transform;
        if (m_viewState) {
            QPointF pan = m_viewState->getPanVisualOffset();
            transform.translate(pan.x(), pan.y());
        }
        sceneNode->updateTransform(transform);
        sceneNode->markDirty(QSGNode::DirtyMatrix);
    }
    
    if (m_materialDirty.exchange(false)) {
        // UPDATE MATERIAL - cheap
        // TODO: Implement material updates in GridSceneNode
        sLog_Render("🎨 Material update");
        sceneNode->markDirty(QSGNode::DirtyMaterial);
    }
    
    if (m_sentinelMonitor) {
        m_sentinelMonitor->endFrame();
    }

    const qint64 totalUs = timer.nsecsElapsed() / 1000;
    sLog_RenderN(10, "UGR paint: total=" << totalUs << "µs cells=" << m_visibleCells.size());

    return sceneNode;
}
```

**CHANGE 7: Remove viewport size setting from mouse events**
```cpp
// BEFORE:
void UnifiedGridRenderer::mousePressEvent(QMouseEvent* event) { 
    if (m_viewState && isVisible() && event->button() == Qt::LeftButton) { 
        m_viewState->setViewportSize(width(), height());  // ❌ Remove this
        m_viewState->handlePanStart(event->position()); 
        event->accept(); 
    } else event->ignore(); 
}

// AFTER:
void UnifiedGridRenderer::mousePressEvent(QMouseEvent* event) { 
    if (m_viewState && isVisible() && event->button() == Qt::LeftButton) { 
        m_viewState->handlePanStart(event->position()); 
        event->accept(); 
    } else event->ignore(); 
}

// Same for mouseReleaseEvent - remove the setViewportSize call
void UnifiedGridRenderer::mouseReleaseEvent(QMouseEvent* event) { 
    if (m_viewState) { 
        m_viewState->handlePanEnd(); 
        event->accept(); 
        m_transformDirty.store(true);  // ✅ Changed from geometryDirty
        update();
    } 
}
```

**CHANGE 8: Remove the mutex lock from updatePaintNode**
```cpp
// BEFORE:
QSGNode* UnifiedGridRenderer::updatePaintNode(...) {
    // ...
    {
        std::lock_guard<std::mutex> lock(m_dataMutex);  // ❌ Remove this entire block
        bool needsRebuild = m_geometryDirty.load() || isNewNode;
        // ...
    }
}

// AFTER:
QSGNode* UnifiedGridRenderer::updatePaintNode(...) {
    // No mutex lock - use atomic flags instead
    // All flag checks use .exchange(false) which is atomic
}
```

---

## Part 2: DataProcessor - Stop Rebuilding Everything

### 🎯 Goal
Implement incremental append instead of clearing and rebuilding all cells on every update.

### 📍 File: `DataProcessor.hpp`

**ADD these member variables:**
```cpp
// Ring buffer management
static constexpr size_t MAX_VISIBLE_CELLS = 100000;  // Adjust based on memory
qint64 m_lastProcessedTime = 0;
bool m_viewportInitialized = false;

// Atomic pointers instead of mutex-protected data
std::atomic<std::shared_ptr<const OrderBook>> m_latestOrderBook;
std::atomic<std::shared_ptr<std::vector<CellInstance>>> m_publishedCells;

// Instance variable for snapshot timing (replace static)
qint64 m_lastSnapshotBucket = 0;

// Throttling
QElapsedTimer m_lastEmitTime;
static constexpr int MIN_EMIT_INTERVAL_MS = 16;  // ~60 FPS max

// Remove these mutex-protected variables:
// std::mutex m_dataMutex;  // ❌ Remove
// std::mutex m_snapshotMutex;  // ❌ Remove
// std::shared_ptr<const OrderBook> m_latestOrderBook;  // ❌ Remove (replaced with atomic)
```

### 📍 File: `DataProcessor.cpp`

**CHANGE 1: Replace mutex locks with atomic operations**
```cpp
// BEFORE:
void DataProcessor::onOrderBookUpdated(std::shared_ptr<const OrderBook> book) {
    {
        std::lock_guard<std::mutex> lock(m_dataMutex);  // ❌ Remove
        m_latestOrderBook = book;
        m_hasValidOrderBook = true;
    }
    updateVisibleCells();
}

// AFTER:
void DataProcessor::onOrderBookUpdated(std::shared_ptr<const OrderBook> book) {
    m_latestOrderBook.store(book);  // ✅ Atomic swap, no lock
    m_hasValidOrderBook = true;
    updateVisibleCells();
}

// BEFORE:
const OrderBook& DataProcessor::getLatestOrderBook() const {
    std::lock_guard<std::mutex> lock(m_dataMutex);  // ❌ Remove
    static OrderBook emptyBook;
    return m_latestOrderBook ? *m_latestOrderBook : emptyBook;
}

// AFTER:
const OrderBook& DataProcessor::getLatestOrderBook() const {
    auto book = m_latestOrderBook.load();  // ✅ Atomic load, no lock
    static OrderBook emptyBook;
    return book ? *book : emptyBook;
}
```

**CHANGE 2: Rewrite updateVisibleCells to append instead of rebuild**
```cpp
// BEFORE:
void DataProcessor::updateVisibleCells() {
    m_visibleCells.clear();  // ❌ DESTROYS EVERYTHING
    
    if (!m_viewState || !m_viewState->isTimeWindowValid()) return;
    
    // ... lots of processing ...
    
    auto visibleSlices = m_liquidityEngine->getVisibleSlices(activeTimeframe, timeStart, timeEnd);
    
    for (const auto* slice : visibleSlices) {
        createCellsFromLiquiditySlice(*slice);  // Creates ALL cells
    }
    
    // Publish snapshot
    {
        std::lock_guard<std::mutex> snapLock(m_snapshotMutex);
        m_publishedCells = std::make_shared<std::vector<CellInstance>>(m_visibleCells);
    }
    emit dataUpdated();
}

// AFTER:
void DataProcessor::updateVisibleCells() {
    // DON'T clear - implement incremental append
    
    if (!m_viewState || !m_viewState->isTimeWindowValid()) return;
    
    int64_t activeTimeframe = m_currentTimeframe_ms;
    
    // Auto-suggest timeframe only if not manually set
    if (!m_manualTimeframeSet && m_liquidityEngine) {
        int64_t optimalTimeframe = m_liquidityEngine->suggestTimeframe(
            m_viewState->getVisibleTimeStart(), m_viewState->getVisibleTimeEnd(), 2000);
        
        if (optimalTimeframe != m_currentTimeframe_ms) {
            m_currentTimeframe_ms = optimalTimeframe;
            activeTimeframe = optimalTimeframe;
            sLog_Render("🔄 AUTO-TIMEFRAME: " << optimalTimeframe << "ms");
        }
    }
    
    if (m_liquidityEngine) {
        qint64 timeStart = m_viewState->getVisibleTimeStart();
        qint64 timeEnd = m_viewState->getVisibleTimeEnd();
        
        // Get NEW slices only (since last update)
        qint64 queryStart = m_lastProcessedTime > 0 ? m_lastProcessedTime : timeStart;
        
        auto newSlices = m_liquidityEngine->getVisibleSlices(activeTimeframe, queryStart, timeEnd);
        
        sLog_Render("🔍 LTSE QUERY: timeframe=" << activeTimeframe << "ms, window=[" << queryStart << "-" << timeEnd << "]");
        sLog_Render("🔍 LTSE RESULT: Found " << newSlices.size() << " new slices");
        
        // Only process NEW slices
        int processedCount = 0;
        for (const auto* slice : newSlices) {
            if (slice->startTime_ms > m_lastProcessedTime) {
                createCellsFromLiquiditySlice(*slice);
                processedCount++;
            }
        }
        
        if (processedCount > 0) {
            sLog_Render("📝 Appended " << processedCount << " new slices");
            m_lastProcessedTime = timeEnd;
        }
        
        // Ring buffer: remove old cells if buffer too large
        if (m_visibleCells.size() > MAX_VISIBLE_CELLS) {
            size_t removeCount = m_visibleCells.size() - MAX_VISIBLE_CELLS;
            m_visibleCells.erase(m_visibleCells.begin(), m_visibleCells.begin() + removeCount);
            sLog_Render("🗑️ Ring buffer cleanup: removed " << removeCount << " old cells");
        }
        
        // Only auto-adjust viewport on FIRST initialization
        if (newSlices.empty() && !m_viewportInitialized) {
            auto allSlices = m_liquidityEngine->getVisibleSlices(activeTimeframe, 0, LLONG_MAX);
            if (!allSlices.empty()) {
                qint64 newestTime = allSlices.back()->endTime_ms;
                qint64 newStart = newestTime - 30000;
                qint64 newEnd = newestTime + 30000;
                
                m_viewState->setViewport(newStart, newEnd, m_viewState->getMinPrice(), m_viewState->getMaxPrice());
                m_viewportInitialized = true;
                sLog_Render("🎯 Initial viewport set to data range");
            }
        }
        
        sLog_Render("🎯 DATA PROCESSOR COVERAGE TotalCells:" << m_visibleCells.size()
                     << " ActiveTimeframe:" << activeTimeframe << "ms");
    }
    
    // Publish snapshot atomically (no mutex)
    m_publishedCells.store(std::make_shared<std::vector<CellInstance>>(m_visibleCells));
    
    // Throttled emit
    if (!m_lastEmitTime.isValid() || m_lastEmitTime.elapsed() >= MIN_EMIT_INTERVAL_MS) {
        emit dataUpdated();
        m_lastEmitTime.restart();
    }
}
```

**CHANGE 3: Fix the static state in captureOrderBookSnapshot**
```cpp
// BEFORE:
void DataProcessor::captureOrderBookSnapshot() {
    // ...
    static qint64 lastBucket = 0;  // ❌ Static state is bad
    
    if (lastBucket == 0) {
        // ...
    }
    
    if (bucketStart > lastBucket) {
        for (qint64 ts = lastBucket + 100; ts <= bucketStart; ts += 100) {
            // Backfill loop - can create 100+ snapshots at once
        }
    }
}

// AFTER:
void DataProcessor::captureOrderBookSnapshot() {
    if (!m_liquidityEngine) return;
    
    auto book_copy = m_latestOrderBook.load();  // Atomic load
    if (!book_copy) return;

    const qint64 nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const qint64 bucketStart = (nowMs / 100) * 100;

    // Only create ONE snapshot per call, don't backfill
    if (bucketStart > m_lastSnapshotBucket) {
        OrderBook ob = *book_copy;
        ob.timestamp = std::chrono::system_clock::time_point(std::chrono::milliseconds(bucketStart));
        m_liquidityEngine->addOrderBookSnapshot(ob);
        m_lastSnapshotBucket = bucketStart;
        updateVisibleCells();
    }
}
```

**CHANGE 4: Cull in world space BEFORE computing screen coords**
```cpp
// BEFORE:
void DataProcessor::createLiquidityCell(...) {
    // ... create cell ...
    cell.screenRect = timeSliceToScreenRect(slice, price);  // Compute first
    
    // Then cull
    if (!viewportRect.intersects(cell.screenRect)) {
        return;  // Too late, already did the math
    }
}

// AFTER:
void DataProcessor::createLiquidityCell(const LiquidityTimeSlice& slice, double price, double liquidity, bool isBid) {
    if (!m_viewState || liquidity <= 0.0) return;
    
    // Cull in WORLD space first (cheap)
    if (price < m_viewState->getMinPrice() || price > m_viewState->getMaxPrice()) {
        return;  // Outside visible price range
    }
    
    if (slice.endTime_ms < m_viewState->getVisibleTimeStart() || 
        slice.startTime_ms > m_viewState->getVisibleTimeEnd()) {
        return;  // Outside visible time range
    }
    
    // Only compute screen coords for visible cells
    CellInstance cell;
    cell.timeSlot = slice.startTime_ms;
    cell.priceLevel = price;
    cell.liquidity = liquidity;
    cell.isBid = isBid;
    cell.intensity = std::min(1.0, liquidity / 1000.0);
    cell.color = isBid ? QColor(0, 255, 0, 128) : QColor(255, 0, 0, 128);
    cell.screenRect = timeSliceToScreenRect(slice, price);
    
    // Basic sanity checks only (no expensive viewport intersection)
    if (cell.screenRect.width() < 0.5 || cell.screenRect.height() < 0.5) return;
    if (cell.screenRect.width() > 200.0) cell.screenRect.setWidth(200.0);
    if (cell.screenRect.height() > 200.0) cell.screenRect.setHeight(200.0);
    
    m_visibleCells.push_back(cell);
    
    if constexpr (kTraceCellDebug) {
        static int cellCounter = 0;
        if (++cellCounter % 50 == 0) {
            sLog_Render("🎯 CELL DEBUG: Created cell #" << cellCounter);
        }
    }
}
```

**CHANGE 5: Fix the manual timeframe timeout logic**
```cpp
// BEFORE:
void DataProcessor::updateVisibleCells() {
    // This runs EVERY call (hundreds per second)
    if (!m_manualTimeframeSet || 
        (m_manualTimeframeTimer.isValid() && m_manualTimeframeTimer.elapsed() > 10000)) {
        // Auto-suggest
    }
}

// AFTER:
// The timeout logic is REMOVED from updateVisibleCells (see CHANGE 2 above)
// Manual timeframe now stays manual until explicitly reset

void DataProcessor::setTimeframe(int timeframe_ms) {
    if (timeframe_ms > 0) {
        m_currentTimeframe_ms = timeframe_ms;
        m_manualTimeframeSet = true;
        // No timer - stays manual forever until reset
        
        if (m_liquidityEngine) {
            m_liquidityEngine->addTimeframe(timeframe_ms);
        }
        
        sLog_Render("🎯 MANUAL TIMEFRAME: " << timeframe_ms << "ms");
        emit dataUpdated();
    }
}

// Add a reset function to header and implement:
void DataProcessor::resetToAutoTimeframe() {
    m_manualTimeframeSet = false;
    sLog_Render("🔄 AUTO-TIMEFRAME: Enabled");
    emit dataUpdated();
}
```

**CHANGE 6: Remove dead code blocks**
```cpp
// FIND AND DELETE this entire unused block:
if (m_useDenseIngestion) {
    // ... 50+ lines of unused dense ingestion code ...
    return;
}

// This code is never executed because m_useDenseIngestion is never set to true
// Delete it to reduce confusion
```

---

## Part 3: CellInstance - Store World Coords Only

### 🎯 Goal
Remove screen coordinates from data layer, compute them in renderer using transform matrix.

### 📍 File: `GridTypes.hpp`

**CHANGE: Modify CellInstance struct**
```cpp
// BEFORE:
struct CellInstance {
    qint64 timeSlot;
    double priceLevel;
    double liquidity;
    bool isBid;
    double intensity;
    QColor color;
    QRectF screenRect;  // ❌ Screen coords don't belong here
};

// AFTER:
struct CellInstance {
    // World coordinates only
    qint64 timeStart_ms;   // Start of time slice
    qint64 timeEnd_ms;     // End of time slice
    double priceMin;       // Bottom of price level
    double priceMax;       // Top of price level
    
    // Display data
    double liquidity;
    bool isBid;
    double intensity;
    QColor color;
    
    // NO screenRect - compute in renderer
};
```

### 📍 File: `DataProcessor.cpp`

**CHANGE: Update createLiquidityCell to store world coords**
```cpp
// BEFORE:
void DataProcessor::createLiquidityCell(...) {
    CellInstance cell;
    cell.timeSlot = slice.startTime_ms;
    cell.priceLevel = price;
    // ...
    cell.screenRect = timeSliceToScreenRect(slice, price);  // ❌ Remove
    m_visibleCells.push_back(cell);
}

// AFTER:
void DataProcessor::createLiquidityCell(const LiquidityTimeSlice& slice, double price, double liquidity, bool isBid) {
    if (!m_viewState || liquidity <= 0.0) return;
    
    // Cull in world space
    if (price < m_viewState->getMinPrice() || price > m_viewState->getMaxPrice()) return;
    if (slice.endTime_ms < m_viewState->getVisibleTimeStart() || 
        slice.startTime_ms > m_viewState->getVisibleTimeEnd()) return;
    
    CellInstance cell;
    // Store WORLD coordinates only
    cell.timeStart_ms = slice.startTime_ms;
    cell.timeEnd_ms = slice.endTime_ms > slice.startTime_ms ? slice.endTime_ms : slice.startTime_ms + m_currentTimeframe_ms;
    
    const double halfTick = slice.tickSize * 0.5;
    cell.priceMin = price - halfTick;
    cell.priceMax = price + halfTick;
    
    // Display properties
    cell.liquidity = liquidity;
    cell.isBid = isBid;
    cell.intensity = std::min(1.0, liquidity / 1000.0);
    cell.color = isBid ? QColor(0, 255, 0, 128) : QColor(255, 0, 0, 128);
    
    m_visibleCells.push_back(cell);
}
```

**REMOVE: The timeSliceToScreenRect function**
```cpp
// Delete this entire function - no longer needed
// QRectF DataProcessor::timeSliceToScreenRect(...) { ... }
```

### 📍 File: `HeatmapStrategy.cpp` (or wherever you create geometry)

**CHANGE: Compute screen coords in renderer**
```cpp
// BEFORE (in your strategy's render method):
void HeatmapStrategy::render(...) {
    for (const auto& cell : batch.cells) {
        // cell.screenRect already computed
        float x = cell.screenRect.x();
        float y = cell.screenRect.y();
        float w = cell.screenRect.width();
        float h = cell.screenRect.height();
        // Create vertices...
    }
}

// AFTER:
void HeatmapStrategy::render(GridSceneNode* node, const GridSliceBatch& batch, GridViewState* viewState) {
    if (!viewState) return;
    
    // Get viewport for coordinate transformation
    Viewport viewport{
        viewState->getVisibleTimeStart(),
        viewState->getVisibleTimeEnd(),
        viewState->getMinPrice(),
        viewState->getMaxPrice(),
        viewState->getViewportWidth(),
        viewState->getViewportHeight()
    };
    
    // Create geometry node
    auto* geomNode = createOrGetGeometryNode(node);
    QSGGeometry* geometry = geomNode->geometry();
    
    // Allocate vertices for all cells
    int vertexCount = batch.cells.size() * 4;  // 4 vertices per cell (quad)
    if (geometry->vertexCount() != vertexCount) {
        geometry->allocate(vertexCount);
    }
    
    auto* vertices = geometry->vertexDataAsColoredPoint2D();
    int vIdx = 0;
    
    for (const auto& cell : batch.cells) {
        // Convert world coords to screen coords at render time
        QPointF topLeft = CoordinateSystem::worldToScreen(
            cell.timeStart_ms, cell.priceMax, viewport);
        QPointF bottomRight = CoordinateSystem::worldToScreen(
            cell.timeEnd_ms, cell.priceMin, viewport);
        
        float x = topLeft.x();
        float y = topLeft.y();
        float w = bottomRight.x() - topLeft.x();
        float h = bottomRight.y() - topLeft.y();
        
        // Convert color to uint
        QColor color = cell.color;
        uint rgba = (uint(color.alpha()) << 24) |
                   (uint(color.red()) << 16) |
                   (uint(color.green()) << 8) |
                   uint(color.blue());
        
        // Create 4 vertices for this quad
        vertices[vIdx++].set(x, y, rgba);           // Top-left
        vertices[vIdx++].set(x + w, y, rgba);       // Top-right
        vertices[vIdx++].set(x, y + h, rgba);       // Bottom-left
        vertices[vIdx++].set(x + w, y + h, rgba);   // Bottom-right
    }
    
    geomNode->markDirty(QSGNode::DirtyGeometry);
}
```

---

## Part 4: Additional Fixes

### 📍 Fix: Thread shutdown safety

**File: `UnifiedGridRenderer.cpp`**

```cpp
// BEFORE:
UnifiedGridRenderer::~UnifiedGridRenderer() {
    if (m_dataProcessorThread && m_dataProcessorThread->isRunning()) {
        m_dataProcessorThread->quit();
        m_dataProcessorThread->wait(5000);  // Might leak if doesn't stop
    }
}

// AFTER:
UnifiedGridRenderer::~UnifiedGridRenderer() {
    // Signal processor to stop first
    if (m_dataProcessor) {
        QMetaObject::invokeMethod(m_dataProcessor.get(), 
                                 &DataProcessor::stopProcessing, 
                                 Qt::BlockingQueuedConnection);
    }
    
    // Then stop thread
    if (m_dataProcessorThread && m_dataProcessorThread->isRunning()) {
        m_dataProcessorThread->quit();
        if (!m_dataProcessorThread->wait(1000)) {
            qWarning() << "DataProcessor thread didn't stop, terminating";
            m_dataProcessorThread->terminate();
            m_dataProcessorThread->wait();
        }
    }
}
```

### 📍 Fix: Remove double-panning

**File: `UnifiedGridRenderer.cpp`**

```cpp
// BEFORE:
QPointF UnifiedGridRenderer::worldToScreen(qint64 timestamp_ms, double price) const {
    // ...
    QPointF screenPos = CoordinateSystem::worldToScreen(timestamp_ms, price, viewport);
    
    // Apply visual pan offset
    QPointF panOffset = getPanVisualOffset();
    screenPos.setX(screenPos.x() + panOffset.x());  // ❌ Double-panning
    screenPos.setY(screenPos.y() + panOffset.y());
    
    return screenPos;
}

// AFTER:
QPointF UnifiedGridRenderer::worldToScreen(qint64 timestamp_ms, double price) const {
    if (!m_viewState) return QPointF();
    
    Viewport viewport{
        m_viewState->getVisibleTimeStart(),
        m_viewState->getVisibleTimeEnd(),
        m_viewState->getMinPrice(),
        m_viewState->getMaxPrice(),
        width(),
        height()
    };
    
    // NO pan offset - that's applied by the transform node
    return CoordinateSystem::worldToScreen(timestamp_ms, price, viewport);
}

// Same for screenToWorld:
QPointF UnifiedGridRenderer::screenToWorld(double screenX, double screenY) const {
    if (!m_viewState) return QPointF();
    
    Viewport viewport{
        m_viewState->getVisibleTimeStart(),
        m_viewState->getVisibleTimeEnd(),
        m_viewState->getMinPrice(),
        m_viewState->getMaxPrice(),
        width(),
        height()
    };
    
    // NO pan offset removal - transform node handles it
    return CoordinateSystem::screenToWorld(QPointF(screenX, screenY), viewport);
}
```

### 📍 Fix: Fix onLiveOrderBookUpdated mutex locks

**File: `DataProcessor.cpp`**

```cpp
// BEFORE:
void DataProcessor::onLiveOrderBookUpdated(...) {
    // ... lots of processing ...
    
    if (!sparseBook.bids.empty() || !sparseBook.asks.empty()) {
        m_liquidityEngine->addOrderBookSnapshot(sparseBook);
        {
            std::lock_guard<std::mutex> lock(m_dataMutex);  // ❌ Remove
            m_latestOrderBook = std::make_shared<OrderBook>(sparseBook);
            m_hasValidOrderBook = true;
        }
        // ...
    }
    updateVisibleCells();
}

// AFTER:
void DataProcessor::onLiveOrderBookUpdated(...) {
    // ... lots of processing ...
    
    if (!sparseBook.bids.empty() || !sparseBook.asks.empty()) {
        m_liquidityEngine->addOrderBookSnapshot(sparseBook);
        
        // Atomic store, no lock
        m_latestOrderBook.store(std::make_shared<OrderBook>(sparseBook));
        m_hasValidOrderBook = true;
        
        sLog_Data("🎯 DataProcessor: Primed LTSE with banded snapshot - bids=" 
                 << sparseBook.bids.size() << " asks=" << sparseBook.asks.size());
    }
    updateVisibleCells();
}
```

---

## 📊 Testing Checklist

After implementing all changes, verify:

### ✅ Phase 1: Dirty Flags Working
```bash
# Test: Pan left/right
# Expected: transformDirty fires, NO geometry rebuild
# Log should show: "🎯 Transform update" NOT "⚠️ Full geometry rebuild"

# Test: Zoom in/out
# Expected: transformDirty fires, NO geometry rebuild
# Log should show: "🎯 Transform update" NOT "⚠️ Full geometry rebuild"

# Test: Change intensity slider
# Expected: materialDirty fires, NO geometry rebuild
# Log should show: "🎨 Material update" NOT "⚠️ Full geometry rebuild"

# Test: Switch render mode
# Expected: geometryDirty fires, DOES rebuild
# Log should show: "⚠️ Full geometry rebuild"
```

### ✅ Phase 2: Incremental Append Working
```bash
# Test: Let data stream for 10 seconds
# Expected: m_visibleCells grows incrementally
# Log should show: "📝 Appended X new slices" and cell count increasing
# Should NOT show: "🗑️ Ring buffer cleanup" until buffer fills

# Test: Let data stream for 5 minutes
# Expected: Ring buffer kicks in, old cells removed
# Log should show: "🗑️ Ring buffer cleanup: removed X old cells"
```

### ✅ Phase 3: Performance Verification
```bash
# Test: With 1000 updates/sec
# Expected: Paint time <0.1ms for transform-only updates
# Expected: dataUpdated emits at ~60 Hz max (throttled)

# Use the logs:
# sLog_RenderN(10, "UGR paint: total=" << totalUs << "µs")
# Should see: ~50-100µs for transform updates
# Should see: ~500-1000µs only for full rebuilds (rare)
```

### ✅ Phase 4: No Regressions
```bash
# Test: Startup sequence
# Expected: No crashes, viewport initializes correctly
# Expected: No "left shift" issue

# Test: Data arrives before viewport set
# Expected: Viewport auto-initializes from first data (once only)

# Test: Pan during heavy data flow
# Expected: Smooth panning, no stuttering

# Test: Zoom out to see 1000+ cells
# Expected: No wraparound, no disappearing columns
```

---

## 🎯 Implementation Order (The Plan)

### Tonight's Work Session:

**Hour 1: UnifiedGridRenderer Dirty Flags (Easy Wins) - 45 min**
1. ✅ Add 4 atomic flags to header (5 min)
2. ✅ Change all pan/zoom methods to use transformDirty (15 min)
3. ✅ Change data update connection to use appendPending (5 min)
4. ✅ Change visual param setters to use materialDirty (10 min)
5. ✅ Remove viewport size calls from mouse events (5 min)
6. ✅ Test: Verify logs show correct flags firing (5 min)

**Hour 2: updatePaintNode Rewrite - 60 min**
1. ✅ Implement 4-flag branching logic (30 min)
2. ✅ Remove mutex lock from updatePaintNode (10 min)
3. ✅ Add logging to each branch (10 min)
4. ✅ Test: Verify each flag triggers correct path (10 min)

**Hour 3: DataProcessor Mutex Removal - 60 min**
1. ✅ Add atomic member variables to header (10 min)
2. ✅ Replace mutex locks with atomic operations in all methods (20 min)
3. ✅ Fix captureOrderBookSnapshot static state (10 min)
4. ✅ Add throttling variables (5 min)
5. ✅ Test: Verify no deadlocks or race conditions (15 min)

**Hour 4: DataProcessor Append Logic - 90 min**
1. ✅ Rewrite updateVisibleCells with append logic (40 min)
2. ✅ Add ring buffer cleanup (15 min)
3. ✅ Add emit throttling (10 min)
4. ✅ Fix manual timeframe logic (10 min)
5. ✅ Remove dead code blocks (5 min)
6. ✅ Test: Verify incremental append works (10 min)

**Hour 5: World Coords + Cleanup - 60 min**
1. ✅ Modify CellInstance struct (remove screenRect) (10 min)
2. ✅ Update createLiquidityCell (store world coords) (20 min)
3. ✅ Remove timeSliceToScreenRect function (5 min)
4. ✅ Update render strategy (compute screen coords) (20 min)
5. ✅ Test: Verify rendering still works (5 min)

**Hour 6: Final Fixes + Testing - 60 min**
1. ✅ Fix thread shutdown (10 min)
2. ✅ Fix double-panning (10 min)
3. ✅ Remove viewport initialization logic (10 min)
4. ✅ Run full testing checklist (20 min)
5. ✅ Profile and measure improvements (10 min)

---

## 🚀 Expected Results

### Before (Current State):
```
Paint time: 0.7ms (full rebuild every frame)
Updates/sec: 100-200 (throttled by rebuilds)
FPS: 28-30 (struggling)
CPU: 40-60% (constant recomputation)
Slice coverage: 54 slices
```

### After (Target State):
```
Paint time: 0.05ms (transform-only updates)
Paint time: 0.7ms (only on full rebuild - rare)
Updates/sec: 1000+ (limited only by network)
FPS: 60 (solid, no drops)
CPU: 10-20% (minimal work)
Slice coverage: 500+ slices
```

### The Gains:
- **10-15x faster paint time** for normal updates
- **No rebuilds during pan/zoom** (butter smooth)
- **Ring buffer efficiency** (constant memory)
- **No mutex contention** (lock-free performance)
- **GPU-friendly transforms** (matrix math is free)
- **10x more data visible** on screen simultaneously

---

## 💡 Pro Tips

1. **Test incrementally** - Don't implement everything at once. Do UnifiedGridRenderer flags first, verify they work, THEN move to DataProcessor.

2. **Use the logs** - Your `sLog_Render` statements will tell you exactly which code paths are firing. If you see "⚠️ Full geometry rebuild" during a pan, you know you missed a spot.

3. **Profile before/after** - Use Qt Creator's profiler or just the timing logs to measure actual improvement. Screenshots of the before/after will be sick content for copeharderpls.

4. **Git commit after each phase** - That way if something breaks, you can bisect and find exactly which change caused it.

5. **The mutex removal is critical** - Atomics are actually easier to reason about than mutexes. No lock ordering issues, no deadlocks, just clean atomic swaps.

6. **World coords are the future** - Once you move to world coords, adding features like dynamic LOD and zoom-dependent LOD becomes trivial.

7. **Don't panic if things break** - You're doing major surgery on hot paths. If something breaks, revert that commit and try a smaller change.

---

## 📝 Summary of All Changes

### Files to Modify:

1. **UnifiedGridRenderer.h**
   - Add 4 atomic bool flags

2. **UnifiedGridRenderer.cpp**
   - Change 8 methods to use correct flags
   - Rewrite updatePaintNode with 4-flag logic
   - Remove mutex lock
   - Remove viewport size calls
   - Fix destructor
   - Fix coordinate conversion (remove double-pan)

3. **DataProcessor.hpp**
   - Add atomic pointers
   - Add ring buffer vars
   - Add throttling vars
   - Remove mutexes
   - Add new member vars

4. **DataProcessor.cpp**
   - Replace ALL mutex locks with atomics (6 locations)
   - Rewrite updateVisibleCells (append not rebuild)
   - Fix captureOrderBookSnapshot (remove static)
   - Remove timeout logic from manual timeframe
   - Add world-space culling before screen coord computation
   - Remove dead dense ingestion code
   - Add throttling to emit

5. **GridTypes.hpp**
   - Modify CellInstance struct (world coords only)

6. **HeatmapStrategy.cpp** (or your render strategy)
   - Compute screen coords from world coords at render time
   - Use CoordinateSystem::worldToScreen in render loop

### Functions to Delete:
- `DataProcessor::timeSliceToScreenRect()` - no longer needed

### Functions to Add:
- `DataProcessor::resetToAutoTimeframe()` - allow user to exit manual mode