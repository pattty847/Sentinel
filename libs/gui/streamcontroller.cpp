#include "streamcontroller.h"
#include "gpudataadapter.h"
#include <QDebug>

StreamController::StreamController(QObject* parent)
    : QObject(parent)
    , m_client(nullptr)
    , m_pollTimer(nullptr)
    , m_orderBookPollTimer(nullptr)
{
    qDebug() << "StreamController created";
}

StreamController::~StreamController() {
    stop();
    qDebug() << "StreamController destroyed";
}

void StreamController::start(const std::vector<std::string>& symbols) {
    qDebug() << "Starting StreamController...";
    
    // Store the symbols for later use
    m_symbols = symbols;
    
    // Create the stream client (like instantiating a Python class)
    m_client = std::make_unique<CoinbaseStreamClient>();
    
    // Subscribe to the symbols
    m_client->subscribe(symbols);
    
    // Start the client
    m_client->start();
    
    // Create and configure the polling timers
    m_pollTimer = new QTimer(this);
    connect(m_pollTimer, &QTimer::timeout, this, &StreamController::pollForTrades);
    m_pollTimer->start(100);

    m_orderBookPollTimer = new QTimer(this);
    connect(m_orderBookPollTimer, &QTimer::timeout, this, &StreamController::pollForOrderBooks);
    m_orderBookPollTimer->start(100);
    
    // Emit connected signal
    emit connected();
    
    qDebug() << "StreamController started successfully";
}

void StreamController::stop() {
    qDebug() << "Stopping StreamController...";
    
    // Stop the timers
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
    
    // Reset the client (smart pointer automatically cleans up)
    m_client.reset();
    
    // Clear tracking data
    m_lastTradeIds.clear();
    
    // Emit disconnected signal
    emit disconnected();
    
    qDebug() << "StreamController stopped";
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
            qDebug() << "🚀 StreamController: Found" << newTrades.size() << "new trades for" << QString::fromStdString(symbol);
        }
        
        // Process each new trade
        for (const auto& trade : newTrades) {
            // Update last seen trade ID
            m_lastTradeIds[symbol] = trade.trade_id;
            
            // 🚀 LOCK-FREE PIPELINE: Push to GPU adapter instead of Qt signals
            if (m_gpuAdapter && !m_gpuAdapter->pushTrade(trade)) {
                qWarning() << "⚠️ StreamController: GPU trade queue full! Trade dropped.";
            }
            
            // 🔥 THROTTLED LOGGING: Only log every 50th trade processing to reduce spam
            static int processLogCount = 0;
            if (++processLogCount % 50 == 1) { // Log 1st, 51st, 101st trade, etc.
                qDebug() << "📤 Pushing trade to GPU queue:" << QString::fromStdString(trade.product_id) 
                         << "$" << trade.price << "size:" << trade.size 
                         << "[" << processLogCount << " trades processed]";
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
            qDebug() << "Polled client for" << QString::fromStdString(symbol) 
                     << "order book. Bids:" << book.bids.size() << "Asks:" << book.asks.size()
                     << "[" << pollLogCount << " polls total]";
        }
        
        if (!book.bids.empty() || !book.asks.empty()) {
            emit orderBookUpdated(book);
        }
    }
}