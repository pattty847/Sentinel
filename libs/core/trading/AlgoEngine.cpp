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
    // Collect commands from all running algos
    std::vector<std::pair<IAlgo*, std::vector<TradeCommand>>> algoCommands;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& [id, algo] : m_algos) {
            if (!algo->isRunning()) {
                continue;
            }

            auto openOrders = std::vector<Order>{}; // TODO: filter by algoId if needed
            Position position{};

            auto cmds = algo->onTick(lastTradePrice, timestampMs, openOrders, position);
            if (!cmds.empty()) {
                algoCommands.emplace_back(algo.get(), std::move(cmds));
            }
        }
    }

    // Process commands through TradingEngine and collect results
    TradingResult combinedResult;
    std::vector<AlgoOrderEvent> algoEvents;

    for (auto& [algo, cmds] : algoCommands) {
        for (auto& cmd : cmds) {
            auto result = m_engine.onCommand(cmd);

            // Build AlgoOrderEvent for each order update from this algo
            for (auto& ou : result.orderUpdates) {
                AlgoOrderEvent ev;
                ev.algoId = cmd.algoId;
                ev.orderId = ou.orderId;
                ev.symbol = ou.symbol;
                ev.side = ou.side;
                ev.orderType = (ou.limitPrice > 0.0) ? OrderType::Limit : OrderType::Market;
                ev.price = (ou.limitPrice > 0.0) ? ou.limitPrice : ou.avgPrice;
                ev.qty = ou.qty;
                ev.status = ou.status;
                ev.timestampMs = timestampMs;
                algoEvents.push_back(ev);

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

    // Also tick limit order fills for this symbol
    auto tickResult = m_engine.onTick(symbol, lastTradePrice, timestampMs);
    for (auto& ou : tickResult.orderUpdates) {
        if (!ou.algoId.empty()) {
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
            algoEvents.push_back(ev);

            // Forward fill notification to algo
            std::lock_guard<std::mutex> lock(m_mutex);
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

    if (m_callback && (!combinedResult.orderUpdates.empty() ||
                        !combinedResult.positionUpdates.empty() ||
                        !combinedResult.pnlSnapshots.empty() ||
                        !algoEvents.empty())) {
        m_callback(std::move(combinedResult), std::move(algoEvents));
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
