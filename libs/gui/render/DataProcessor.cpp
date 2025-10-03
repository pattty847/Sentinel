/*
Sentinel — DataProcessor
Role: Implements the background data processing loop for the V2 rendering architecture.
Inputs/Outputs: Queues incoming data and processes it in batches using LiquidityTimeSeriesEngine.
Threading: The constructor moves the object to a worker QThread; processQueue runs on that thread.
Performance: A timer-driven loop processes data in batches, trading latency for throughput.
Integration: The concrete implementation of the V2 asynchronous data processing pipeline.
Observability: Logs the number of items processed in each batch via sLog_Render.
Related: DataProcessor.hpp, LiquidityTimeSeriesEngine.h.
Assumptions: The processing interval is a good balance between latency and efficiency.
*/
#include "DataProcessor.hpp"
#include "GridViewState.hpp"
#include "../../core/SentinelLogging.hpp"
#include "../../core/DataCache.hpp"  // 🚀 PHASE 3: Access to dense LiveOrderBook
#include "../CoordinateSystem.h"  // 🎯 FIX: Use proper coordinate transformation
#include <QColor>
#include <chrono>
#include <limits>
#include <climits>
#include <cmath>

DataProcessor::DataProcessor(QObject* parent)
    : QObject(parent) {
    
    m_snapshotTimer = new QTimer(this);
    connect(m_snapshotTimer, &QTimer::timeout, this, &DataProcessor::captureOrderBookSnapshot);
    
    // 🚀 PHASE 3: DataProcessor now owns the LiquidityTimeSeriesEngine!
    m_liquidityEngine = new LiquidityTimeSeriesEngine(this);
    
    sLog_App("🚀 DataProcessor: Initialized for V2 architecture");
}

DataProcessor::~DataProcessor() {
    stopProcessing();
}

void DataProcessor::startProcessing() {
    // 🚀 Base 100ms sampler: ensure continuous time buckets
    sLog_App("🚀 DataProcessor: Starting 100ms base sampler");
    if (m_snapshotTimer && !m_snapshotTimer->isActive()) {
        m_snapshotTimer->start(100);
        sLog_App("🚀 DataProcessor: Started processing with 100ms snapshots");
    }
}

void DataProcessor::stopProcessing() {
    if (m_snapshotTimer) {
        m_snapshotTimer->stop();
    }
}

void DataProcessor::onTradeReceived(const Trade& trade) {
    if (trade.product_id.empty()) return;
    
    auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        trade.timestamp.time_since_epoch()).count();
    
    {
        std::lock_guard<std::mutex> lock(m_dataMutex);
        
        if (m_viewState && !m_viewState->isTimeWindowValid()) {
            initializeViewportFromTrade(trade);
        }
        
        sLog_Data("📊 DataProcessor TRADE UPDATE: Processing trade");
    }
    
    emit dataUpdated();
    
    sLog_Data("🎯 DataProcessor TRADE: $" << trade.price 
                 << " vol:" << trade.size 
                 << " timestamp:" << timestamp);
}

void DataProcessor::onOrderBookUpdated(std::shared_ptr<const OrderBook> book) {
    if (!book || book->product_id.empty() || book->bids.empty() || book->asks.empty()) return;
    
    {
        std::lock_guard<std::mutex> lock(m_dataMutex);
        
        m_latestOrderBook = book;
        m_hasValidOrderBook = true;
        
        if (m_viewState && !m_viewState->isTimeWindowValid()) {
            initializeViewportFromOrderBook(*book);
        }
    }
    
    emit dataUpdated();
    
    sLog_Data("🎯 DataProcessor ORDER BOOK update"
             << " Bids:" << book->bids.size() << " Asks:" << book->asks.size());
}

const OrderBook& DataProcessor::getLatestOrderBook() const {
    std::lock_guard<std::mutex> lock(m_dataMutex);
    // This might be problematic if m_latestOrderBook is null
    // but for now we assume it's valid when called.
    static OrderBook emptyBook;
    return m_latestOrderBook ? *m_latestOrderBook : emptyBook;
}

void DataProcessor::captureOrderBookSnapshot() {
    if (!m_liquidityEngine) return;
    
    std::shared_ptr<const OrderBook> book_copy;
    {
        std::lock_guard<std::mutex> lock(m_dataMutex);
        book_copy = m_latestOrderBook;
    }
    if (!book_copy) return;

    // Align to system 100ms buckets; carry-forward if we skipped buckets
    const qint64 nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const qint64 bucketStart = (nowMs / 100) * 100;

    static qint64 lastBucket = 0;
    if (lastBucket == 0) {
        OrderBook ob = *book_copy;
        ob.timestamp = std::chrono::system_clock::time_point(std::chrono::milliseconds(bucketStart));
        m_liquidityEngine->addOrderBookSnapshot(ob);
        lastBucket = bucketStart;
        emit dataUpdated();
        return;
    }

    if (bucketStart > lastBucket) {
        for (qint64 ts = lastBucket + 100; ts <= bucketStart; ts += 100) {
            OrderBook ob = *book_copy;
            ob.timestamp = std::chrono::system_clock::time_point(std::chrono::milliseconds(ts));
            m_liquidityEngine->addOrderBookSnapshot(ob);
            lastBucket = ts;
        }
        emit dataUpdated();
    }
}

void DataProcessor::initializeViewportFromTrade(const Trade& trade) {
    if (!m_viewState) return;
    
    auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        trade.timestamp.time_since_epoch()).count();
    
    qint64 timeStart = timestamp - 30000;
    qint64 timeEnd = timestamp + 30000;
    double minPrice = trade.price - 100.0;
    double maxPrice = trade.price + 100.0;
    
    m_viewState->setViewport(timeStart, timeEnd, minPrice, maxPrice);
    
    sLog_App("🎯 DataProcessor VIEWPORT FROM TRADE: $" << minPrice << "-$" << maxPrice << " at " << timestamp);
    
    emit viewportInitialized();
}

void DataProcessor::initializeViewportFromOrderBook(const OrderBook& book) {
    if (!m_viewState) return;
    
    double bestBidPrice = std::numeric_limits<double>::quiet_NaN();
    if (!book.bids.empty()) {
        // Find the highest bid price (bids should be sorted highest to lowest)
        bestBidPrice = book.bids[0].price;
    }

    double bestAskPrice = std::numeric_limits<double>::quiet_NaN();
    if (!book.asks.empty()) {
        // Find the lowest ask price (asks should be sorted lowest to highest)  
        bestAskPrice = book.asks[0].price;
    }

    double midPrice;
    if (!std::isnan(bestBidPrice) && !std::isnan(bestAskPrice)) {
        midPrice = (bestBidPrice + bestAskPrice) / 2.0;
    } else if (!std::isnan(bestBidPrice)) {
        midPrice = bestBidPrice;
    } else if (!std::isnan(bestAskPrice)) {
        midPrice = bestAskPrice;
    } else {
        midPrice = 100000.0; // Default fallback for BTC
    }
    
    // Use the order book timestamp instead of system time for proper alignment
    auto bookTime = std::chrono::duration_cast<std::chrono::milliseconds>(
        book.timestamp.time_since_epoch()).count();
    
    qint64 timeStart = bookTime - 30000;
    qint64 timeEnd = bookTime + 30000;
    double minPrice = midPrice - 100.0;
    double maxPrice = midPrice + 100.0;
    
    m_viewState->setViewport(timeStart, timeEnd, minPrice, maxPrice);
    
    sLog_App("🎯 DataProcessor VIEWPORT FROM ORDER BOOK:");
    sLog_App("   Mid Price: $" << midPrice);
    sLog_App("   Price Window: $" << minPrice << " - $" << maxPrice);
    
    emit viewportInitialized();
}

// 🚀 PHASE 3: Handle dense LiveOrderBook updates directly! 
void DataProcessor::onLiveOrderBookUpdated(const QString& productId) {
    if (!m_dataCache || !m_liquidityEngine) {
        sLog_Render("❌ DataProcessor: DataCache or LiquidityEngine not set");
        return;
    }
    
    // Get direct dense LiveOrderBook access (no conversion!)
    const auto& liveBook = m_dataCache->getDirectLiveOrderBook(productId.toStdString());
    
    sLog_Render("🚀 PHASE 3: DataProcessor processing dense LiveOrderBook - bids:" << liveBook.getBidCount() << " asks:" << liveBook.getAskCount());
    
    // Convert dense LiveOrderBook to sparse OrderBook for LTSE processing
    // TODO: Eventually LTSE should accept dense format directly
    OrderBook sparseBook;
    sparseBook.product_id = productId.toStdString();
    sparseBook.timestamp = std::chrono::system_clock::now();
    
    // Convert dense to sparse using a mid-price band (viewport-independent)
    const auto& denseBids = liveBook.getBids();
    const auto& denseAsks = liveBook.getAsks();
    // Mid price
    double bestBid = std::numeric_limits<double>::quiet_NaN();
    double bestAsk = std::numeric_limits<double>::quiet_NaN();
    // Find bests via dense arrays
    for (size_t i = denseBids.size(); i > 0; --i) { if (denseBids[i-1] > 0.0) { bestBid = liveBook.getMinPrice() + (static_cast<double>(i-1) * liveBook.getTickSize()); break; } }
    for (size_t i = 0; i < denseAsks.size(); ++i) { if (denseAsks[i] > 0.0) { bestAsk = liveBook.getMinPrice() + (static_cast<double>(i) * liveBook.getTickSize()); break; } }
    double mid = (!std::isnan(bestBid) && !std::isnan(bestAsk)) ? (bestBid + bestAsk) * 0.5
               : (!std::isnan(bestBid) ? bestBid : (!std::isnan(bestAsk) ? bestAsk : liveBook.getMinPrice() + 0.5 * (denseBids.size() * liveBook.getTickSize())));
    double halfBand = 0.0;
    switch (m_bandMode) {
        case BandMode::FixedDollar: halfBand = std::max(1e-6, m_bandValue); break;
        case BandMode::PercentMid:  halfBand = std::max(1e-6, std::abs(mid) * m_bandValue); break;
        case BandMode::Ticks:       halfBand = std::max(1.0, m_bandValue) * liveBook.getTickSize(); break;
    }
    // Safety caps
    const double maxHalfBand = (denseBids.size() * liveBook.getTickSize()) * 0.5;
    halfBand = std::min(halfBand, maxHalfBand);
    double bandMinPrice = mid - halfBand;
    double bandMaxPrice = mid + halfBand;
    
    // Calculate index bounds for band window
    size_t minIndex = std::max(0.0, (bandMinPrice - liveBook.getMinPrice()) / liveBook.getTickSize());
    size_t maxIndex = std::min(static_cast<double>(std::max(denseBids.size(), denseAsks.size())), 
                              (bandMaxPrice - liveBook.getMinPrice()) / liveBook.getTickSize());
    
    // Convert bids (scan only visible range from highest to lowest price)
    for (size_t i = std::min(maxIndex, denseBids.size()); i > minIndex && i > 0; --i) {
        size_t idx = i - 1;
        if (denseBids[idx] > 0.0) {
            double price = liveBook.getMinPrice() + (static_cast<double>(idx) * liveBook.getTickSize());
            sparseBook.bids.push_back({price, denseBids[idx]});
        }
    }
    
    // Convert asks (scan only visible range from lowest to highest price)
    for (size_t i = minIndex; i < std::min(maxIndex, denseAsks.size()); ++i) {
        if (denseAsks[i] > 0.0) {
            double price = liveBook.getMinPrice() + (static_cast<double>(i) * liveBook.getTickSize());
            sparseBook.asks.push_back({price, denseAsks[i]});
        }
    }
    
    // Prime LTSE immediately and cache for sampler
    m_liquidityEngine->addOrderBookSnapshot(sparseBook);
    {
        std::lock_guard<std::mutex> lock(m_dataMutex);
        m_latestOrderBook = std::make_shared<OrderBook>(sparseBook);
        m_hasValidOrderBook = true;
    }
    
    sLog_Data("🎯 DataProcessor: LiveOrderBook cached + primed snapshot - bids=" << sparseBook.bids.size() << " asks=" << sparseBook.asks.size());
    emit dataUpdated();
}

void DataProcessor::clearData() {
    std::lock_guard<std::mutex> lock(m_dataMutex);
    
    m_latestOrderBook = nullptr;
    m_hasValidOrderBook = false;
    
    if (m_viewState) {
        m_viewState->resetZoom();
    }
    
    sLog_App("🎯 DataProcessor: Data cleared");
    emit dataUpdated();
}

// 🚀 PHASE 3: Business logic methods moved from UGR

void DataProcessor::updateVisibleCells() {
    m_visibleCells.clear();
    
    if (!m_viewState || !m_viewState->isTimeWindowValid()) return;
    
    int64_t activeTimeframe = m_currentTimeframe_ms;
    
    // 🚀 OPTIMIZATION 1: Use manual timeframe if set, otherwise auto-suggest
    if (!m_manualTimeframeSet || 
        (m_manualTimeframeTimer.isValid() && m_manualTimeframeTimer.elapsed() > 10000)) {  // 10 second timeout
        
        // Auto-suggest timeframe when no manual override
        if (m_liquidityEngine) {
            int64_t optimalTimeframe = m_liquidityEngine->suggestTimeframe(
                m_viewState->getVisibleTimeStart(), m_viewState->getVisibleTimeEnd(), 2000);
            
            if (optimalTimeframe != m_currentTimeframe_ms) {
                m_currentTimeframe_ms = optimalTimeframe;
                activeTimeframe = optimalTimeframe;
                sLog_Render("🔄 AUTO-TIMEFRAME UPDATE: " << optimalTimeframe << "ms (viewport-optimized)");
            }
        }
    } else {
        sLog_Render("🎯 MANUAL TIMEFRAME: Using " << m_currentTimeframe_ms << "ms (user-selected)");
    }
    
    // Get liquidity slices for active timeframe within viewport
    if (m_liquidityEngine) {
        qint64 timeStart = m_viewState->getVisibleTimeStart();
        qint64 timeEnd = m_viewState->getVisibleTimeEnd();
        sLog_Render("🔍 LTSE QUERY: timeframe=" << activeTimeframe << "ms, window=[" << timeStart << "-" << timeEnd << "]");
        
        auto visibleSlices = m_liquidityEngine->getVisibleSlices(activeTimeframe, timeStart, timeEnd);
        sLog_Render("🔍 LTSE RESULT: Found " << visibleSlices.size() << " slices for rendering");
        
        // 🔍 DEBUG: Check if slices are being processed
        int processedSlices = 0;
        
        // Auto-fix viewport if no data found but data exists
        if (visibleSlices.empty()) {
            auto allSlices = m_liquidityEngine->getVisibleSlices(activeTimeframe, 0, LLONG_MAX);
            if (!allSlices.empty()) {
                qint64 oldestTime = allSlices.front()->startTime_ms;
                qint64 newestTime = allSlices.back()->endTime_ms;
                qint64 gap = timeStart - newestTime;
                sLog_Render("🔍 LTSE TIME MISMATCH: Have " << allSlices.size() << " slices in range [" << oldestTime << "-" << newestTime << "], but viewport is [" << timeStart << "-" << timeEnd << "]");
                sLog_Render("🔍 TIME GAP: " << gap << "ms between newest data and viewport start");
                
                // AUTO-FIX: Snap viewport to actual data range
                if (gap > 60000) { // If gap > 1 minute, auto-adjust
                    qint64 newStart = newestTime - 30000; // 30s before newest data
                    qint64 newEnd = newestTime + 30000;   // 30s after newest data
                    sLog_Render("🔧 AUTO-ADJUSTING VIEWPORT: [" << newStart << "-" << newEnd << "] to match data");
                    
                    m_viewState->setViewport(newStart, newEnd, m_viewState->getMinPrice(), m_viewState->getMaxPrice());
                    
                    // Retry query with corrected viewport
                    visibleSlices = m_liquidityEngine->getVisibleSlices(activeTimeframe, newStart, newEnd);
                    sLog_Render("🎯 VIEWPORT FIX RESULT: Found " << visibleSlices.size() << " slices after adjustment");
                }
            }
        }
        
        // 🚀 Process all visible slices; viewport + transform handle GPU load
        for (const auto* slice : visibleSlices) {
            ++processedSlices;
            createCellsFromLiquiditySlice(*slice);
        }
        
        sLog_Render("🔍 SLICE PROCESSING: Processed " << processedSlices << "/" << visibleSlices.size() << " slices");
        
        sLog_Render("🎯 DATA PROCESSOR COVERAGE Slices:" << visibleSlices.size()
                 << " TotalCells:" << m_visibleCells.size()
                 << " ActiveTimeframe:" << activeTimeframe << "ms"
                 << " (Manual:" << (m_manualTimeframeSet ? "YES" : "NO") << ")");
    }
    
    emit dataUpdated();
}

void DataProcessor::createCellsFromLiquiditySlice(const LiquidityTimeSlice& slice) {
    if (!m_viewState) return;
    
    double minPrice = m_viewState->getMinPrice();
    double maxPrice = m_viewState->getMaxPrice();
    
    // 🔍 DEBUG: Log slice processing details
    static int sliceCounter = 0;
    if (++sliceCounter % 10 == 0) {
        sLog_Render("🎯 SLICE DEBUG #" << sliceCounter << ": time=" << slice.startTime_ms 
                    << " bids=" << slice.bidMetrics.size() << " asks=" << slice.askMetrics.size() 
                    << " priceRange=$" << minPrice << "-$" << maxPrice);
    }
    
    // Create cells for bid levels (tick-based iteration)
    for (size_t i = 0; i < slice.bidMetrics.size(); ++i) {
        const auto& metrics = slice.bidMetrics[i];
        if (metrics.snapshotCount > 0) {
            double price = slice.minTick * slice.tickSize + (static_cast<double>(i) * slice.tickSize);
            if (price >= minPrice && price <= maxPrice) {
                createLiquidityCell(slice, price, slice.getDisplayValue(price, true, 0), true);
            }
        }
    }
    
    // Create cells for ask levels (tick-based iteration)
    for (size_t i = 0; i < slice.askMetrics.size(); ++i) {
        const auto& metrics = slice.askMetrics[i];
        if (metrics.snapshotCount > 0) {
            double price = slice.minTick * slice.tickSize + (static_cast<double>(i) * slice.tickSize);
            if (price >= minPrice && price <= maxPrice) {
                createLiquidityCell(slice, price, slice.getDisplayValue(price, false, 0), false);
            }
        }
    }
}

void DataProcessor::createLiquidityCell(const LiquidityTimeSlice& slice, double price, double liquidity, bool isBid) {
    if (liquidity <= 0.0 || !m_viewState) return;
    
    CellInstance cell;
    cell.timeSlot = slice.startTime_ms;
    cell.priceLevel = price;
    cell.liquidity = liquidity;
    cell.isBid = isBid;
    cell.intensity = std::min(1.0, liquidity / 1000.0);
    cell.color = isBid ? QColor(0, 255, 0, 128) : QColor(255, 0, 0, 128);
    // Compute screen-space rect from world coordinates
    cell.screenRect = timeSliceToScreenRect(slice, price);

    // Cull degenerate or off-screen rectangles
    const double minPixel = 0.5;   // Avoid zero-area artifacts
    const double maxPixel = 200.0; // Clamp pathological sizes
    if (cell.screenRect.width() < minPixel || cell.screenRect.height() < minPixel) {
        return;
    }
    if (cell.screenRect.width() > maxPixel) {
        // Clamp width to prevent giant blocks due to bad transforms
        cell.screenRect.setWidth(maxPixel);
    }
    if (cell.screenRect.height() > maxPixel) {
        cell.screenRect.setHeight(maxPixel);
    }

    // Basic viewport culling using current item size from view state
    const double viewportW = m_viewState ? m_viewState->getViewportWidth() : 0.0;
    const double viewportH = m_viewState ? m_viewState->getViewportHeight() : 0.0;
    if (viewportW > 0.0 && viewportH > 0.0) {
        const QRectF viewportRect(0.0, 0.0, viewportW, viewportH);
        if (!viewportRect.intersects(cell.screenRect)) {
            return;
        }
    }

    m_visibleCells.push_back(cell);
    
    // 🔍 DEBUG: Log cell creation occasionally
    static int cellCounter = 0;
    if (++cellCounter % 50 == 0) {
        sLog_Render("🎯 CELL DEBUG: Created cell #" << cellCounter << " at screenRect(" 
                    << cell.screenRect.x() << "," << cell.screenRect.y() << "," 
                    << cell.screenRect.width() << "x" << cell.screenRect.height() 
                    << ") liquidity=" << cell.liquidity << " isBid=" << cell.isBid);
    }
}

QRectF DataProcessor::timeSliceToScreenRect(const LiquidityTimeSlice& slice, double price) const {
    if (!m_viewState) return QRectF();
    
    // 🎯 FIX: Use CoordinateSystem for proper world-to-screen transformation
    Viewport viewport{
        m_viewState->getVisibleTimeStart(), m_viewState->getVisibleTimeEnd(),
        m_viewState->getMinPrice(), m_viewState->getMaxPrice(),
        m_viewState->getViewportWidth(), m_viewState->getViewportHeight()
    };

    // Use slice's tick size for vertical sizing to match aggregation
    const double halfTick = slice.tickSize * 0.5;

    // Ensure a non-zero time extent
    int64_t timeStart = slice.startTime_ms;
    int64_t timeEnd = slice.endTime_ms;
    if (timeEnd <= timeStart) {
        int64_t span = slice.duration_ms > 0 ? slice.duration_ms : std::max<int64_t>(m_currentTimeframe_ms, 1);
        timeEnd = timeStart + span;
    }

    // Use CoordinateSystem for proper transformation like old renderer
    QPointF topLeft = CoordinateSystem::worldToScreen(timeStart, price + halfTick, viewport);
    QPointF bottomRight = CoordinateSystem::worldToScreen(timeEnd, price - halfTick, viewport);
    
    QRectF result(topLeft, bottomRight);
    
    // 🔍 DEBUG: Log coordinate transformation once per batch
    static int debugCounter = 0;
    if (++debugCounter % 100 == 0) {
        sLog_Render("🎯 COORD DEBUG: World(" << timeStart << "," << price << ") -> Screen(" 
                    << topLeft.x() << "," << topLeft.y() << ") Viewport[" 
                    << viewport.timeStart_ms << "-" << viewport.timeEnd_ms << ", $" 
                    << viewport.priceMin << "-$" << viewport.priceMax << "] Size[" 
                    << viewport.width << "x" << viewport.height << "]");
    }
    
    return result;
}

// 🚀 PHASE 3: LTSE delegation methods (moved from UGR)

void DataProcessor::setPriceResolution(double resolution) {
    if (m_liquidityEngine && resolution > 0) {
        m_liquidityEngine->setPriceResolution(resolution);
        emit dataUpdated();
    }
}

double DataProcessor::getPriceResolution() const {
    return m_liquidityEngine ? m_liquidityEngine->getPriceResolution() : 1.0;
}

void DataProcessor::addTimeframe(int timeframe_ms) {
    if (m_liquidityEngine) {
        m_liquidityEngine->addTimeframe(timeframe_ms);
    }
}

int64_t DataProcessor::suggestTimeframe(qint64 timeStart, qint64 timeEnd, int maxCells) const {
    return m_liquidityEngine ? m_liquidityEngine->suggestTimeframe(timeStart, timeEnd, maxCells) : 100;
}

std::vector<LiquidityTimeSlice> DataProcessor::getVisibleSlices(qint64 timeStart, qint64 timeEnd, double minPrice, double maxPrice) const {
    if (m_liquidityEngine) {
        auto slicePtrs = m_liquidityEngine->getVisibleSlices(m_currentTimeframe_ms, timeStart, timeEnd);
        std::vector<LiquidityTimeSlice> slices;
        slices.reserve(slicePtrs.size());
        for (const auto* ptr : slicePtrs) {
            if (ptr) slices.push_back(*ptr);
        }
        return slices;
    }
    return {};
}

int DataProcessor::getDisplayMode() const {
    return m_liquidityEngine ? static_cast<int>(m_liquidityEngine->getDisplayMode()) : 0;
}

// 🚀 PHASE 3: Manual timeframe management (preserve existing logic)

void DataProcessor::setTimeframe(int timeframe_ms) {
    if (timeframe_ms > 0) {
        m_currentTimeframe_ms = timeframe_ms;
        m_manualTimeframeSet = true;
        m_manualTimeframeTimer.restart();
        
        if (m_liquidityEngine) {
            m_liquidityEngine->addTimeframe(timeframe_ms);
        }
        
        sLog_Render("🎯 MANUAL TIMEFRAME SET: " << timeframe_ms << "ms");
        emit dataUpdated();
    }
}

bool DataProcessor::isManualTimeframeSet() const {
    return m_manualTimeframeSet;
}