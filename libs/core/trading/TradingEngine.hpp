#pragma once

#include "OrderStore.hpp"
#include "PaperExecutionAdapter.hpp"
#include "PositionManager.hpp"

#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace trading {

struct TradingResult {
    std::vector<OrderUpdate> orderUpdates;
    std::vector<PositionUpdate> positionUpdates;
    std::vector<PnlSnapshot> pnlSnapshots;
    std::vector<RiskOrderUpdate> riskOrderUpdates;
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
    std::vector<Order> getOpenOrders(const std::string& symbol, const std::string& algoId = "") const;
    std::optional<Position> getPosition(const std::string& symbol) const;

private:
    struct AttachedRiskState {
        bool hasTakeProfit = false;
        double takeProfitPrice = 0.0;
        bool hasStopLoss = false;
        double stopLossPrice = 0.0;
    };

    TradingResult handlePlaceOrder(const TradeCommand& command);
    TradingResult handleCancelOrder(const TradeCommand& command);
    TradingResult handleCancelAll(const TradeCommand& command);
    TradingResult handleFlatten(const TradeCommand& command);
    TradingResult handleSetAttachedRisk(const TradeCommand& command);

    TradingResult fillOrder(Order& order, double fillPrice, double markPrice, int64_t timestampMs);
    TradingResult handleTriggeredRiskExit(const std::string& symbol,
                                          const Position& position,
                                          const AttachedRiskState& risk,
                                          double lastTradePrice,
                                          int64_t timestampMs);
    std::optional<AttachedRiskState> findAttachedRisk(const std::string& symbol) const;
    RiskOrderUpdate buildRiskOrderUpdate(const std::string& symbol, const AttachedRiskState& risk) const;
    void appendRiskUpdate(TradingResult& out, const std::string& symbol, const AttachedRiskState& risk) const;
    bool clearAttachedRiskIfPresent(TradingResult& out, const std::string& symbol);
    bool shouldClearAttachedRiskAfterFill(double priorQty, double nextQty) const;
    static bool sameSide(double lhsQty, double rhsQty);
    PnlSnapshot buildSnapshot(const std::string& symbol, double markPrice, int64_t timestampMs, const std::string& algoId) const;

    std::string nextOrderId();

    mutable std::mutex m_mutex;
    uint64_t m_orderSeq = 0;
    PriceResolver m_priceResolver;
    OrderStore m_orders;
    PositionManager m_positions;
    PaperExecutionAdapter m_execution;
    std::unordered_map<std::string, AttachedRiskState> m_attachedRiskBySymbol;
};

TradeCommand parseTradeCommandJson(const std::string& raw);

} // namespace trading
