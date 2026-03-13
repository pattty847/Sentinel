#pragma once

#include "OrderStore.hpp"
#include "PaperExecutionAdapter.hpp"
#include "PositionManager.hpp"

#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace trading {

struct TradingResult {
    std::vector<OrderUpdate> orderUpdates;
    std::vector<PositionUpdate> positionUpdates;
    std::vector<PnlSnapshot> pnlSnapshots;
};

class TradingEngine {
public:
    using PriceResolver = std::function<double(const std::string& symbol)>;

    TradingEngine(PriceResolver resolver, double slippageBps);

    TradingResult onCommand(const TradeCommand& command);
    TradingResult onExternalFill(const std::string& orderId,
                                 double cumulativeFilledQty,
                                 double fillPrice);

    // Called on every trade tick to check if resting limit orders should fill.
    // Returns fills and position updates for any triggered orders.
    TradingResult onTick(const std::string& symbol, double lastTradePrice, int64_t timestampMs);

    std::optional<Order> findOrder(const std::string& orderId) const;

private:
    TradingResult handlePlaceOrder(const TradeCommand& command);
    TradingResult handleCancelOrder(const TradeCommand& command);
    TradingResult handleCancelAll(const TradeCommand& command);
    TradingResult handleFlatten(const TradeCommand& command);

    TradingResult fillOrder(Order& order, double fillPrice, double markPrice, int64_t timestampMs);
    PnlSnapshot buildSnapshot(const std::string& symbol, double markPrice, int64_t timestampMs, const std::string& algoId) const;

    std::string nextOrderId();

    mutable std::mutex m_mutex;
    uint64_t m_orderSeq = 0;
    PriceResolver m_priceResolver;
    OrderStore m_orders;
    PositionManager m_positions;
    PaperExecutionAdapter m_execution;
};

TradeCommand parseTradeCommandJson(const std::string& raw);

} // namespace trading
