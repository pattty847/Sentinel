#include "gpudataadapter.h"
#include "SentinelLogging.hpp"
#include <QDebug>
#include <QElapsedTimer>
#include <algorithm>
#include <chrono>
#include <QDateTime>
#include <cstdint>

GPUDataAdapter::GPUDataAdapter(QObject* parent)
    : QObject(parent)
    , m_processTimer(new QTimer(this))
    , m_candle100msTimer(new QTimer(this))
    , m_candle500msTimer(new QTimer(this))
    , m_candle1sTimer(new QTimer(this))
    , m_liveOrderBook("BTC-USD") // Initialize stateful order book
{
    sLog_Init("🚀 GPUDataAdapter: Initializing lock-free data pipeline...");
    
    initializeBuffers();
    
    // Connect processing timers
    connect(m_processTimer, &QTimer::timeout, this, &GPUDataAdapter::processIncomingData);
    connect(m_candle100msTimer, &QTimer::timeout, this, &GPUDataAdapter::processHighFrequencyCandles);
    connect(m_candle500msTimer, &QTimer::timeout, this, &GPUDataAdapter::processMediumFrequencyCandles);
    connect(m_candle1sTimer, &QTimer::timeout, this, &GPUDataAdapter::processSecondCandles);
    
    // Start timers at their proper intervals
    m_processTimer->start(16);     // 16ms - Trade scatter + Order book heatmap
    m_candle100msTimer->start(100); // 100ms - High-frequency candles
    m_candle500msTimer->start(500); // 500ms - Medium-frequency candles  
    m_candle1sTimer->start(1000);   // 1000ms - Second candles
    
    sLog_Init("✅ GPUDataAdapter: Lock-free pipeline initialized"
             << "- Reserve size:" << m_reserveSize 
             << "- Trade queue capacity: 65536"
             << "- OrderBook queue capacity: 16384"
             << "- Candle timers: 100ms, 500ms, 1s");
}

void GPUDataAdapter::initializeBuffers() {
    // Runtime-configurable buffer size (handle Intel UHD VRAM limits)
    QSettings settings;
    m_reserveSize = settings.value("chart/reserveSize", 2'000'000).toULongLong();
    
    // Pre-allocate once: std::max(expected, userPref)
    size_t actualSize = (m_reserveSize > 100000) ? m_reserveSize : 100000; // Minimum 100k
    
    sLog_Init("🔧 GPUDataAdapter: Pre-allocating buffers - Trade buffer:" << actualSize
             << "Heatmap buffer:" << actualSize);

    m_tradeBuffer.reserve(actualSize);
    m_heatmapBuffer.reserve(actualSize);
    m_candleBuffer.reserve(actualSize);

    // Initialize with empty elements to avoid reallocations
    m_tradeBuffer.resize(actualSize);
    m_heatmapBuffer.resize(actualSize);
    m_candleBuffer.resize(actualSize);
    
    sLog_Init("💾 GPUDataAdapter: Buffer allocation complete");
}

bool GPUDataAdapter::pushTrade(const Trade& trade) {
    if (m_tradeQueue.push(trade)) {
        m_pointsPushed.fetch_add(1, std::memory_order_relaxed);
        return true;
    } else {
        // Queue full - performance alert
        m_frameDrops.fetch_add(1, std::memory_order_relaxed);
        emit performanceAlert("Trade queue full - dropping data!");
        return false;
    }
}

bool GPUDataAdapter::pushOrderBook(const OrderBook& orderBook) {
    if (m_orderBookQueue.push(orderBook)) {
        return true;
    } else {
        // Queue full - performance alert
        m_frameDrops.fetch_add(1, std::memory_order_relaxed);
        emit performanceAlert("OrderBook queue full - dropping data!");
        return false;
    }
}

void GPUDataAdapter::processIncomingData() {
    static QElapsedTimer frameTimer;
    frameTimer.start();
    
    // ZERO MALLOC ZONE - rolling write cursor
    Trade trade;
    m_tradeWriteCursor = 0; // Reset cursor, DON'T clear()
    m_candleWriteCursor = 0;

    size_t tradesProcessed = 0;
    
    // Process trades with rate limiting based on firehose setting
    while (m_tradeQueue.pop(trade) && m_tradeWriteCursor < m_reserveSize) {
        if (Q_UNLIKELY(trade.trade_id.empty())) continue; // Guard clause

        // Convert to GPU point with coordinate mapping
        m_tradeBuffer[m_tradeWriteCursor++] = convertTradeToGPUPoint(trade);
        m_candleLOD.addTrade(trade);
        m_currentSymbol = trade.product_id;
        
        // 🚀 PHASE 2: Add trade to time-windowed history buffer
        {
            std::lock_guard<std::mutex> lock(m_tradeHistoryMutex);
            m_tradeHistory.push_back(trade);
        }
        
        tradesProcessed++;
        
        // Rate limiting for stress testing
        if (tradesProcessed >= static_cast<size_t>(m_firehoseRate / 60)) {
            break; // Limit to firehose_rate / 60 per frame (60 FPS assumption)
        }
    }

    // Candle processing moved to separate time-based timers
    // (100ms, 500ms, 1s timers handle their respective candle updates)
    
    // 🚀 STATEFUL: Process order books with LiveOrderBook
    OrderBook orderBook;
    m_heatmapWriteCursor = 0;
    
    size_t orderBooksProcessed = 0;
    while (m_orderBookQueue.pop(orderBook) && orderBooksProcessed < 10) { 
        // Initialize from snapshot or apply updates to LiveOrderBook
        if (orderBooksProcessed == 0) {
            // First order book is treated as a snapshot
            m_liveOrderBook.initializeFromSnapshot(orderBook);
        } else {
            // Subsequent order books are treated as updates
            for (const auto& bid : orderBook.bids) {
                m_liveOrderBook.applyUpdate("bid", bid.price, bid.size);
            }
            
            for (const auto& ask : orderBook.asks) {
                m_liveOrderBook.applyUpdate("ask", ask.price, ask.size);
            }
        }
        
        orderBooksProcessed++;
    }
    
    // Convert LiveOrderBook to GPU quads (only if data was updated)
    if (orderBooksProcessed > 0) {
        convertLiveOrderBookToQuads();
    }
    
    // 📊 TRADE DISTRIBUTION TRACKING in lock-free pipeline
    static int gpuTotalBuys = 0, gpuTotalSells = 0, gpuTotalUnknown = 0;
    if (Q_LIKELY(m_tradeWriteCursor > 0)) {
        // Count colors in current buffer
        for (size_t i = 0; i < m_tradeWriteCursor; ++i) {
            const auto& point = m_tradeBuffer[i];
            if (point.g > 0.8f && point.r < 0.2f) gpuTotalBuys++;        // Green = Buy
            else if (point.r > 0.8f && point.g < 0.2f) gpuTotalSells++; // Red = Sell
            else gpuTotalUnknown++;                                      // Yellow/Other
        }
        
        // Log distribution every 50 trades
        static int gpuDistributionCounter = 0;
        if (++gpuDistributionCounter % 50 == 0) {
            int gpuTotalTrades = gpuTotalBuys + gpuTotalSells + gpuTotalUnknown;
            double gpuBuyPercent = gpuTotalTrades > 0 ? (gpuTotalBuys * 100.0 / gpuTotalTrades) : 0.0;
            double gpuSellPercent = gpuTotalTrades > 0 ? (gpuTotalSells * 100.0 / gpuTotalTrades) : 0.0;
            
            sLog_GPU("📊 GPU ADAPTER DISTRIBUTION: Total:" << gpuTotalTrades
                     << " Buys:" << gpuTotalBuys << "(" << QString::number(gpuBuyPercent, 'f', 1) << "%)"
                     << " Sells:" << gpuTotalSells << "(" << QString::number(gpuSellPercent, 'f', 1) << "%)"
                     << " Unknown:" << gpuTotalUnknown);
        }
        
        emit tradesReady(m_tradeBuffer.data(), m_tradeWriteCursor);
        m_processedTrades.fetch_add(tradesProcessed, std::memory_order_relaxed);
    }
    
    if (Q_LIKELY(m_heatmapWriteCursor > 0)) {
        emit heatmapReady(m_heatmapBuffer.data(), m_heatmapWriteCursor);
    }

    // Candle updates now emitted by separate time-based timers
    
    // 🚀 PHASE 2: Periodic cleanup of trade history buffer (every 5 seconds)
    int64_t currentTime_ms = QDateTime::currentMSecsSinceEpoch();
    if (currentTime_ms - m_lastHistoryCleanup_ms > CLEANUP_INTERVAL_MS) {
        cleanupOldTradeHistory();
        m_lastHistoryCleanup_ms = currentTime_ms;
    }
    
    // Performance monitoring
    qint64 frameTime = frameTimer.elapsed();
    if (frameTime > 16) { // >60 FPS threshold
        m_frameDrops.fetch_add(1, std::memory_order_relaxed);
        sLog_Performance("⚠️ GPUDataAdapter: Frame time exceeded:" << frameTime << "ms");
    }
    
    // Debug output every 1000 frames (~16.7 seconds at 60 FPS)
    static int frameCount = 0;
    if (++frameCount % 1000 == 0) {
        sLog_Performance("📊 GPUDataAdapter Stats:"
                 << "Trades processed:" << m_processedTrades.load()
                 << "Points pushed:" << m_pointsPushed.load()
                 << "Frame drops:" << m_frameDrops.load()
                 << "Trade queue size:" << m_tradeQueue.size()
                 << "OrderBook queue size:" << m_orderBookQueue.size());
    }
}

GPUTypes::Point GPUDataAdapter::convertTradeToGPUPoint(const Trade& trade) {
    // Update coordinate cache if needed
    updateCoordinateCache(trade.price);
    
    GPUTypes::Point point;
    
    // Time mapping (artificial spacing for now - will be natural in Phase 4)
    static double artificialTimeOffset = 0.0;
    artificialTimeOffset += 500.0; // 500ms spacing
    
    // For now, use simple time-based X coordinate
    double normalizedTime = artificialTimeOffset / m_coordCache.timeSpanMs;
    point.x = static_cast<float>(1.0 - fmod(normalizedTime, 1.0)); // Wrap around
    
    // Price mapping to Y coordinate
    if (m_coordCache.initialized) {
        double normalizedPrice = (trade.price - m_coordCache.minPrice) / 
                                (m_coordCache.maxPrice - m_coordCache.minPrice);
        normalizedPrice = std::max(0.05, std::min(0.95, normalizedPrice)); // Clamp
        point.y = static_cast<float>(1.0 - normalizedPrice); // Invert Y (top = high price)
    } else {
        point.y = 0.5f; // Center if no price range established
    }
    
    // 🎨 Color based on trade side
    const char* sideStr = "UNKNOWN";
    if (trade.side == AggressorSide::Buy) {
        point.r = 0.0f; point.g = 1.0f; point.b = 0.0f; point.a = 0.8f; // Green
        sideStr = "BUY";
    } else if (trade.side == AggressorSide::Sell) {
        point.r = 1.0f; point.g = 0.0f; point.b = 0.0f; point.a = 0.8f; // Red
        sideStr = "SELL";
    } else {
        // Unknown side - yellow for debugging
        point.r = 1.0f; point.g = 1.0f; point.b = 0.0f; point.a = 0.8f; // Yellow
        sideStr = "UNKNOWN";
    }
    
    // 🔍 DEBUG: Log first 10 GPU conversions in lock-free pipeline
    static int gpuConversionCount = 0;
    if (++gpuConversionCount <= 10) {
        sLog_GPU("🎨 GPU ADAPTER COLOR #" << gpuConversionCount << ": Side:" << sideStr
                 << " Price:" << trade.price
                 << " Color RGBA:(" << point.r << "," << point.g << "," << point.b << "," << point.a << ")");
    }
    
    return point;
}

void GPUDataAdapter::updateCoordinateCache(double price) {
    if (!m_coordCache.initialized) {
        m_coordCache.minPrice = price * 0.98; // 2% buffer below
        m_coordCache.maxPrice = price * 1.02; // 2% buffer above
        m_coordCache.initialized = true;
    } else {
        // Auto-expand range if needed
        if (price < m_coordCache.minPrice) {
            m_coordCache.minPrice = price * 0.98;
        } else if (price > m_coordCache.maxPrice) {
            m_coordCache.maxPrice = price * 1.02;
        }
    }
}

void GPUDataAdapter::setReserveSize(size_t size) {
    m_reserveSize = size;
    // Note: Would need to reinitialize buffers in production
    sLog_GPU("🔧 GPUDataAdapter: Reserve size set to" << size);
}

void GPUDataAdapter::resetWriteCursors() {
    m_tradeWriteCursor = 0;
    m_heatmapWriteCursor = 0;
}

// 🕯️ TIME-BASED CANDLE PROCESSING: Emit candle updates at proper intervals

void GPUDataAdapter::processHighFrequencyCandles() {
    // Process 100ms candles only
    processCandleTimeFrame(CandleLOD::TF_100ms);
}

void GPUDataAdapter::processMediumFrequencyCandles() {
    // Process 500ms candles only
    processCandleTimeFrame(CandleLOD::TF_500ms);
}

void GPUDataAdapter::processSecondCandles() {
    // Process 1s candles only
    processCandleTimeFrame(CandleLOD::TF_1sec);
}

void GPUDataAdapter::processCandleTimeFrame(CandleLOD::TimeFrame timeframe) {
    const auto& vec = m_candleLOD.getCandlesForTimeFrame(timeframe);
    if (vec.empty()) return;

    size_t tfIndex = static_cast<size_t>(timeframe);
    const OHLC& latestCandle = vec.back();
    
    // Only emit if candle has changed (time-based boundary crossed)
    if (latestCandle.timestamp_ms != m_lastEmittedCandleTime[tfIndex]) {
        CandleUpdate update;
        update.symbol = m_currentSymbol;
        update.timestamp_ms = latestCandle.timestamp_ms;
        update.timeframe = timeframe;
        update.candle = latestCandle;
        update.isNewCandle = true; // Always true for time-based updates
        
        m_lastEmittedCandleTime[tfIndex] = update.timestamp_ms;
        
        // Emit immediately for time-based candles
        std::vector<CandleUpdate> ready = {update};
        emit candlesReady(ready);
        
        // Debug logging for first few candles of each timeframe
        static std::array<int, CandleLOD::NUM_TIMEFRAMES> candleCount{};
        if (++candleCount[tfIndex] <= 5) {
            sLog_Candles("🕯️ TIME-BASED CANDLE EMIT #" << candleCount[tfIndex]
                     << "TimeFrame:" << CandleUtils::timeFrameName(timeframe)
                     << "Timestamp:" << update.timestamp_ms
                     << "OHLC:" << update.candle.open << update.candle.high 
                     << update.candle.low << update.candle.close);
        }
    }
}

// 🚀 PHASE 2: TIME-WINDOWED TRADE HISTORY CLEANUP
void GPUDataAdapter::cleanupOldTradeHistory() {
    std::lock_guard<std::mutex> lock(m_tradeHistoryMutex);
    
    if (m_tradeHistory.empty()) return;
    
    // Calculate cutoff time (10 minutes ago)
    auto now = std::chrono::system_clock::now();
    auto cutoffTime = now - HISTORY_WINDOW_SPAN;
    int64_t cutoff_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        cutoffTime.time_since_epoch()).count();
    
    // Remove trades older than cutoff
    size_t initialSize = m_tradeHistory.size();
    
    while (!m_tradeHistory.empty()) {
        // Convert trade timestamp to milliseconds for comparison
        int64_t trade_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            m_tradeHistory.front().timestamp.time_since_epoch()).count();
        
        if (trade_ms < cutoff_ms) {
            m_tradeHistory.pop_front();
        } else {
            break; // Remaining trades are newer
        }
    }
    
    size_t removedCount = initialSize - m_tradeHistory.size();
    
    // Log cleanup activity (throttled)
    static int cleanupCount = 0;
    if (++cleanupCount <= 10 || removedCount > 0) {
        sLog_GPU("🧹 PHASE 2 TRADE HISTORY CLEANUP #" << cleanupCount
                 << " Removed:" << removedCount << " old trades"
                 << " Current size:" << m_tradeHistory.size()
                 << " Window:" << HISTORY_WINDOW_SPAN.count() << " seconds");
    }
}

// 🚀 STATEFUL: Convert LiveOrderBook to GPU heatmap quads
void GPUDataAdapter::convertLiveOrderBookToQuads() {
    m_heatmapWriteCursor = 0;
    
    // Get bid levels from stateful order book
    auto bids = m_liveOrderBook.getAllBids();
    for (const auto& bid : bids) {
        if (m_heatmapWriteCursor >= m_reserveSize) break;
        
        GPUTypes::QuadInstance quad;
        quad.x = 0.0f; // Will be calculated in GPU widget
        quad.y = static_cast<float>(bid.price);
        quad.width = static_cast<float>(bid.size * 100.0); // Scale for visibility
        quad.height = 2.0f; // Fixed height
        quad.r = 0.0f; quad.g = 1.0f; quad.b = 0.0f; quad.a = 0.8f; // Green for bids
        
        m_heatmapBuffer[m_heatmapWriteCursor++] = quad;
    }
    
    // Get ask levels from stateful order book
    auto asks = m_liveOrderBook.getAllAsks();
    for (const auto& ask : asks) {
        if (m_heatmapWriteCursor >= m_reserveSize) break;
        
        GPUTypes::QuadInstance quad;
        quad.x = 0.0f; // Will be calculated in GPU widget
        quad.y = static_cast<float>(ask.price);
        quad.width = static_cast<float>(ask.size * 100.0); // Scale for visibility
        quad.height = 2.0f; // Fixed height
        quad.r = 1.0f; quad.g = 0.0f; quad.b = 0.0f; quad.a = 0.8f; // Red for asks
        
        m_heatmapBuffer[m_heatmapWriteCursor++] = quad;
    }
    
    // Log LiveOrderBook performance metrics
    static int heatmapUpdateCount = 0;
    if (++heatmapUpdateCount % 100 == 0) {
        sLog_GPU("🚀 LIVE ORDER BOOK STATS #" << heatmapUpdateCount
                 << " Bid count:" << m_liveOrderBook.getBidCount()
                 << " Ask count:" << m_liveOrderBook.getAskCount()
                 << " Best bid:" << getBestBid()
                 << " Best ask:" << getBestAsk()
                 << " Spread:" << getSpread()
                 << " Heatmap quads:" << m_heatmapWriteCursor);
    }
}