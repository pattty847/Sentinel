#include "LiveTradingSession.hpp"

namespace trading {

LiveTradingSession::LiveTradingSession(TradingEngine::PriceResolver resolver, double slippageBps)
    : m_broker(std::move(resolver), std::make_unique<TradeDrivenExecutionModel>(), slippageBps) {}

void LiveTradingSession::registerAlgo(std::unique_ptr<IAlgo> algo) {
    const std::string algoId = algo->id();
    AlgoFactory factory;
    factory.make = [algoId]() -> std::unique_ptr<IAlgo> {
        if (algoId == "AvendellaMM") {
            return std::make_unique<AvendellaMM>();
        }
        return {};
    };

    std::lock_guard<std::mutex> lock(m_mutex);
    m_factories[algoId] = std::move(factory);
}

bool LiveTradingSession::startAlgo(const std::string& algoId, const std::string& symbol, const AlgoParams& params) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto fit = m_factories.find(algoId);
    if (fit == m_factories.end()) {
        return false;
    }
    auto adapter = std::make_unique<AlgoBacktestAdapter>(fit->second.make());
    if (!adapter) {
        return false;
    }
    adapter->start(symbol, params);
    m_runningAlgos[algoId] = std::move(adapter);
    return true;
}

void LiveTradingSession::stopAlgo(const std::string& algoId) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_runningAlgos.find(algoId);
    if (it != m_runningAlgos.end()) {
        it->second->stop();
        m_runningAlgos.erase(it);
    }
}

std::vector<std::string> LiveTradingSession::runningAlgos() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<std::string> out;
    for (const auto& [algoId, adapter] : m_runningAlgos) {
        if (adapter->isRunning()) {
            out.push_back(algoId);
        }
    }
    return out;
}

void LiveTradingSession::processTradeCommand(const TradeCommand& command) {
    const auto events = m_broker.submitIntents({fromTradeCommand(command)});
    auto tradingResult = tradingResultFromExecutionEvents(events);
    auto algoEvents = algoEventsFromExecutionEvents(events, command.timestamp);

    ResultCallback cb;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& event : events) {
            for (auto& [algoId, adapter] : m_runningAlgos) {
                adapter->onExecutionEvent(event);
            }
        }
        cb = m_callback;
    }

    if (cb && (!tradingResult.orderUpdates.empty() || !tradingResult.positionUpdates.empty() ||
               !tradingResult.pnlSnapshots.empty() || !tradingResult.riskOrderUpdates.empty() ||
               !algoEvents.empty())) {
        cb(std::move(tradingResult), std::move(algoEvents));
    }
}

void LiveTradingSession::onTradeTick(const std::string& symbol, double lastTradePrice, int64_t timestampMs) {
    MarketEvent event;
    event.type = MarketEventType::Trade;
    event.timestampMs = timestampMs;
    event.trade = TradeEvent{symbol, lastTradePrice, 0.0, timestampMs};

    TradingResult combined;
    auto marketEvents = m_broker.onMarketEvent(event);
    appendTradingResult(combined, tradingResultFromExecutionEvents(marketEvents));
    auto algoEvents = algoEventsFromExecutionEvents(marketEvents, timestampMs);

    std::vector<std::pair<std::string, std::vector<OrderIntent>>> intentsByAlgo;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& [algoId, adapter] : m_runningAlgos) {
            if (!adapter->isRunning()) {
                continue;
            }
            if (!adapter->symbol().empty() && adapter->symbol() != symbol) {
                continue;
            }
            const auto snapshot = m_broker.snapshotFor(symbol, timestampMs);
            auto intents = adapter->onMarketEvent(event, snapshot);
            if (!intents.empty()) {
                intentsByAlgo.emplace_back(algoId, std::move(intents));
            }
        }
    }

    for (const auto& [algoId, intents] : intentsByAlgo) {
        auto execEvents = m_broker.submitIntents(intents);
        appendTradingResult(combined, tradingResultFromExecutionEvents(execEvents));
        auto perAlgoEvents = algoEventsFromExecutionEvents(execEvents, timestampMs);
        algoEvents.insert(algoEvents.end(), perAlgoEvents.begin(), perAlgoEvents.end());

        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_runningAlgos.find(algoId);
        if (it != m_runningAlgos.end()) {
            for (const auto& execEvent : execEvents) {
                it->second->onExecutionEvent(execEvent);
            }
        }
    }

    ResultCallback cb;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        cb = m_callback;
    }
    if (cb && (!combined.orderUpdates.empty() || !combined.positionUpdates.empty() ||
               !combined.pnlSnapshots.empty() || !combined.riskOrderUpdates.empty() ||
               !algoEvents.empty())) {
        cb(std::move(combined), std::move(algoEvents));
    }
}

void LiveTradingSession::setResultCallback(ResultCallback cb) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_callback = std::move(cb);
}

AlgoOrderEvent LiveTradingSession::buildAlgoEvent(const OrderUpdate& update, int64_t timestampMs) {
    AlgoOrderEvent event;
    event.algoId = update.algoId;
    event.orderId = update.orderId;
    event.symbol = update.symbol;
    event.side = update.side;
    event.orderType = update.limitPrice > 0.0 ? OrderType::Limit : OrderType::Market;
    event.price = update.limitPrice > 0.0 ? update.limitPrice : update.avgPrice;
    event.qty = update.qty;
    event.status = update.status;
    event.timestampMs = timestampMs;
    return event;
}

void LiveTradingSession::appendTradingResult(TradingResult& dst, const TradingResult& src) {
    dst.orderUpdates.insert(dst.orderUpdates.end(), src.orderUpdates.begin(), src.orderUpdates.end());
    dst.positionUpdates.insert(dst.positionUpdates.end(), src.positionUpdates.begin(), src.positionUpdates.end());
    dst.pnlSnapshots.insert(dst.pnlSnapshots.end(), src.pnlSnapshots.begin(), src.pnlSnapshots.end());
    dst.riskOrderUpdates.insert(dst.riskOrderUpdates.end(), src.riskOrderUpdates.begin(), src.riskOrderUpdates.end());
}

TradingResult LiveTradingSession::tradingResultFromExecutionEvents(const std::vector<ExecutionEvent>& events) {
    TradingResult result;
    for (const auto& event : events) {
        if (event.orderUpdate.has_value()) {
            result.orderUpdates.push_back(*event.orderUpdate);
        }
        if (event.positionUpdate.has_value()) {
            result.positionUpdates.push_back(*event.positionUpdate);
        }
        if (event.pnlSnapshot.has_value()) {
            result.pnlSnapshots.push_back(*event.pnlSnapshot);
        }
        if (event.riskOrderUpdate.has_value()) {
            result.riskOrderUpdates.push_back(*event.riskOrderUpdate);
        }
    }
    return result;
}

std::vector<AlgoOrderEvent> LiveTradingSession::algoEventsFromExecutionEvents(const std::vector<ExecutionEvent>& events,
                                                                               int64_t timestampMs) {
    std::vector<AlgoOrderEvent> out;
    for (const auto& event : events) {
        if (!event.orderUpdate.has_value()) {
            continue;
        }
        const auto& update = *event.orderUpdate;
        if (update.algoId.empty()) {
            continue;
        }
        out.push_back(buildAlgoEvent(update, timestampMs));
    }
    return out;
}

} // namespace trading
