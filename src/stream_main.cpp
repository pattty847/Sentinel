#include "coinbasestreamclient.h"
#include "tradedata.h"
#include <iostream>
#include <chrono>
#include <thread>
#include <unordered_map>

int main() {
    std::cout << "[Coinbase Stream Test Starting...]" << std::endl;
    CoinbaseStreamClient client;
    client.subscribe({"BTC-USD", "ETH-USD"});
    client.start();

    // Track last seen trade_id for each symbol to avoid reprinting
    std::unordered_map<std::string, std::string> lastTradeIds;

    auto start = std::chrono::steady_clock::now();
    
    // For full-speed streaming, use getNewTrades instead of sleep
    bool useFullHose = true;  // Set to false to use the slower 200ms version
    
    if (useFullHose) {
        std::cout << "[Running at full speed - no duplicates!]" << std::endl;
        while (std::chrono::steady_clock::now() - start < std::chrono::seconds(60)) {
            for (const auto& sym : {std::string("BTC-USD"), std::string("ETH-USD")}) {
                auto newTrades = client.getNewTrades(sym, lastTradeIds[sym]);
                
                for (const auto& trade : newTrades) {
                    lastTradeIds[sym] = trade.trade_id;
                    
                    std::string side_str;
                    switch (trade.side) {
                        case Side::Buy: side_str = "buy"; break;
                        case Side::Sell: side_str = "sell"; break;
                        default: side_str = "unknown"; break;
                    }
                    
                    std::cout << sym << ": " << trade.price << "@" << trade.size
                              << " [" << side_str << "] ID:" << trade.trade_id << std::endl;
                }
            }
            // Optional: tiny sleep to prevent excessive CPU usage
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    } else {
        std::cout << "[Running with 200ms polling]" << std::endl;
        while (std::chrono::steady_clock::now() - start < std::chrono::seconds(60)) {
            for (const auto& sym : {std::string("BTC-USD"), std::string("ETH-USD")}) {
                auto trades = client.getRecentTrades(sym);
                if (!trades.empty()) {
                    const auto& last = trades.back();
                    
                    // Only print if this is a new trade
                    if (lastTradeIds[sym] != last.trade_id) {
                        lastTradeIds[sym] = last.trade_id;
                        
                        std::string side_str;
                        switch (last.side) {
                            case Side::Buy: side_str = "buy"; break;
                            case Side::Sell: side_str = "sell"; break;
                            default: side_str = "unknown"; break;
                        }
                        
                        std::cout << sym << ": " << last.price << "@" << last.size
                                  << " [" << side_str << "] ID:" << last.trade_id << std::endl;
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }

    return 0;
}
