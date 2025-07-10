#ifndef STREAMCONTROLLER_H
#define STREAMCONTROLLER_H

// 🐍 Python: import something
// ⚡ C++: #include <something> - these are like imports but happen at compile time
#include <QObject>     // Qt's base class for signal/slot magic
#include <QTimer>      // For periodic polling (like Python's time.sleep in a loop)
#include <memory>      // For smart pointers (like Python's automatic memory management)
#include <unordered_map> // For the last trade IDs tracking (like Python's dict)
#include "CoinbaseStreamClient.hpp"
#include "tradedata.h"

// Forward declaration for lock-free GPU pipeline
class GPUDataAdapter;

// 🐍 Python: class StreamController:
// ⚡ C++: class StreamController : public QObject {
//        The ": public QObject" is like inheritance in Python!
class StreamController : public QObject {
    Q_OBJECT  // 🪄 Qt magic macro - this enables signals/slots (like Python decorators)
    
public:
    // 🐍 Python: def __init__(self, parent=None):
    // ⚡ C++: explicit StreamController(QObject* parent = nullptr);
    //        "explicit" prevents weird automatic conversions
    //        "nullptr" is like Python's None
    explicit StreamController(QObject* parent = nullptr);
    
    // 🐍 Python: def __del__(self):  (destructor)
    // ⚡ C++: ~StreamController();   (destructor with ~ symbol)
    ~StreamController();

// 🐍 Python: No direct equivalent, but think of these as "events you can listen to"
// ⚡ C++: signals are like Python's event system or callback functions
signals:
    // 🐍 Python: def on_trade_received(self, trade): pass  # callback
    // ⚡ C++: void tradeReceived(const Trade& trade);      # signal
    //        "const Trade&" means "read-only reference to Trade object"
    void tradeReceived(const Trade& trade);
    void orderBookUpdated(const OrderBook& book);
    void connected();
    void disconnected();

// 🐍 Python: These would be regular methods you call
// ⚡ C++: public slots are methods that can be called AND connected to signals
public slots:
    void start(const std::vector<std::string>& symbols);
    void stop();
    
    // GPU pipeline integration
    void setGPUAdapter(GPUDataAdapter* adapter) { m_gpuAdapter = adapter; }
    
    // 🚀 ULTRA-FAST: Expose CoinbaseStreamClient for FastOrderBook access
    CoinbaseStreamClient* getClient() const { return m_client.get(); }

// 🐍 Python: These would be private methods (def _method_name)
// ⚡ C++: private slots are internal methods connected to signals
private slots:
    void pollForTrades();
    void pollForOrderBooks();

// 🐍 Python: These would be private variables (self._variable)
// ⚡ C++: private member variables (prefixed with m_ by convention)
private:
    // 🐍 Python: self.client = CoinbaseStreamClient()
    // ⚡ C++: std::unique_ptr<CoinbaseStreamClient> m_client;
    //        unique_ptr is like Python's automatic garbage collection
    std::unique_ptr<CoinbaseStreamClient> m_client;
    
    // 🐍 Python: self.timer = Timer()
    // ⚡ C++: QTimer* m_pollTimer;
    //        The * means "pointer to QTimer" (like Python's references)
    QTimer* m_pollTimer;
    
    // 🐍 Python: self.orderBookPollTimer = Timer()
    // ⚡ C++: QTimer* m_orderBookPollTimer;
    //        The * means "pointer to QTimer" (like Python's references)
    QTimer* m_orderBookPollTimer;
    
    // 🐍 Python: self.symbols = []
    // ⚡ C++: std::vector<std::string> m_symbols;
    //        vector is like Python's list, string is like Python's str
    std::vector<std::string> m_symbols;
    
    // 🐍 Python: self.last_trade_ids = {}
    // ⚡ C++: std::unordered_map<std::string, std::string> m_lastTradeIds;
    //        unordered_map is like Python's dict
    std::unordered_map<std::string, std::string> m_lastTradeIds;
    
    // Lock-free GPU pipeline integration
    GPUDataAdapter* m_gpuAdapter = nullptr;
};

#endif // STREAMCONTROLLER_H 