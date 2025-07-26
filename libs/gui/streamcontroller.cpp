#include "StreamController.h"
#include "SentinelLogging.hpp"
#include "MarketDataCore.hpp"
#include <QDebug>
// Phase 4: gpudataadapter.h removed - using GridIntegrationAdapter with Qt signals

StreamController::StreamController(QObject* parent)
    : QObject(parent)
    , m_client(nullptr)
    , m_pollTimer(nullptr)
    , m_orderBookPollTimer(nullptr)
{
    sLog_Init("StreamController created");
}

StreamController::~StreamController() {
    stop();
    sLog_Init("StreamController destroyed");
}

void StreamController::start(const std::vector<std::string>& symbols) {
    sLog_Init("Starting StreamController...");
    
    // Store the symbols for later use
    m_symbols = symbols;
    
    // Create the stream client (like instantiating a Python class)
    m_client = std::make_unique<CoinbaseStreamClient>();
    
    // Subscribe to the symbols
    m_client->subscribe(symbols);
    
    // Start the client
    m_client->start();
    
    // 🔥 NEW: Connect to real-time WebSocket signals instead of polling
    if (m_client->getMarketDataCore()) {
        connect(m_client->getMarketDataCore(), &MarketDataCore::tradeReceived,
                this, &StreamController::onTradeReceived, Qt::QueuedConnection);
        connect(m_client->getMarketDataCore(), &MarketDataCore::orderBookUpdated,
                this, &StreamController::onOrderBookUpdated, Qt::QueuedConnection);
        connect(m_client->getMarketDataCore(), &MarketDataCore::connectionStatusChanged,
                this, &StreamController::onConnectionStatusChanged, Qt::QueuedConnection);
        
        sLog_Init("✅ Connected to real-time WebSocket signals");
    } else {
        sLog_Warning("⚠️ MarketDataCore not available, falling back to polling");
        
        // Fallback to polling if real-time signals not available
        m_pollTimer = new QTimer(this);
        connect(m_pollTimer, &QTimer::timeout, this, &StreamController::pollForTrades);
        m_pollTimer->start(100);

        m_orderBookPollTimer = new QTimer(this);
        connect(m_orderBookPollTimer, &QTimer::timeout, this, &StreamController::pollForOrderBooks);
        m_orderBookPollTimer->start(100);
    }
    
    // Emit connected signal
    emit connected();
    
    sLog_Init("StreamController started successfully");
}

void StreamController::stop() {
    sLog_Init("Stopping StreamController...");
    
    // 1. Reset the client first. This is critical.
    // This stops the underlying threads in MarketDataCore and ensures no
    // new work is done, even if a stray timer event fires.
    m_client.reset();
    
    // 2. Now that the client is gone, we can safely stop and delete the timers.
    if (m_pollTimer) {
        m_pollTimer->stop();
        m_pollTimer->deleteLater();
        m_pollTimer = nullptr;
    }
    if (m_orderBookPollTimer) {
        m_orderBookPollTimer->stop();
        m_orderBookPollTimer->deleteLater();
        m_orderBookPollTimer = nullptr;
    }
    
    // Clear tracking data
    m_lastTradeIds.clear();
    
    // Emit disconnected signal
    emit disconnected();
    
    sLog_Init("StreamController stopped");
}

// 🔥 NEW: Real-time signal handlers
void StreamController::onTradeReceived(const Trade& trade) {
    // 🔥 REAL-TIME: Process trade immediately from WebSocket
    // Log only the first 10 trades
    static int tradeCount = 0;
    if (tradeCount < 10) {
        sLog_Trades("🚀 REAL-TIME: Received trade" << QString::fromStdString(trade.product_id) 
                   << "$" << trade.price << "size:" << trade.size);
        tradeCount++;
    }

    // 🎯 PHASE 4: Direct Qt signal pipeline (replaced lock-free GPUDataAdapter)
    // Trades now flow: StreamController -> GridIntegrationAdapter via Qt signals
    
    // Keep Qt signal for backward compatibility
    emit tradeReceived(trade);
}

void StreamController::onOrderBookUpdated(const OrderBook& orderBook) {
    // 🔥 REAL-TIME: Process order book immediately from WebSocket
    //sLog_Network("🔥 REAL-TIME: Received order book for" << QString::fromStdString(orderBook.product_id)
                 //<< "Bids:" << orderBook.bids.size() << "Asks:" << orderBook.asks.size());
    
    // Emit to GUI components
    emit orderBookUpdated(orderBook);
}

void StreamController::onConnectionStatusChanged(bool connected) {
    if (connected) {
        sLog_Connection("✅ WebSocket connection established");
    } else {
        sLog_Connection("❌ WebSocket connection lost");
    }
}

void StreamController::pollForTrades() {
    // Check if client exists
    if (!m_client) return;
    
    // Poll each symbol for new trades
    for (const auto& symbol : m_symbols) {
        // Get new trades since last seen trade ID
        std::string lastTradeId = m_lastTradeIds.count(symbol) ? m_lastTradeIds[symbol] : "";
        auto newTrades = m_client->getNewTrades(symbol, lastTradeId);
        
        // Debug logging
        if (!newTrades.empty()) {
            sLog_Trades("🚀 StreamController: Found" << newTrades.size() << "new trades for" << QString::fromStdString(symbol));
        }
        
        // Process each new trade
        for (const auto& trade : newTrades) {
            // Update last seen trade ID
            m_lastTradeIds[symbol] = trade.trade_id;
            
            // 🎯 PHASE 4: Direct Qt signal pipeline (replaced lock-free GPUDataAdapter)
            // Trades now flow: StreamController -> GridIntegrationAdapter via Qt signals
            
            // 🔥 THROTTLED LOGGING: Only log every 50th trade processing to reduce spam
            static int processLogCount = 0;
            if (++processLogCount % 50 == 1) { // Log 1st, 51st, 101st trade, etc.
                sLog_Trades("📤 Pushing trade to GPU queue:" << QString::fromStdString(trade.product_id) 
                         << "$" << trade.price << "size:" << trade.size 
                         << "[" << processLogCount << " trades processed]");
            }
            
            // Keep Qt signal for backward compatibility (will be removed in final version)
            emit tradeReceived(trade);
        }
    }
}

void StreamController::pollForOrderBooks() {
    if (!m_client) return;

    // qDebug() << "Polling for order books..."; // Let's be more specific
    for (const auto& symbol : m_symbols) {
        auto book = m_client->getOrderBook(symbol);
        
        // 🔥 THROTTLED LOGGING: Only log every 20th order book poll to reduce spam
        static int pollLogCount = 0;
        if (++pollLogCount % 20 == 1) { // Log 1st, 21st, 41st poll, etc.
            sLog_Network("Polled client for" << QString::fromStdString(symbol) 
                     << "order book. Bids:" << book.bids.size() << "Asks:" << book.asks.size()
                     << "[" << pollLogCount << " polls total]");
        }
        
        if (!book.bids.empty() || !book.asks.empty()) {
            emit orderBookUpdated(book);
        }
    }
}