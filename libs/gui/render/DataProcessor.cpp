#include "DataProcessor.hpp"
#include "GridViewState.hpp"
#include "../../core/SentinelLogging.hpp"
#include <chrono>

DataProcessor::DataProcessor(QObject* parent)
    : QObject(parent) {
    
    // Setup 100ms order book polling timer for consistent updates
    m_snapshotTimer = new QTimer(this);
    connect(m_snapshotTimer, &QTimer::timeout, this, &DataProcessor::captureOrderBookSnapshot);
    
    sLog_Init("🚀 DataProcessor: Initialized for V2 architecture");
}

DataProcessor::~DataProcessor() {
    stopProcessing();
}

void DataProcessor::startProcessing() {
    if (m_snapshotTimer && !m_snapshotTimer->isActive()) {
        m_snapshotTimer->start(100);  // 100ms base resolution
        sLog_Init("🚀 DataProcessor: Started processing with 100ms snapshots");
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
        
        // Initialize viewport from first trade if needed - delegate to ViewState
        if (m_viewState && !m_viewState->isTimeWindowValid()) {
            initializeViewportFromTrade(trade);
        }
        
        // 🚀 THROTTLED TRADE LOGGING: Only log every 20th trade to reduce spam
        static int tradeUpdateCount = 0;
        if (++tradeUpdateCount % 20 == 0) {
            sLog_Trades("📊 DataProcessor TRADE UPDATE: Processing trade #" << tradeUpdateCount);
        }
    }
    
    emit dataUpdated();
    
    // Debug logging for first few trades
    static int realTradeCount = 0;
    if (++realTradeCount <= 10) {
        sLog_Trades("🎯 DataProcessor TRADE #" << realTradeCount << ": $" << trade.price 
                 << " vol:" << trade.size 
                 << " timestamp:" << timestamp);
    }
}

void DataProcessor::onOrderBookUpdated(const OrderBook& book) {
    if (book.product_id.empty() || book.bids.empty() || book.asks.empty()) return;
    
    {
        std::lock_guard<std::mutex> lock(m_dataMutex);
        
        // Store latest order book for snapshots
        m_latestOrderBook = book;
        m_hasValidOrderBook = true;
        
        // Initialize viewport from order book if needed - delegate to ViewState
        if (m_viewState && !m_viewState->isTimeWindowValid()) {
            initializeViewportFromOrderBook(book);
        }
    }
    
    emit dataUpdated();
    
    // Debug logging for first few order books
    static int orderBookCount = 0;
    if (++orderBookCount <= 10) {
        sLog_Network("🎯 DataProcessor ORDER BOOK #" << orderBookCount 
                 << " Bids:" << book.bids.size() << " Asks:" << book.asks.size());
    }
}

void DataProcessor::captureOrderBookSnapshot() {
    if (!m_hasValidOrderBook || !m_liquidityEngine) return;
    
    {
        std::lock_guard<std::mutex> lock(m_dataMutex);
        
        // ALWAYS pass the latest order book to the engine to ensure a continuous time series.
        // This prevents visual gaps in the heatmap when the market is quiet.
        if (m_viewState && m_viewState->isTimeWindowValid()) {
            m_liquidityEngine->addOrderBookSnapshot(m_latestOrderBook, 
                m_viewState->getMinPrice(), m_viewState->getMaxPrice());
        } else {
            // Fallback to unfiltered if viewport not yet initialized
            m_liquidityEngine->addOrderBookSnapshot(m_latestOrderBook);
        }
    }
    
    emit dataUpdated();
}

void DataProcessor::initializeViewportFromTrade(const Trade& trade) {
    if (!m_viewState) return;
    
    auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        trade.timestamp.time_since_epoch()).count();
    
    qint64 timeStart = timestamp - 30000;  // 30 seconds ago
    qint64 timeEnd = timestamp + 30000;    // 30 seconds ahead
    double minPrice = trade.price - 100.0;
    double maxPrice = trade.price + 100.0;
    
    m_viewState->setViewport(timeStart, timeEnd, minPrice, maxPrice);
    
    sLog_Init("🎯 DataProcessor VIEWPORT INITIALIZED FROM TRADE:");
    sLog_Init("   Time: " << timeStart << " - " << timeEnd);
    sLog_Init("   Price: $" << minPrice << " - $" << maxPrice);
    sLog_Init("   Based on trade: $" << trade.price << " at " << timestamp);
    
    emit viewportInitialized();
}

void DataProcessor::initializeViewportFromOrderBook(const OrderBook& book) {
    if (!m_viewState) return;
    
    double bestBid = book.bids[0].price;
    double bestAsk = book.asks[0].price;
    double midPrice = (bestBid + bestAsk) / 2.0;
    
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    
    qint64 timeStart = now - 30000;  // 30 seconds ago
    qint64 timeEnd = now + 30000;    // 30 seconds ahead
    double minPrice = midPrice - 100.0;
    double maxPrice = midPrice + 100.0;
    
    m_viewState->setViewport(timeStart, timeEnd, minPrice, maxPrice);
    
    sLog_Init("🎯 DataProcessor VIEWPORT FROM ORDER BOOK:");
    sLog_Init("   Mid Price: $" << midPrice);
    sLog_Init("   Price Window: $" << minPrice << " - $" << maxPrice);
    
    emit viewportInitialized();
}

void DataProcessor::clearData() {
    std::lock_guard<std::mutex> lock(m_dataMutex);
    
    m_latestOrderBook = OrderBook{};
    m_hasValidOrderBook = false;
    
    if (m_viewState) {
        m_viewState->resetZoom();  // This resets the viewport state
    }
    
    sLog_Init("🎯 DataProcessor: Data cleared");
    emit dataUpdated();
}