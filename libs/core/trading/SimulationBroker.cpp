#include "SimulationBroker.hpp"

#include <algorithm>

namespace trading {

SimulationBroker::SimulationBroker(TradingEngine::PriceResolver resolver,
                                   std::unique_ptr<IExecutionModel> executionModel,
                                   double slippageBps)
    : m_engine(
        resolver ? std::move(resolver) : [this](const std::string& symbol) { return resolvePrice(symbol); },
        slippageBps)
    , m_executionModel(std::move(executionModel)) {}

std::vector<ExecutionEvent> SimulationBroker::onMarketEvent(const MarketEvent& event) {
    m_result.marketEvents.push_back(event);
    m_result.summary.eventCount++;

    if (event.type == MarketEventType::Trade && event.trade.has_value()) {
        m_lastTradePriceBySymbol[event.trade->symbol] = event.trade->price;
    }

    auto result = m_executionModel->onMarketEvent(m_engine, event);
    return recordTradingResult(result, event.timestampMs);
}

std::vector<ExecutionEvent> SimulationBroker::submitIntents(const std::vector<OrderIntent>& intents) {
    std::vector<ExecutionEvent> out;
    for (const auto& intent : intents) {
        auto result = m_executionModel->onIntent(m_engine, intent);
        auto events = recordTradingResult(result, intent.timestampMs);
        out.insert(out.end(), events.begin(), events.end());
    }
    return out;
}

BrokerSnapshot SimulationBroker::snapshotFor(const std::string& symbol, int64_t timestampMs) const {
    BrokerSnapshot snapshot;
    snapshot.symbol = symbol;
    snapshot.timestampMs = timestampMs;
    snapshot.openOrders = m_engine.getOpenOrders(symbol);
    snapshot.position = m_engine.getPosition(symbol);
    auto it = m_lastTradePriceBySymbol.find(symbol);
    if (it != m_lastTradePriceBySymbol.end()) {
        snapshot.lastTradePrice = it->second;
    }
    return snapshot;
}

BacktestResult SimulationBroker::buildResult(const BacktestConfig& config) const {
    BacktestResult result = m_result;
    result.summary.symbol = config.symbol;
    result.summary.strategyId = config.strategyId;
    if (!result.marketEvents.empty()) {
        result.summary.startedAtMs = result.marketEvents.front().timestampMs;
        result.summary.finishedAtMs = result.marketEvents.back().timestampMs;
    }
    return result;
}

std::vector<ExecutionEvent> SimulationBroker::recordTradingResult(const TradingResult& result, int64_t timestampMs) {
    std::vector<ExecutionEvent> events;
    events.reserve(result.orderUpdates.size() + result.positionUpdates.size() + result.pnlSnapshots.size());

    for (const auto& update : result.orderUpdates) {
        ExecutionEvent event;
        event.type = classifyOrderEvent(update);
        event.timestampMs = timestampMs;
        event.symbol = update.symbol;
        event.algoId = update.algoId;
        event.orderUpdate = update;
        m_result.executionEvents.push_back(event);
        m_result.orderLifecycleLog.push_back(update);
        if (update.status == OrderStatus::Filled || update.status == OrderStatus::Partial) {
            m_result.fillLog.push_back(update);
            if (update.status == OrderStatus::Filled) {
                m_result.summary.fillCount++;
            }
        }
        m_result.summary.orderEventCount++;
        events.push_back(event);
    }

    for (const auto& update : result.positionUpdates) {
        ExecutionEvent event;
        event.type = ExecutionEventType::PositionUpdated;
        event.timestampMs = timestampMs;
        event.symbol = update.symbol;
        event.positionUpdate = update;
        m_result.executionEvents.push_back(event);
        m_result.positionTimeline.push_back(update);
        events.push_back(event);
    }

    for (const auto& snapshot : result.pnlSnapshots) {
        ExecutionEvent event;
        event.type = ExecutionEventType::PnlSnapshot;
        event.timestampMs = snapshot.timestampMs;
        event.symbol = snapshot.symbol;
        event.algoId = snapshot.algoId;
        event.pnlSnapshot = snapshot;
        m_result.executionEvents.push_back(event);
        m_result.pnlCurve.push_back(snapshot);
        updateSummaryFromPnl(snapshot);
        events.push_back(event);
    }

    return events;
}

ExecutionEventType SimulationBroker::classifyOrderEvent(const OrderUpdate& update) {
    switch (update.status) {
    case OrderStatus::New: return ExecutionEventType::OrderAccepted;
    case OrderStatus::Open: return ExecutionEventType::OrderOpened;
    case OrderStatus::Partial: return ExecutionEventType::OrderPartialFill;
    case OrderStatus::Filled: return ExecutionEventType::OrderFilled;
    case OrderStatus::Canceled: return ExecutionEventType::OrderCanceled;
    case OrderStatus::Rejected: return ExecutionEventType::OrderRejected;
    }
    return ExecutionEventType::OrderAccepted;
}

double SimulationBroker::resolvePrice(const std::string& symbol) const {
    auto it = m_lastTradePriceBySymbol.find(symbol);
    if (it == m_lastTradePriceBySymbol.end()) {
        return 0.0;
    }
    return it->second;
}

void SimulationBroker::updateSummaryFromPnl(const PnlSnapshot& snapshot) {
    m_result.summary.realizedPnl = snapshot.realizedPnl;
    m_result.summary.unrealizedPnl = snapshot.unrealizedPnl;
    m_result.summary.totalPnl = snapshot.totalPnl;
    if (m_result.pnlCurve.empty()) {
        m_result.summary.maxDrawdown = 0.0;
        return;
    }

    double peak = m_result.pnlCurve.front().totalPnl;
    double maxDrawdown = 0.0;
    for (const auto& point : m_result.pnlCurve) {
        peak = std::max(peak, point.totalPnl);
        maxDrawdown = std::max(maxDrawdown, peak - point.totalPnl);
    }
    m_result.summary.maxDrawdown = maxDrawdown;
}

} // namespace trading
