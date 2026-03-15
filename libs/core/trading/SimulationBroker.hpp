#pragma once

#include "BacktestTypes.hpp"
#include "IExecutionModel.hpp"

#include <memory>
#include <unordered_map>

namespace trading {

class SimulationBroker {
public:
    SimulationBroker(TradingEngine::PriceResolver resolver,
                     std::unique_ptr<IExecutionModel> executionModel,
                     double slippageBps);

    std::vector<ExecutionEvent> onMarketEvent(const MarketEvent& event);
    std::vector<ExecutionEvent> submitIntents(const std::vector<OrderIntent>& intents);

    BrokerSnapshot snapshotFor(const std::string& symbol, int64_t timestampMs) const;
    BacktestResult buildResult(const BacktestConfig& config) const;

private:
    double resolvePrice(const std::string& symbol) const;
    std::vector<ExecutionEvent> recordTradingResult(const TradingResult& result, int64_t timestampMs);
    static ExecutionEventType classifyOrderEvent(const OrderUpdate& update);
    void updateSummaryFromPnl(const PnlSnapshot& snapshot);

    TradingEngine m_engine;
    std::unique_ptr<IExecutionModel> m_executionModel;
    std::unordered_map<std::string, double> m_lastTradePriceBySymbol;
    BacktestResult m_result;
};

} // namespace trading
