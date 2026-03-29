#pragma once

#include "AlgoBacktestAdapter.hpp"
#include "AlgoEngine.hpp"
#include "AvendellaMM.hpp"
#include "SimulationBroker.hpp"
#include "TradeDrivenExecutionModel.hpp"

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace trading {

class LiveTradingSession {
public:
    using ResultCallback = std::function<void(TradingResult, std::vector<AlgoOrderEvent>)>;

    LiveTradingSession(TradingEngine::PriceResolver resolver, double slippageBps);

    void registerAlgo(std::unique_ptr<IAlgo> algo);
    bool startAlgo(const std::string& algoId, const std::string& symbol, const AlgoParams& params);
    void stopAlgo(const std::string& algoId);
    std::vector<std::string> runningAlgos() const;

    void processTradeCommand(const TradeCommand& command);
    void onTradeTick(const std::string& symbol, double lastTradePrice, int64_t timestampMs);

    void setResultCallback(ResultCallback cb);
    SimulationBroker& broker() { return m_broker; }
    const SimulationBroker& broker() const { return m_broker; }

private:
    struct AlgoFactory {
        std::function<std::unique_ptr<IAlgo>()> make;
    };

    static AlgoOrderEvent buildAlgoEvent(const OrderUpdate& update, int64_t timestampMs);
    static void appendTradingResult(TradingResult& dst, const TradingResult& src);
    static TradingResult tradingResultFromExecutionEvents(const std::vector<ExecutionEvent>& events);
    static std::vector<AlgoOrderEvent> algoEventsFromExecutionEvents(const std::vector<ExecutionEvent>& events,
                                                                     int64_t timestampMs);

    mutable std::mutex m_mutex;
    SimulationBroker m_broker;
    std::unordered_map<std::string, AlgoFactory> m_factories;
    std::unordered_map<std::string, std::unique_ptr<AlgoBacktestAdapter>> m_runningAlgos;
    ResultCallback m_callback;
};

} // namespace trading
