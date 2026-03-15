#pragma once

#include "IAlgo.hpp"
#include "IBacktestStrategy.hpp"

#include <memory>

namespace trading {

class AlgoBacktestAdapter final : public IBacktestStrategy {
public:
    explicit AlgoBacktestAdapter(std::unique_ptr<IAlgo> algo);
    AlgoBacktestAdapter(std::unique_ptr<IAlgo> algo,
                        std::string symbol,
                        AlgoParams params);

    const std::string& id() const override;
    void start(const std::string& symbol, const AlgoParams& params);
    void stop();
    bool isRunning() const;
    const std::string& symbol() const;
    void onExecutionEvent(const ExecutionEvent& event) override;
    std::vector<OrderIntent> onMarketEvent(const MarketEvent& event,
                                           const BrokerSnapshot& snapshot) override;

    std::unique_ptr<IAlgo> m_algo;
    std::string m_symbol;
    AlgoParams m_params;
};

} // namespace trading
