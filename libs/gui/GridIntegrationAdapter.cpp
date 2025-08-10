#include "GridIntegrationAdapter.h"
#include "SentinelLogging.hpp"
#include <QWriteLocker>
#include <QReadLocker>
#include <QDebug>

GridIntegrationAdapter::GridIntegrationAdapter(QObject* parent)
    : QObject(parent)
    , m_bufferCleanupTimer(new QTimer(this))
    , m_processingTimer(new QTimer(this))
{
    // Setup buffer cleanup timer (30 seconds like migration plan)
    m_bufferCleanupTimer->setInterval(30000);
    connect(m_bufferCleanupTimer, &QTimer::timeout, this, &GridIntegrationAdapter::trimOldData);
    m_bufferCleanupTimer->start();
    
    // Setup processing timer (16ms batching like old GPUDataAdapter)
    m_processingTimer->setInterval(16);
    connect(m_processingTimer, &QTimer::timeout, this, &GridIntegrationAdapter::processDataBuffer);
    m_processingTimer->start();
    
    sLog_App("🎯 GridIntegrationAdapter: Initialized as primary data pipeline hub");
}

void GridIntegrationAdapter::setGridModeEnabled(bool enabled) {
    // 🎯 PHASE 5: Always grid-only mode now - simplified logic
    if (m_gridModeEnabled == enabled) return;
    
    m_gridModeEnabled = enabled;
    emit gridModeChanged(enabled);
    
    // Populate grid with existing buffer data when enabling
    if (enabled && m_gridRenderer) {
        QReadLocker locker(&m_dataLock);
        for (const auto& trade : m_tradeBuffer) {
            processTradeForGrid(trade);
        }
        for (const auto& orderBook : m_orderBookBuffer) {
            processOrderBookForGrid(orderBook);
        }
    }
}

void GridIntegrationAdapter::setGridRenderer(UnifiedGridRenderer* renderer) {
    m_gridRenderer = renderer;
    if (m_gridRenderer) {
        sLog_App("🎯 GridIntegrationAdapter: Connected to UnifiedGridRenderer");
    }
}

void GridIntegrationAdapter::onTradeReceived(const Trade& trade) {
    {
        QWriteLocker locker(&m_dataLock);
        m_tradeBuffer.push_back(trade);
        
        // Buffer overflow protection
        if (m_tradeBuffer.size() > static_cast<size_t>(m_maxHistorySlices)) {
            emit bufferOverflow(m_tradeBuffer.size(), m_maxHistorySlices);
            m_tradeBuffer.erase(m_tradeBuffer.begin(), m_tradeBuffer.begin() + m_maxHistorySlices/4);
        }
    }
    
    // 🎯 PHASE 5: Pure grid-only mode - always route to grid renderer
    if (m_gridRenderer) {
        processTradeForGrid(trade);
    }
}

void GridIntegrationAdapter::onOrderBookUpdated(const OrderBook& orderBook) {
    {
        QWriteLocker locker(&m_dataLock);
        m_orderBookBuffer.push_back(orderBook);
        
        // Buffer overflow protection  
        if (m_orderBookBuffer.size() > static_cast<size_t>(m_maxHistorySlices)) {
            emit bufferOverflow(m_orderBookBuffer.size(), m_maxHistorySlices);
            m_orderBookBuffer.erase(m_orderBookBuffer.begin(), m_orderBookBuffer.begin() + m_maxHistorySlices/4);
        }
    }
    
    // 🎯 PHASE 5: Pure grid-only mode - always route to grid renderer
    if (m_gridRenderer) {
        processOrderBookForGrid(orderBook);
    }
}

void GridIntegrationAdapter::processTradeForGrid(const Trade& trade) {
    if (!m_gridRenderer) return;
    
    // Phase 3: Wire up actual data flow to UnifiedGridRenderer
    m_gridRenderer->addTrade(trade);
    
    sLog_Data("🎯 Fed trade to UnifiedGridRenderer: price=" << trade.price << " size=" << trade.size);
}

void GridIntegrationAdapter::processOrderBookForGrid(const OrderBook& orderBook) {
    if (!m_gridRenderer) return;
    
    // Phase 3: Wire up actual data flow to UnifiedGridRenderer  
    m_gridRenderer->updateOrderBook(orderBook);
    
    sLog_Data("🎯 Fed order book to UnifiedGridRenderer - bids:" << orderBook.bids.size() << "asks:" << orderBook.asks.size());
}

void GridIntegrationAdapter::processDataBuffer() {
    // This 16ms timer processes any batched data
    // For now, most processing happens immediately in the data receipt functions
    // This can be enhanced later for more sophisticated batching
}

void GridIntegrationAdapter::trimOldData() {
    QWriteLocker locker(&m_dataLock);
    
    // Keep only recent data to prevent memory growth
    // This is a simple time-based cleanup - could be enhanced to use viewport info
    
    const int maxRecentTrades = m_maxHistorySlices / 2;
    if (m_tradeBuffer.size() > static_cast<size_t>(maxRecentTrades)) {
        m_tradeBuffer.erase(m_tradeBuffer.begin(), 
                           m_tradeBuffer.end() - maxRecentTrades);
    }
    
    const int maxRecentOrderBooks = m_maxHistorySlices / 10; // Order books are larger
    if (m_orderBookBuffer.size() > static_cast<size_t>(maxRecentOrderBooks)) {
        m_orderBookBuffer.erase(m_orderBookBuffer.begin(),
                               m_orderBookBuffer.end() - maxRecentOrderBooks);
    }
    
    sLog_Render("🧹 GridIntegrationAdapter: Trimmed buffers - trades:" << m_tradeBuffer.size() 
                     << " orderBooks:" << m_orderBookBuffer.size());
}

void GridIntegrationAdapter::setBatchIntervalMs(int intervalMs) {
    if (intervalMs < 1) intervalMs = 1; // Prevent zero/negative
    if (m_processingTimer->interval() != intervalMs) {
        m_processingTimer->setInterval(intervalMs);
        emit batchIntervalChanged(intervalMs);
        sLog_Render(QString("🕒 Batch interval set to %1 ms (%2 FPS)").arg(intervalMs).arg(1000.0/intervalMs, 0, 'f', 1));
    }
}

int GridIntegrationAdapter::batchIntervalMs() const {
    return m_processingTimer->interval();
}