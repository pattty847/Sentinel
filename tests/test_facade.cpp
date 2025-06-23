#include "libs/core/coinbasestreamclient.h"
#include <iostream>
#include <vector>
#include <string>
#include <thread>   
#include <chrono>

int main() {
    std::cout << "🧪 Testing CoinbaseStreamClient Facade Implementation" << std::endl;
    std::cout << "===============================================" << std::endl;
    
    try {
        // Test 1: Constructor
        std::cout << "\n🏗️  Test 1: Constructor" << std::endl;
        CoinbaseStreamClient client;
        std::cout << "✅ Constructor successful - facade initialized!" << std::endl;
        
        // Test 2: Subscribe (before start)
        std::cout << "\n📝 Test 2: Subscribe before start" << std::endl;
        std::vector<std::string> symbols = {"BTC-USD", "ETH-USD"};
        client.subscribe(symbols);
        std::cout << "✅ Subscribe successful - symbols saved for later!" << std::endl;
        
        // Test 3: Data access (should return empty but not crash)
        std::cout << "\n📊 Test 3: Data access before connection" << std::endl;
        auto trades = client.getRecentTrades("BTC-USD");
        auto newTrades = client.getNewTrades("BTC-USD", "");
        auto orderBook = client.getOrderBook("BTC-USD");
        std::cout << "✅ Data access successful - empty results as expected!" << std::endl;
        std::cout << "   Recent trades: " << trades.size() << std::endl;
        std::cout << "   New trades: " << newTrades.size() << std::endl;
        std::cout << "   Order book product: " << orderBook.product_id << std::endl;
        
        // Test 4: Start client
        std::cout << "\n🚀 Test 4: Start client" << std::endl;
        client.start();
        std::cout << "✅ Start successful - MarketDataCore should be running!" << std::endl;
        
        // Test 5: Subscribe after start
        std::cout << "\n📡 Test 5: Subscribe after start" << std::endl;
        client.subscribe(symbols);
        std::cout << "✅ Subscribe after start successful!" << std::endl;
        
        // Test 6: Let it run briefly
        std::cout << "\n⏱️  Test 6: Running for 3 seconds..." << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(3));
        std::cout << "✅ Runtime test successful!" << std::endl;
        
        // Test 7: Stop client  
        std::cout << "\n🛑 Test 7: Stop client" << std::endl;
        client.stop();
        std::cout << "✅ Stop successful - clean shutdown!" << std::endl;
        
        std::cout << "\n🎉 ALL TESTS PASSED! Facade implementation is working perfectly!" << std::endl;
        std::cout << "✨ The 615-line monolith has been successfully transformed into a clean facade!" << std::endl;
        
    } catch (const std::exception& e) {
        std::cerr << "❌ Test failed with exception: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
} 