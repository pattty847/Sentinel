#pragma once

#include "BacktestTypes.hpp"

#include <string>
#include <vector>

namespace trading {

class IBacktestStrategy {
public:
    virtual ~IBacktestStrategy() = default;

    virtual const std::string& id() const = 0;
    virtual void onExecutionEvent(const ExecutionEvent& event) = 0;
    virtual std::vector<OrderIntent> onMarketEvent(const MarketEvent& event,
                                                   const BrokerSnapshot& snapshot) = 0;
};

} // namespace trading
