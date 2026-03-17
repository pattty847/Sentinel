#include "TradeDrivenExecutionModel.hpp"

namespace trading {

TradingResult TradeDrivenExecutionModel::onIntent(TradingEngine& engine, const OrderIntent& intent) {
    return engine.onCommand(toTradeCommand(intent));
}

TradingResult TradeDrivenExecutionModel::onMarketEvent(TradingEngine& engine, const MarketEvent& event) {
    if (event.type != MarketEventType::Trade || !event.trade.has_value()) {
        return {};
    }
    const auto& trade = *event.trade;
    return engine.onTick(trade.symbol, trade.price, trade.timestampMs);
}

} // namespace trading
