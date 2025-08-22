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
#include <QColor>
#include <chrono>
#include <limits>
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
    // 🚀 PHASE 3: No more timer-based polling! Real-time signal-driven processing
    sLog_App("🚀 DataProcessor: Ready for real-time signal-driven processing (no timer needed!)");
    
    // Keep timer code for backward compatibility but don't start it
    // if (m_snapshotTimer && !m_snapshotTimer->isActive()) {
    //     m_snapshotTimer->start(100);
    //     sLog_App("🚀 DataProcessor: Started processing with 100ms snapshots");
    // }
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
    if (!m_hasValidOrderBook || !m_liquidityEngine) return;
    
    std::shared_ptr<const OrderBook> book_copy;
    {
        std::lock_guard<std::mutex> lock(m_dataMutex);
        book_copy = m_latestOrderBook;
    }

    if(book_copy) {
        m_liquidityEngine->addOrderBookSnapshot(*book_copy);
    }
    
    emit dataUpdated();
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
    
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    
    qint64 timeStart = now - 30000;
    qint64 timeEnd = now + 30000;
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
    
    // Convert dense to sparse (temporary until LTSE accepts dense format)
    const auto& denseBids = liveBook.getBids();
    const auto& denseAsks = liveBook.getAsks();
    
    // Convert bids (scan from highest to lowest price)
    for (size_t i = denseBids.size(); i-- > 0; ) {
        if (denseBids[i] > 0.0) {
            double price = liveBook.getMinPrice() + (static_cast<double>(i) * liveBook.getTickSize());
            sparseBook.bids.push_back({price, denseBids[i]});
        }
    }
    
    // Convert asks (scan from lowest to highest price)
    for (size_t i = 0; i < denseAsks.size(); ++i) {
        if (denseAsks[i] > 0.0) {
            double price = liveBook.getMinPrice() + (static_cast<double>(i) * liveBook.getTickSize());
            sparseBook.asks.push_back({price, denseAsks[i]});
        }
    }
    
    // Process through LTSE
    m_liquidityEngine->addOrderBookSnapshot(sparseBook);
    
    sLog_Data("🎯 DataProcessor: LiveOrderBook processed - " << sparseBook.bids.size() << " bids, " << sparseBook.asks.size() << " asks");
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
        auto visibleSlices = m_liquidityEngine->getVisibleSlices(
            activeTimeframe, m_viewState->getVisibleTimeStart(), m_viewState->getVisibleTimeEnd());
        
        // 🚀 PERFORMANCE FIX: Process slices normally, but implement smart culling when needed
        size_t processedSlices = 0;
        for (const auto* slice : visibleSlices) {
            createCellsFromLiquiditySlice(*slice);
            processedSlices++;
            
            // 🚀 PERFORMANCE FIX: Hard cap cell count with smart slice management
            if (m_visibleCells.size() > 8000) {
                size_t remainingSlices = visibleSlices.size() - processedSlices;
                sLog_Render("⚠️ CELL LIMIT HIT: Truncating at " << m_visibleCells.size() 
                           << " cells (" << remainingSlices << " remaining slices skipped) to maintain FPS");
                break;  // Keep the data we've processed so far
            }
        }
        
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
    cell.timeSlot = slice.startTime_ms;  // Use correct field names
    cell.priceLevel = price;
    cell.liquidity = liquidity;
    cell.isBid = isBid;
    cell.intensity = std::min(1.0, liquidity / 1000.0);  // Normalize intensity
    cell.color = isBid ? QColor(0, 255, 0, 128) : QColor(255, 0, 0, 128);  // Default colors
    
    m_visibleCells.push_back(cell);
}

QRectF DataProcessor::timeSliceToScreenRect(const LiquidityTimeSlice& slice, double price) const {
    if (!m_viewState) return QRectF();
    
    // Use current ViewState viewport for coordinate conversion
    double timeStart = static_cast<double>(slice.startTime_ms);
    double timeEnd = static_cast<double>(slice.endTime_ms);
    double priceResolution = slice.tickSize;  // Use slice's tick size
    
    QPointF topLeft(timeStart, price + priceResolution/2);
    QPointF bottomRight(timeEnd, price - priceResolution/2);
    
    return QRectF(topLeft, bottomRight);
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