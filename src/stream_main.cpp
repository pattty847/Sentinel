#include "coinbasestreamclient.h"
#include <iostream>
#include <chrono>
#include <thread>

int main() {
    CoinbaseStreamClient client;
    client.subscribe({"BTC-USD", "ETH-USD"});
    client.start();

    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(5)) {
        for (const auto& sym : {std::string("BTC-USD"), std::string("ETH-USD")}) {
            auto trades = client.getRecentTrades(sym);
            if (!trades.empty()) {
                const auto& last = trades.back();
                std::cout << sym << ": " << last.price << "@" << last.size
                          << " [" << last.side << "]" << std::endl;
            }
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    return 0;
}
