#include "streamcontroller.h"
#include <QDebug>  // For debugging output (like Python's print statements)

// 🐍 Python: def __init__(self, parent=None):
// ⚡ C++: StreamController::StreamController(QObject* parent)
//        The :: means "this method belongs to StreamController class"
StreamController::StreamController(QObject* parent)
    : QObject(parent)  // 🪄 This calls the parent class constructor (like super().__init__())
    , m_client(nullptr)  // Initialize to null (like Python's None)
    , m_pollTimer(nullptr)
    , m_orderBookPollTimer(nullptr)
{
    // 🐍 Python: print("StreamController created")
    // ⚡ C++: qDebug() << "StreamController created";
    qDebug() << "StreamController created";
}

// 🐍 Python: def __del__(self):
// ⚡ C++: StreamController::~StreamController() - destructor with ~
StreamController::~StreamController() {
    stop();  // Make sure we clean up properly
    qDebug() << "StreamController destroyed";
}

// 🐍 Python: def start(self, symbols):
// ⚡ C++: void StreamController::start(const std::vector<std::string>& symbols)
//        "const &" means "read-only reference" - efficient like Python's object passing
void StreamController::start(const std::vector<std::string>& symbols) {
    qDebug() << "Starting StreamController...";
    
    // Store the symbols for later use
    // 🐍 Python: self.symbols = symbols
    // ⚡ C++: m_symbols = symbols;  (the = operator copies the vector)
    m_symbols = symbols;
    
    // Create the stream client (like instantiating a Python class)
    // 🐍 Python: self.client = CoinbaseStreamClient()
    // ⚡ C++: m_client = std::make_unique<CoinbaseStreamClient>();
    //        make_unique creates a smart pointer (auto memory management)
    m_client = std::make_unique<CoinbaseStreamClient>();
    
    // Subscribe to the symbols
    // 🐍 Python: self.client.subscribe(symbols)
    // ⚡ C++: m_client->subscribe(symbols);
    //        -> is used for pointer access (like . in Python)
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
    // 🐍 Python: self.on_connected()  # call callback
    // ⚡ C++: emit connected();        # emit Qt signal
    emit connected();
    
    qDebug() << "StreamController started successfully";
}

// 🐍 Python: def stop(self):
// ⚡ C++: void StreamController::stop()
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
    // 🐍 Python: self.client = None
    // ⚡ C++: m_client.reset();  # smart pointer cleanup
    m_client.reset();
    
    // Clear tracking data
    // 🐍 Python: self.last_trade_ids.clear()
    // ⚡ C++: m_lastTradeIds.clear();
    m_lastTradeIds.clear();
    
    // Emit disconnected signal
    emit disconnected();
    
    qDebug() << "StreamController stopped";
}

// 🐍 Python: def poll_for_trades(self):  # private method
// ⚡ C++: void StreamController::pollForTrades()  # private slot
void StreamController::pollForTrades() {
    // Check if client exists
    // 🐍 Python: if not self.client: return
    // ⚡ C++: if (!m_client) return;  # ! means "not" like Python's "not"
    if (!m_client) return;
    
    // Poll each symbol for new trades
    // 🐍 Python: for symbol in self.symbols:
    // ⚡ C++: for (const auto& symbol : m_symbols) {
    //        "const auto&" lets the compiler figure out the type (like Python's dynamic typing)
    for (const auto& symbol : m_symbols) {
        // Get new trades since last seen trade ID
        // 🐍 Python: new_trades = self.client.get_new_trades(symbol, self.last_trade_ids.get(symbol, ""))
        // ⚡ C++: auto newTrades = m_client->getNewTrades(symbol, m_lastTradeIds[symbol]);
        std::string lastTradeId = m_lastTradeIds.count(symbol) ? m_lastTradeIds[symbol] : "";
        auto newTrades = m_client->getNewTrades(symbol, lastTradeId);
        
        // Debug logging
        if (!newTrades.empty()) {
            qDebug() << "🚀 StreamController: Found" << newTrades.size() << "new trades for" << QString::fromStdString(symbol);
        }
        
        // Process each new trade
        // 🐍 Python: for trade in new_trades:
        // ⚡ C++: for (const auto& trade : newTrades) {
        for (const auto& trade : newTrades) {
            // Update last seen trade ID
            // 🐍 Python: self.last_trade_ids[symbol] = trade.trade_id
            // ⚡ C++: m_lastTradeIds[symbol] = trade.trade_id;
            m_lastTradeIds[symbol] = trade.trade_id;
            
            // Emit the trade signal (this is where the magic happens!)
            // 🐍 Python: self.on_trade_received(trade)  # call callback
            // ⚡ C++: emit tradeReceived(trade);        # emit Qt signal
            qDebug() << "📤 Emitting trade:" << QString::fromStdString(trade.product_id) 
                     << "$" << trade.price << "size:" << trade.size;
            emit tradeReceived(trade);
        }
    }
}

void StreamController::pollForOrderBooks() {
    if (!m_client) return;

    // qDebug() << "Polling for order books..."; // Let's be more specific
    for (const auto& symbol : m_symbols) {
        auto book = m_client->getOrderBook(symbol);
        qDebug() << "Polled client for" << QString::fromStdString(symbol) << "order book. Bids:" << book.bids.size() << "Asks:" << book.asks.size();
        if (!book.bids.empty() || !book.asks.empty()) {
            emit orderBookUpdated(book);
        }
    }
}