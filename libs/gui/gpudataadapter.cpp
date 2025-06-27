#include "gpudataadapter.h"
#include <QDebug>
#include <QElapsedTimer>
#include <algorithm>
#include <chrono>

GPUDataAdapter::GPUDataAdapter(QObject* parent)
    : QObject(parent)
    , m_processTimer(new QTimer(this))
{
    qDebug() << "🚀 GPUDataAdapter: Initializing lock-free data pipeline...";
    
    initializeBuffers();
    
    // Connect processing timer
    connect(m_processTimer, &QTimer::timeout, this, &GPUDataAdapter::processIncomingData);
    
    // Start with 16ms (60 FPS) processing interval
    m_processTimer->start(16);
    
    qDebug() << "✅ GPUDataAdapter: Lock-free pipeline initialized"
             << "- Reserve size:" << m_reserveSize 
             << "- Trade queue capacity: 65536"
             << "- OrderBook queue capacity: 16384";
}

void GPUDataAdapter::initializeBuffers() {
    // Runtime-configurable buffer size (handle Intel UHD VRAM limits)
    QSettings settings;
    m_reserveSize = settings.value("chart/reserveSize", 2'000'000).toULongLong();
    
    // Pre-allocate once: std::max(expected, userPref)
    size_t actualSize = (m_reserveSize > 100000) ? m_reserveSize : 100000; // Minimum 100k
    
    qDebug() << "🔧 GPUDataAdapter: Pre-allocating buffers - Trade buffer:" << actualSize 
             << "Heatmap buffer:" << actualSize;
    
    m_tradeBuffer.reserve(actualSize);
    m_heatmapBuffer.reserve(actualSize);
    
    // Initialize with empty elements to avoid reallocations
    m_tradeBuffer.resize(actualSize);
    m_heatmapBuffer.resize(actualSize);
    
    qDebug() << "💾 GPUDataAdapter: Buffer allocation complete";
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
    
    size_t tradesProcessed = 0;
    
    // Process trades with rate limiting based on firehose setting
    while (m_tradeQueue.pop(trade) && m_tradeWriteCursor < m_reserveSize) {
        if (Q_UNLIKELY(trade.trade_id.empty())) continue; // Guard clause
        
        // Convert to GPU point with coordinate mapping
        m_tradeBuffer[m_tradeWriteCursor++] = convertTradeToGPUPoint(trade);
        tradesProcessed++;
        
        // Rate limiting for stress testing
        if (tradesProcessed >= static_cast<size_t>(m_firehoseRate / 60)) {
            break; // Limit to firehose_rate / 60 per frame (60 FPS assumption)
        }
    }
    
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
    
    // Emit data if we have any
    if (Q_LIKELY(m_tradeWriteCursor > 0)) {
        emit tradesReady(m_tradeBuffer.data(), m_tradeWriteCursor);
        m_processedTrades.fetch_add(tradesProcessed, std::memory_order_relaxed);
    }
    
    if (Q_LIKELY(m_heatmapWriteCursor > 0)) {
        emit heatmapReady(m_heatmapBuffer.data(), m_heatmapWriteCursor);
    }
    
    // Performance monitoring
    qint64 frameTime = frameTimer.elapsed();
    if (frameTime > 16) { // >60 FPS threshold
        m_frameDrops.fetch_add(1, std::memory_order_relaxed);
        qWarning() << "⚠️ GPUDataAdapter: Frame time exceeded:" << frameTime << "ms";
    }
    
    // Debug output every 1000 frames (~16.7 seconds at 60 FPS)
    static int frameCount = 0;
    if (++frameCount % 1000 == 0) {
        qDebug() << "📊 GPUDataAdapter Stats:"
                 << "Trades processed:" << m_processedTrades.load()
                 << "Points pushed:" << m_pointsPushed.load()
                 << "Frame drops:" << m_frameDrops.load()
                 << "Trade queue size:" << m_tradeQueue.size()
                 << "OrderBook queue size:" << m_orderBookQueue.size();
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
    
    // Color based on trade side
    if (trade.side == AggressorSide::Buy) {
        point.r = 0.0f; point.g = 1.0f; point.b = 0.0f; point.a = 0.8f; // Green
    } else {
        point.r = 1.0f; point.g = 0.0f; point.b = 0.0f; point.a = 0.8f; // Red
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
    qDebug() << "🔧 GPUDataAdapter: Reserve size set to" << size;
}

void GPUDataAdapter::resetWriteCursors() {
    m_tradeWriteCursor = 0;
    m_heatmapWriteCursor = 0;
} 