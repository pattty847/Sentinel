#pragma once

#include "IAlgo.hpp"

#include <string>
#include <unordered_map>

namespace trading {

/**
 * AvendellaMM — symmetric market-making algorithm.
 *
 * Strategy:
 *   - Posts a limit buy and limit sell symmetrically around the last trade mid price.
 *   - Cancels and re-quotes when the current quote is stale (mid has moved > requote_threshold).
 *   - Applies inventory skew: if long, shift quotes down to encourage selling; if short, shift up.
 *   - Stops quoting the side that would push inventory beyond max_position_qty.
 */
class AvendellaMM : public IAlgo {
public:
    AvendellaMM();

    const std::string& id() const override { return m_id; }
    const std::string& displayName() const override { return m_displayName; }

    void start(const std::string& symbol, const AlgoParams& params) override;
    void stop() override;
    bool isRunning() const override { return m_running; }

    std::vector<TradeCommand> onTick(double lastTradePrice, int64_t timestampMs,
                                      const std::vector<Order>& openOrders,
                                      const Position& position) override;

    void onOrderUpdate(const OrderUpdate& update) override;

private:
    std::string m_id = "AvendellaMM";
    std::string m_displayName = "Avendella MM";

    bool m_running = false;
    std::string m_symbol;
    AlgoParams m_params;

    double m_lastMid = 0.0;       // Mid price at last quote
    std::string m_bidOrderId;     // ID of resting bid
    std::string m_askOrderId;     // ID of resting ask
    bool m_bidLive = false;
    bool m_askLive = false;

    uint64_t m_cmdSeq = 0;

    // Returns true if the current quote needs to be refreshed.
    bool isStale(double newMid) const;

    std::string nextCmdId();
    TradeCommand makeLimitOrder(OrderSide side, double price, int64_t timestampMs) const;
    TradeCommand makeCancelOrder(const std::string& orderId, int64_t timestampMs) const;
};

} // namespace trading
