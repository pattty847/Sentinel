#pragma once

#include "IAlgo.hpp"
#include "TradingEngine.hpp"

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace trading {

// AlgoOrderEvent: broadcast to clients so they can overlay algo activity on the chart.
struct AlgoOrderEvent {
    std::string algoId;
    std::string orderId;
    std::string symbol;
    OrderSide side = OrderSide::Unknown;
    OrderType orderType = OrderType::Limit;
    double price = 0.0;
    double qty = 0.0;
    OrderStatus status = OrderStatus::New;
    int64_t timestampMs = 0;
};

/**
 * AlgoEngine — manages and ticks IAlgo instances.
 *
 * All public methods are thread-safe (called from the data-processing thread).
 * The callback for broadcasting results (orders, position updates, PnL, algo events)
 * is invoked synchronously outside the engine mutex.
 */
class AlgoEngine {
public:
    using ResultCallback = std::function<void(TradingResult, std::vector<AlgoOrderEvent>)>;

    explicit AlgoEngine(TradingEngine& engine);

    // Register a built-in algorithm by ID. Called once at startup.
    void registerAlgo(std::unique_ptr<IAlgo> algo);

    // Start a named algorithm on a symbol with parameters.
    // Returns false if the algo is unknown.
    bool startAlgo(const std::string& algoId, const std::string& symbol, const AlgoParams& params);

    // Stop a running algorithm.
    void stopAlgo(const std::string& algoId);

    // Called on every trade tick. Ticks all running algos and processes their commands.
    void onTick(const std::string& symbol, double lastTradePrice, int64_t timestampMs);

    // Called when an order update arrives — forwarded to the relevant algo.
    void onOrderUpdate(const OrderUpdate& update);

    // Set the callback to receive trading results + algo events.
    void setResultCallback(ResultCallback cb);

    std::vector<std::string> runningAlgos() const;

private:
    mutable std::mutex m_mutex;
    TradingEngine& m_engine;
    std::unordered_map<std::string, std::unique_ptr<IAlgo>> m_algos;
    ResultCallback m_callback;
};

} // namespace trading
