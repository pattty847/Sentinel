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
#include <chrono>
#include <limits>
#include <cmath>

DataProcessor::DataProcessor(QObject* parent)
    : QObject(parent) {
    
    m_snapshotTimer = new QTimer(this);
    connect(m_snapshotTimer, &QTimer::timeout, this, &DataProcessor::captureOrderBookSnapshot);
    
    sLog_App("🚀 DataProcessor: Initialized for V2 architecture");
}

DataProcessor::~DataProcessor() {
    stopProcessing();
}

void DataProcessor::startProcessing() {
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