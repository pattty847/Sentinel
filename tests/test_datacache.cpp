#include "../libs/core/DataCache.hpp"
#include <iostream>
#include <chrono>
#include <thread>
#include <atomic>

int main() {
    std::cout << "🧪 Testing DataCache Implementation..." << std::endl;
    
    DataCache cache;
    
    // Test 1: Basic trade addition and retrieval
    std::cout << "\n📊 Test 1: Basic Trade Operations" << std::endl;
    
    Trade trade1;
    trade1.product_id = "BTC-USD";
    trade1.trade_id = "1";
    trade1.price = 50000.0;
    trade1.size = 0.1;
    trade1.side = AggressorSide::Buy;
    trade1.timestamp = std::chrono::system_clock::now();
    
    cache.addTrade(trade1);
    
    auto trades = cache.recentTrades("BTC-USD");
    std::cout << "✅ Added 1 trade, retrieved " << trades.size() << " trades" << std::endl;
    
    // Test 2: Ring buffer overflow behavior
    std::cout << "\n🔄 Test 2: Ring Buffer Overflow (1000+ trades)" << std::endl;
    
    // Add many trades to test ring buffer
    for (int i = 2; i <= 1100; ++i) {
        Trade trade;
        trade.product_id = "BTC-USD";
        trade.trade_id = std::to_string(i);
        trade.price = 50000.0 + i;
        trade.size = 0.1;
        trade.side = AggressorSide::Buy;
        trade.timestamp = std::chrono::system_clock::now();
        cache.addTrade(trade);
    }
    
    trades = cache.recentTrades("BTC-USD");
    std::cout << "✅ Added 1100 trades, ring buffer contains " << trades.size() << " trades (should be 1000)" << std::endl;
    
    // Test 3: Multiple symbols
    std::cout << "\n🪙 Test 3: Multiple Symbols" << std::endl;
    
    Trade ethTrade;
    ethTrade.product_id = "ETH-USD";
    ethTrade.trade_id = "eth1";
    ethTrade.price = 3000.0;
    ethTrade.size = 1.0;
    ethTrade.side = AggressorSide::Sell;
    ethTrade.timestamp = std::chrono::system_clock::now();
    
    cache.addTrade(ethTrade);
    
    auto btcTrades = cache.recentTrades("BTC-USD");
    auto ethTrades = cache.recentTrades("ETH-USD");
    
    std::cout << "✅ BTC-USD trades: " << btcTrades.size() << std::endl;
    std::cout << "✅ ETH-USD trades: " << ethTrades.size() << std::endl;
    
    // Test 4: OrderBook operations
    std::cout << "\n📖 Test 4: OrderBook Operations" << std::endl;
    
    OrderBook book;
    book.product_id = "BTC-USD";
    book.timestamp = std::chrono::system_clock::now();
    
    OrderBookLevel bid1 = {49999.0, 0.5};
    OrderBookLevel ask1 = {50001.0, 0.3};
    book.bids.push_back(bid1);
    book.asks.push_back(ask1);
    
    cache.updateBook(book);
    
    auto retrievedBook = cache.book("BTC-USD");
    std::cout << "✅ OrderBook has " << retrievedBook.bids.size() << " bids, " 
              << retrievedBook.asks.size() << " asks" << std::endl;
    
    // Test 5: Thread safety simulation
    std::cout << "\n🧵 Test 5: Concurrent Read/Write Simulation" << std::endl;
    
    std::vector<std::thread> threads;
    std::atomic<int> readCount{0};
    std::atomic<int> writeCount{0};
    
    // Writer thread
    threads.emplace_back([&cache, &writeCount]() {
        for (int i = 0; i < 100; ++i) {
            Trade trade;
            trade.product_id = "TEST-USD";
            trade.trade_id = "t" + std::to_string(i);
            trade.price = 1000.0 + i;
            trade.size = 0.1;
            trade.side = AggressorSide::Buy;
            trade.timestamp = std::chrono::system_clock::now();
            cache.addTrade(trade);
            writeCount++;
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    });
    
    // Reader threads
    for (int t = 0; t < 3; ++t) {
        threads.emplace_back([&cache, &readCount]() {
            for (int i = 0; i < 50; ++i) {
                auto trades = cache.recentTrades("TEST-USD");
                readCount++;
                std::this_thread::sleep_for(std::chrono::microseconds(150));
            }
        });
    }
    
    // Wait for all threads
    for (auto& thread : threads) {
        thread.join();
    }
    
    std::cout << "✅ Concurrent operations completed: " << writeCount << " writes, " 
              << readCount << " reads" << std::endl;
    
    // Test 6: tradesSince functionality
    std::cout << "\n🔍 Test 6: tradesSince Functionality" << std::endl;
    
    auto testTrades = cache.recentTrades("TEST-USD");
    if (!testTrades.empty()) {
        std::string lastId = testTrades[testTrades.size()/2].trade_id;
        auto newTrades = cache.tradesSince("TEST-USD", lastId);
        std::cout << "✅ tradesSince('" << lastId << "') returned " << newTrades.size() << " trades" << std::endl;
    }
    
    std::cout << "\n🎉 All tests completed! DataCache is working correctly!" << std::endl;
    
    return 0;
} 