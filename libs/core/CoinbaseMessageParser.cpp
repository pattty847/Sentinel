#include "CoinbaseMessageParser.hpp"
#include <chrono>

CoinbaseMessageParser::CoinbaseMessageParser(DataCache& cache)
    : m_cache(cache) {}

void CoinbaseMessageParser::parseAndDispatch(const std::string& payload) {
    try {
        auto message = nlohmann::json::parse(payload);
        dispatch(message);
    } catch (const std::exception& e) {
        sLog_Error("JSON parse error:" << e.what());
    }
}

void CoinbaseMessageParser::dispatch(const nlohmann::json& message) {
    if (!message.is_object()) return;

    std::string channel = message.value("channel", "");
    std::string type = message.value("type", "");

    if (channel == "market_trades") {
        if (message.contains("events")) {
            for (const auto& event : message["events"]) {
                if (event.contains("trades")) {
                    for (const auto& trade_data : event["trades"]) {
                        Trade trade;
                        trade.product_id = trade_data.value("product_id", "");
                        trade.trade_id = trade_data.value("trade_id", "");
                        trade.price = std::stod(trade_data.value("price", "0"));
                        trade.size = std::stod(trade_data.value("size", "0"));
                        std::string side = trade_data.value("side", "");
                        if (side == "BUY") trade.side = AggressorSide::Buy;
                        else if (side == "SELL") trade.side = AggressorSide::Sell;
                        else trade.side = AggressorSide::Unknown;
                        trade.timestamp = std::chrono::system_clock::now();
                        m_cache.addTrade(trade);
                    }
                }
            }
        }
    } else if (channel == "l2_data") {
        if (message.contains("events")) {
            for (const auto& event : message["events"]) {
                std::string eventType = event.value("type", "");
                std::string product_id = event.value("product_id", "");
                if (eventType == "snapshot" && event.contains("updates") && !product_id.empty()) {
                    OrderBook snapshot;
                    snapshot.product_id = product_id;
                    snapshot.timestamp = std::chrono::system_clock::now();
                    for (const auto& update : event["updates"]) {
                        if (!update.contains("side") || !update.contains("price_level") || !update.contains("new_quantity"))
                            continue;
                        std::string side = update["side"];
                        double price = std::stod(update["price_level"].get<std::string>());
                        double quantity = std::stod(update["new_quantity"].get<std::string>());
                        if (quantity > 0.0) {
                            OrderBookLevel level{price, quantity};
                            if (side == "bid") snapshot.bids.push_back(level);
                            else if (side == "offer") snapshot.asks.push_back(level);
                        }
                    }
                    m_cache.initializeLiveOrderBook(product_id, snapshot);
                } else if (eventType == "update" && event.contains("updates") && !product_id.empty()) {
                    for (const auto& update : event["updates"]) {
                        if (!update.contains("side") || !update.contains("price_level") || !update.contains("new_quantity"))
                            continue;
                        std::string side = update["side"];
                        double price = std::stod(update["price_level"].get<std::string>());
                        double quantity = std::stod(update["new_quantity"].get<std::string>());
                        m_cache.updateLiveOrderBook(product_id, side, price, quantity);
                    }
                }
            }
        }
    } else if (channel == "subscriptions") {
        sLog_Subscription("Subscription confirmed:" << QString::fromStdString(message.dump()));
    } else if (type == "error") {
        sLog_Error("Coinbase error:" << QString::fromStdString(message.value("message", "unknown")));
    }
}

