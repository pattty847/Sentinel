#pragma once

#include "BacktestTypes.hpp"
#include "IBacktestStrategy.hpp"
#include "MarketEventSource.hpp"
#include "SimulationBroker.hpp"

namespace trading {

class ReplayEngine {
public:
    BacktestResult run(IMarketEventSource& eventSource,
                       IBacktestStrategy& strategy,
                       SimulationBroker& broker,
                       const BacktestConfig& config) const;
};

} // namespace trading
