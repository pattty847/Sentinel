#pragma once
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "TradeData.h"

namespace MessageParser {

inline std::vector<Trade> parseMarketTrades(const std::string& jsonStr) {
    std::vector<Trade> trades;
    auto message = nlohmann::json::parse(jsonStr);
    if (message.contains("events")) {
        for (const auto& event : message["events"]) {
            if (event.contains("trades")) {
                for (const auto& t : event["trades"]) {
                    Trade trade;
                    trade.product_id = t.value("product_id", "");
                    trade.trade_id = t.value("trade_id", "");
                    trade.price = std::stod(t.value("price", "0"));
                    trade.size = std::stod(t.value("size", "0"));
                    std::string side = t.value("side", "");
                    if (side == "BUY") trade.side = AggressorSide::Buy;
                    else if (side == "SELL") trade.side = AggressorSide::Sell;
                    else trade.side = AggressorSide::Unknown;
                    trade.timestamp = std::chrono::system_clock::now();
                    trades.push_back(trade);
                }
            }
        }
    }
    return trades;
}

inline OrderBook parseL2Update(const std::string& jsonStr) {
    auto message = nlohmann::json::parse(jsonStr);
    OrderBook book;
    if (message.contains("product_id"))
        book.product_id = message["product_id"].get<std::string>();
    book.timestamp = std::chrono::system_clock::now();
    if (message.contains("updates")) {
        for (const auto& update : message["updates"]) {
            std::string side = update.value("side", "");
            double price = std::stod(update.value("price_level", "0"));
            double quantity = std::stod(update.value("new_quantity", "0"));
            OrderBookLevel level{price, quantity};
            if (side == "bid") book.bids.push_back(level);
            else if (side == "offer" || side == "ask") book.asks.push_back(level);
        }
    }
    return book;
}

} // namespace MessageParser
