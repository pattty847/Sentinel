#include "streamcontroller.h"
#include "gpudataadapter.h"
#include "Log.hpp"

static constexpr auto CAT = "StreamCtrl";

StreamController::StreamController(QObject* parent)
    : QObject(parent)
    , m_client(nullptr)
    , m_pollTimer(nullptr)
    , m_orderBookPollTimer(nullptr)
{
    LOG_I(CAT, "StreamController created");
}

StreamController::~StreamController() {
    stop();
    LOG_I(CAT, "StreamController destroyed");
}

void StreamController::start(const std::vector<std::string>& symbols) {
    LOG_I(CAT, "Starting StreamController...");
    
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
    
    LOG_I(CAT, "StreamController started successfully");
}

void StreamController::stop() {
    LOG_I(CAT, "Stopping StreamController...");
    
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
    
    LOG_I(CAT, "StreamController stopped");
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
            LOG_D(CAT, "🚀 StreamController: Found {} new trades for {}", newTrades.size(), QString::fromStdString(symbol).toStdString());
        }
        
        // Process each new trade
        for (const auto& trade : newTrades) {
            // Update last seen trade ID
            m_lastTradeIds[symbol] = trade.trade_id;
            
            // 🚀 LOCK-FREE PIPELINE: Push to GPU adapter instead of Qt signals
            if (m_gpuAdapter && !m_gpuAdapter->pushTrade(trade)) {
                LOG_W(CAT, "⚠️ StreamController: GPU trade queue full! Trade dropped.");
            }
            
            // 🔥 THROTTLED LOGGING: Only log every 50th trade processing to reduce spam
            static int processLogCount = 0;
            if (++processLogCount % 50 == 1) {
                LOG_D(CAT, "📤 Pushing trade to GPU queue: {} $ {} size:{} [{} trades processed]",
                      QString::fromStdString(trade.product_id).toStdString(), trade.price, trade.size, processLogCount);
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
        if (++pollLogCount % 20 == 1) {
            LOG_D(CAT, "Polled client for {} order book. Bids:{} Asks:{} [{} polls total]",
                  QString::fromStdString(symbol).toStdString(), book.bids.size(), book.asks.size(), pollLogCount);
        }
        
        if (!book.bids.empty() || !book.asks.empty()) {
            emit orderBookUpdated(book);
        }
    }
}