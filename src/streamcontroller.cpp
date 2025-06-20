#include "streamcontroller.h"
#include <QDebug>  // For debugging output (like Python's print statements)

// 🐍 Python: def __init__(self, parent=None):
// ⚡ C++: StreamController::StreamController(QObject* parent)
//        The :: means "this method belongs to StreamController class"
StreamController::StreamController(QObject* parent)
    : QObject(parent)  // 🪄 This calls the parent class constructor (like super().__init__())
    , m_client(nullptr)  // Initialize to null (like Python's None)
    , m_pollTimer(nullptr)
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
    
    // Create and configure the polling timer
    // 🐍 Python: self.timer = Timer()
    // ⚡ C++: m_pollTimer = new QTimer(this);
    //        "new" creates an object, "this" is like Python's self
    m_pollTimer = new QTimer(this);
    
    // Connect timer signal to our polling slot
    // 🐍 Python: self.timer.timeout.connect(self.poll_for_trades)
    // ⚡ C++: connect(m_pollTimer, &QTimer::timeout, this, &StreamController::pollForTrades);
    //        This is Qt's signal/slot connection (like Python event handlers)
    connect(m_pollTimer, &QTimer::timeout, this, &StreamController::pollForTrades);
    
    // Start the timer with 100ms interval (super fast polling!)
    // 🐍 Python: self.timer.start(100)  # 100ms
    // ⚡ C++: m_pollTimer->start(100);
    m_pollTimer->start(100);
    
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
    
    // Stop the timer
    // 🐍 Python: if self.timer: self.timer.stop()
    // ⚡ C++: if (m_pollTimer) { m_pollTimer->stop(); }
    if (m_pollTimer) {
        m_pollTimer->stop();
        m_pollTimer->deleteLater();  // Qt's way of safe deletion
        m_pollTimer = nullptr;       // Set to null (like Python's None)
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
        auto newTrades = m_client->getNewTrades(symbol, m_lastTradeIds[symbol]);
        
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
            emit tradeReceived(trade);
        }
    }
}

// 🪄 Qt magic: This line tells Qt to generate the signal/slot code
// It's like Python decorators but happens at compile time
#include "streamcontroller.moc" 