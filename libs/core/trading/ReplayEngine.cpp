#include "ReplayEngine.hpp"

namespace trading {

BacktestResult ReplayEngine::run(IMarketEventSource& eventSource,
                                 IBacktestStrategy& strategy,
                                 SimulationBroker& broker,
                                 const BacktestConfig& config) const {
    while (auto event = eventSource.next()) {
        auto executionEvents = broker.onMarketEvent(*event);
        for (const auto& executionEvent : executionEvents) {
            strategy.onExecutionEvent(executionEvent);
        }

        std::string symbol = config.symbol;
        if (event->trade.has_value() && !event->trade->symbol.empty()) {
            symbol = event->trade->symbol;
        }
        auto snapshot = broker.snapshotFor(symbol, event->timestampMs);
        auto intents = strategy.onMarketEvent(*event, snapshot);
        auto intentEvents = broker.submitIntents(intents);
        for (const auto& executionEvent : intentEvents) {
            strategy.onExecutionEvent(executionEvent);
        }
    }

    return broker.buildResult(config);
}

} // namespace trading
