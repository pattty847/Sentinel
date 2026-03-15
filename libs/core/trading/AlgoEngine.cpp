#include "AlgoEngine.hpp"

namespace trading {

AlgoEngine::AlgoEngine(TradingEngine& engine)
    : m_engine(engine) {}

void AlgoEngine::registerAlgo(std::unique_ptr<IAlgo> algo) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_algos[algo->id()] = std::move(algo);
}

bool AlgoEngine::startAlgo(const std::string& algoId, const std::string& symbol, const AlgoParams& params) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_algos.find(algoId);
    if (it == m_algos.end()) {
        return false;
    }
    it->second->start(symbol, params);
    return true;
}

void AlgoEngine::stopAlgo(const std::string& algoId) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_algos.find(algoId);
    if (it != m_algos.end()) {
        it->second->stop();
    }
}

void AlgoEngine::onTick(const std::string& symbol, double lastTradePrice, int64_t timestampMs) {
    TradingResult combinedResult;
    std::vector<AlgoOrderEvent> algoEvents;
    std::vector<std::pair<IAlgo*, std::vector<TradeCommand>>> algoCommands;

    const auto buildAlgoEvent = [timestampMs](const OrderUpdate& ou) {
        AlgoOrderEvent ev;
        ev.algoId = ou.algoId;
        ev.orderId = ou.orderId;
        ev.symbol = ou.symbol;
        ev.side = ou.side;
        ev.orderType = (ou.limitPrice > 0.0) ? OrderType::Limit : OrderType::Market;
        ev.price = (ou.limitPrice > 0.0) ? ou.limitPrice : ou.avgPrice;
        ev.qty = ou.qty;
        ev.status = ou.status;
        ev.timestampMs = timestampMs;
        return ev;
    };

    // Fill resting orders for this market tick before algos decide to cancel/requote.
    auto tickResult = m_engine.onTick(symbol, lastTradePrice, timestampMs);
    std::vector<OrderUpdate> algoOwnedTickUpdates;
    for (auto& ou : tickResult.orderUpdates) {
        if (!ou.algoId.empty()) {
            algoEvents.push_back(buildAlgoEvent(ou));
            algoOwnedTickUpdates.push_back(ou);
        }
    }
    if (!algoOwnedTickUpdates.empty()) {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& ou : algoOwnedTickUpdates) {
            auto it = m_algos.find(ou.algoId);
            if (it != m_algos.end()) {
                it->second->onOrderUpdate(ou);
            }
        }
    }
    combinedResult.orderUpdates.insert(combinedResult.orderUpdates.end(),
                                       tickResult.orderUpdates.begin(), tickResult.orderUpdates.end());
    combinedResult.positionUpdates.insert(combinedResult.positionUpdates.end(),
                                          tickResult.positionUpdates.begin(), tickResult.positionUpdates.end());
    combinedResult.pnlSnapshots.insert(combinedResult.pnlSnapshots.end(),
                                       tickResult.pnlSnapshots.begin(), tickResult.pnlSnapshots.end());

    // Collect commands from all running algos after fills/position changes have been applied.
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& [id, algo] : m_algos) {
            if (!algo->isRunning()) {
                continue;
            }
            if (!algo->symbol().empty() && algo->symbol() != symbol) {
                continue;
            }

            auto openOrders = m_engine.getOpenOrders(symbol, algo->id());
            Position position = m_engine.getPosition(symbol).value_or(Position{symbol, 0.0, 0.0, 0.0, 0.0});

            auto cmds = algo->onTick(lastTradePrice, timestampMs, openOrders, position);
            if (!cmds.empty()) {
                algoCommands.emplace_back(algo.get(), std::move(cmds));
            }
        }
    }

    for (auto& [algo, cmds] : algoCommands) {
        for (auto& cmd : cmds) {
            auto result = m_engine.onCommand(cmd);

            // Build AlgoOrderEvent for each order update from this algo
            for (auto& ou : result.orderUpdates) {
                algoEvents.push_back(buildAlgoEvent(ou));

                // Forward to algo for state tracking
                algo->onOrderUpdate(ou);
            }

            combinedResult.orderUpdates.insert(combinedResult.orderUpdates.end(),
                                                result.orderUpdates.begin(), result.orderUpdates.end());
            combinedResult.positionUpdates.insert(combinedResult.positionUpdates.end(),
                                                   result.positionUpdates.begin(), result.positionUpdates.end());
            combinedResult.pnlSnapshots.insert(combinedResult.pnlSnapshots.end(),
                                                result.pnlSnapshots.begin(), result.pnlSnapshots.end());
        }
    }

    ResultCallback cb;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        cb = m_callback;
    }

    if (cb && (!combinedResult.orderUpdates.empty() ||
               !combinedResult.positionUpdates.empty() ||
               !combinedResult.pnlSnapshots.empty() ||
               !algoEvents.empty())) {
        cb(std::move(combinedResult), std::move(algoEvents));
    }
}

void AlgoEngine::onOrderUpdate(const OrderUpdate& update) {
    if (update.algoId.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_algos.find(update.algoId);
    if (it != m_algos.end()) {
        it->second->onOrderUpdate(update);
    }
}

void AlgoEngine::setResultCallback(ResultCallback cb) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_callback = std::move(cb);
}

std::vector<std::string> AlgoEngine::runningAlgos() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<std::string> out;
    for (auto& [id, algo] : m_algos) {
        if (algo->isRunning()) {
            out.push_back(id);
        }
    }
    return out;
}

} // namespace trading
