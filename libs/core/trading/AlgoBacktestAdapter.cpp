#include "AlgoBacktestAdapter.hpp"

namespace trading {

AlgoBacktestAdapter::AlgoBacktestAdapter(std::unique_ptr<IAlgo> algo)
    : m_algo(std::move(algo)) {}

AlgoBacktestAdapter::AlgoBacktestAdapter(std::unique_ptr<IAlgo> algo,
                                         std::string symbol,
                                         AlgoParams params)
    : m_algo(std::move(algo))
    , m_symbol(std::move(symbol))
    , m_params(params) {
    start(m_symbol, m_params);
}

const std::string& AlgoBacktestAdapter::id() const {
    return m_algo->id();
}

void AlgoBacktestAdapter::start(const std::string& symbol, const AlgoParams& params) {
    m_symbol = symbol;
    m_params = params;
    m_algo->start(m_symbol, m_params);
}

void AlgoBacktestAdapter::stop() {
    m_algo->stop();
}

bool AlgoBacktestAdapter::isRunning() const {
    return m_algo->isRunning();
}

const std::string& AlgoBacktestAdapter::symbol() const {
    return m_algo->symbol();
}

void AlgoBacktestAdapter::onExecutionEvent(const ExecutionEvent& event) {
    if (!event.orderUpdate.has_value()) {
        return;
    }
    const auto& update = *event.orderUpdate;
    if (update.algoId != m_algo->id()) {
        return;
    }
    m_algo->onOrderUpdate(update);
}

std::vector<OrderIntent> AlgoBacktestAdapter::onMarketEvent(const MarketEvent& event,
                                                            const BrokerSnapshot& snapshot) {
    if (event.type != MarketEventType::Trade || !event.trade.has_value()) {
        return {};
    }
    if (event.trade->symbol != m_symbol) {
        return {};
    }

    auto position = snapshot.position.value_or(Position{m_symbol, 0.0, 0.0, 0.0, 0.0});
    auto commands = m_algo->onTick(event.trade->price, event.trade->timestampMs, snapshot.openOrders, position);
    std::vector<OrderIntent> intents;
    intents.reserve(commands.size());
    for (const auto& command : commands) {
        intents.push_back(trading::fromTradeCommand(command));
    }
    return intents;
}

} // namespace trading
