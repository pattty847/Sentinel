#include "gpudataadapter.h"
#include "Log.hpp"
#include <QElapsedTimer>
#include <algorithm>
#include <chrono>
#include <QDateTime>

static constexpr auto CAT = "GPUAdapter";

GPUDataAdapter::GPUDataAdapter(QObject* parent)
    : QObject(parent)
    , m_processTimer(new QTimer(this))
    , m_candle100msTimer(new QTimer(this))
    , m_candle500msTimer(new QTimer(this))
    , m_candle1sTimer(new QTimer(this))
{
    LOG_I(CAT, "🚀 GPUDataAdapter: Initializing lock-free data pipeline...");
    
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
    
    LOG_I(CAT,
          "✅ GPUDataAdapter: Lock-free pipeline initialized - Reserve size:{} - Trade queue capacity: 65536 - OrderBook queue capacity: 16384",
          m_reserveSize);
}

void GPUDataAdapter::initializeBuffers() {
    // Runtime-configurable buffer size (handle Intel UHD VRAM limits)
    QSettings settings;
    m_reserveSize = settings.value("chart/reserveSize", 2'000'000).toULongLong();
    
    // Pre-allocate once: std::max(expected, userPref)
    size_t actualSize = (m_reserveSize > 100000) ? m_reserveSize : 100000; // Minimum 100k
    
    LOG_I(CAT, "🔧 GPUDataAdapter: Pre-allocating buffers - Trade buffer:{} Heatmap buffer:{}", actualSize, actualSize);

    m_tradeBuffer.reserve(actualSize);
    m_heatmapBuffer.reserve(actualSize);
    m_candleBuffer.reserve(actualSize);

    // Initialize with empty elements to avoid reallocations
    m_tradeBuffer.resize(actualSize);
    m_heatmapBuffer.resize(actualSize);
    m_candleBuffer.resize(actualSize);
    
    LOG_I(CAT, "💾 GPUDataAdapter: Buffer allocation complete");
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
        tradesProcessed++;
        
        // Rate limiting for stress testing
        if (tradesProcessed >= static_cast<size_t>(m_firehoseRate / 60)) {
            break; // Limit to firehose_rate / 60 per frame (60 FPS assumption)
        }
    }

    // Candle processing moved to separate time-based timers
    // (100ms, 500ms, 1s timers handle their respective candle updates)
    
    // Process order books for heatmap
    OrderBook orderBook;
    m_heatmapWriteCursor = 0;
    
    size_t orderBooksProcessed = 0;
    while (m_orderBookQueue.pop(orderBook) && orderBooksProcessed < 10) { // Limit order book processing
        // Convert order book to heatmap quads
        for (const auto& bid : orderBook.bids) {
            if (m_heatmapWriteCursor >= m_reserveSize) break;
            
            GPUTypes::QuadInstance quad;
            quad.x = 0.0f; // Will be calculated in GPU widget
            quad.y = static_cast<float>(bid.price);
            quad.width = static_cast<float>(bid.size * 100.0); // Scale for visibility
            quad.height = 2.0f; // Fixed height
            quad.r = 0.0f; quad.g = 1.0f; quad.b = 0.0f; quad.a = 0.8f; // Green for bids
            
            m_heatmapBuffer[m_heatmapWriteCursor++] = quad;
        }
        
        for (const auto& ask : orderBook.asks) {
            if (m_heatmapWriteCursor >= m_reserveSize) break;
            
            GPUTypes::QuadInstance quad;
            quad.x = 0.0f; // Will be calculated in GPU widget
            quad.y = static_cast<float>(ask.price);
            quad.width = static_cast<float>(ask.size * 100.0); // Scale for visibility
            quad.height = 2.0f; // Fixed height
            quad.r = 1.0f; quad.g = 0.0f; quad.b = 0.0f; quad.a = 0.8f; // Red for asks
            
            m_heatmapBuffer[m_heatmapWriteCursor++] = quad;
        }
        
        orderBooksProcessed++;
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
            
            LOG_D(CAT,
                  "📊 GPU ADAPTER DISTRIBUTION: Total:{} Buys:{}({}%) Sells:{}({}%) Unknown:{}",
                  gpuTotalTrades,
                  gpuTotalBuys, QString::number(gpuBuyPercent, 'f', 1).toStdString(),
                  gpuTotalSells, QString::number(gpuSellPercent, 'f', 1).toStdString(),
                  gpuTotalUnknown);
        }
        
        emit tradesReady(m_tradeBuffer.data(), m_tradeWriteCursor);
        m_processedTrades.fetch_add(tradesProcessed, std::memory_order_relaxed);
    }
    
    if (Q_LIKELY(m_heatmapWriteCursor > 0)) {
        emit heatmapReady(m_heatmapBuffer.data(), m_heatmapWriteCursor);
    }

    // Candle updates now emitted by separate time-based timers
    
    // Performance monitoring
    qint64 frameTime = frameTimer.elapsed();
    if (frameTime > 16) { // >60 FPS threshold
        m_frameDrops.fetch_add(1, std::memory_order_relaxed);
        LOG_W(CAT, "⚠️ GPUDataAdapter: Frame time exceeded: {} ms", frameTime);
    }
    
    // Debug output every 1000 frames (~16.7 seconds at 60 FPS)
    static int frameCount = 0;
    if (++frameCount % 1000 == 0) {
        LOG_I(CAT,
              "📊 GPUDataAdapter Stats: Trades processed:{} Points pushed:{} Frame drops:{} Trade queue size:{} OrderBook queue size:{}",
              m_processedTrades.load(), m_pointsPushed.load(), m_frameDrops.load(),
              m_tradeQueue.size(), m_orderBookQueue.size());
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
        LOG_D(CAT, "🎨 GPU ADAPTER COLOR #{} : Side:{} Price:{} Color RGBA:({},{},{},{})",
              gpuConversionCount, sideStr, trade.price, point.r, point.g, point.b, point.a);
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
    LOG_I(CAT, "🔧 GPUDataAdapter: Reserve size set to {}", size);
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
            qDebug() << "🕯️ TIME-BASED CANDLE EMIT #" << candleCount[tfIndex]
                     << "TimeFrame:" << CandleUtils::timeFrameName(timeframe)
                     << "Timestamp:" << update.timestamp_ms
                     << "OHLC:" << update.candle.open << update.candle.high 
                     << update.candle.low << update.candle.close;
        }
    }
} 