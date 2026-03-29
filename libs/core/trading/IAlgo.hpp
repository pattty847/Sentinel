#pragma once

#include "TradingTypes.hpp"

#include <string>
#include <vector>

namespace trading {

// Parameters bag for configuring an algorithm at runtime.
struct AlgoParams {
    double spreadBps = 10.0;      // Half-spread width in basis points
    double orderQty = 0.01;       // Order size per side
    double maxPositionQty = 0.1;  // Maximum net inventory before skewing
    double skewBps = 5.0;         // Inventory skew offset per unit of position
};

// Abstract interface for a server-side trading algorithm.
// Receives market ticks, produces TradeCommands, and can be started/stopped.
class IAlgo {
public:
    virtual ~IAlgo() = default;

    virtual const std::string& id() const = 0;
    virtual const std::string& displayName() const = 0;

    virtual void start(const std::string& symbol, const AlgoParams& params) = 0;
    virtual void stop() = 0;
    virtual bool isRunning() const = 0;
    virtual const std::string& symbol() const = 0;

    // Called on every trade tick. Returns any trade commands the algo wants to issue.
    virtual std::vector<TradeCommand> onTick(double lastTradePrice, int64_t timestampMs,
                                              const std::vector<Order>& openOrders,
                                              const Position& position) = 0;

    // Called when an order belonging to this algo is updated.
    virtual void onOrderUpdate(const OrderUpdate& update) = 0;
};

} // namespace trading
