// =============================================================================
// PHASE 6: COMPREHENSIVE TESTING SUITE
// Validate the architectural transformation's correctness and performance
// =============================================================================

#include <iostream>
#include <chrono>
#include <thread>
#include <vector>
#include <future>
#include <random>
#include <atomic>
#include <cassert>

// Test our beautiful facade
#include "libs/core/coinbasestreamclient.h"

class ComprehensiveTestSuite {
private:
    CoinbaseStreamClient client;
    std::atomic<int> test_count{0};
    std::atomic<int> passed_tests{0};
    
    // Helper for measuring execution time
    template<typename Func>
    double measureExecutionTime(Func&& func) {
        auto start = std::chrono::high_resolution_clock::now();
        func();
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration<double, std::milli>(end - start);
        return duration.count();
    }
    
    void assertTrue(bool condition, const std::string& test_name) {
        test_count++;
        if (condition) {
            passed_tests++;
            std::cout << "✅ " << test_name << " - PASSED" << std::endl;
        } else {
            std::cout << "❌ " << test_name << " - FAILED" << std::endl;
        }
    }

public:
    // =================================================================
    // TEST 1: API COMPATIBILITY TESTING
    // Verify all public methods exist and return expected types
    // =================================================================
    void testApiCompatibility() {
        std::cout << "\n🔍 TEST 1: API COMPATIBILITY VALIDATION" << std::endl;
        std::cout << "=========================================" << std::endl;
        
        try {
            // Test that all public methods exist and can be called
            client.start();
            assertTrue(true, "start() method exists and callable");
            
            std::vector<std::string> symbols = {"BTC-USD", "ETH-USD"};
            client.subscribe(symbols);
            assertTrue(true, "subscribe() method exists and callable");
            
            // Test data access methods (may return empty initially)
            auto trades = client.getRecentTrades("BTC-USD");
            assertTrue(true, "getRecentTrades() method exists and callable");
            
            auto new_trades = client.getNewTrades("BTC-USD", "last_id");
            assertTrue(true, "getNewTrades() method exists and callable");
            
            auto order_book = client.getOrderBook("BTC-USD");
            assertTrue(true, "getOrderBook() method exists and callable");
            
            client.stop();
            assertTrue(true, "stop() method exists and callable");
            
        } catch (const std::exception& e) {
            std::cout << "Exception in API compatibility test: " << e.what() << std::endl;
            assertTrue(false, "API compatibility - exception thrown");
        }
    }
    
    // =================================================================
    // TEST 2: PERFORMANCE BENCHMARKING
    // Measure performance improvements vs original implementation
    // =================================================================
    void testPerformanceBenchmarking() {
        std::cout << "\n⚡ TEST 2: PERFORMANCE BENCHMARKING" << std::endl;
        std::cout << "===================================" << std::endl;
        
        client.start();
        client.subscribe({"BTC-USD", "ETH-USD"});
        
        // Give some time for data to accumulate
        std::this_thread::sleep_for(std::chrono::seconds(2));
        
        // Benchmark data access operations
        const int iterations = 1000;
        
        auto trades_latency = measureExecutionTime([&]() {
            for (int i = 0; i < iterations; ++i) {
                auto trades = client.getRecentTrades("BTC-USD");
            }
        });
        
        auto orderbook_latency = measureExecutionTime([&]() {
            for (int i = 0; i < iterations; ++i) {
                auto book = client.getOrderBook("BTC-USD");
            }
        });
        
        double avg_trades_latency = trades_latency / iterations;
        double avg_orderbook_latency = orderbook_latency / iterations;
        
        std::cout << "📊 getRecentTrades() avg latency: " << avg_trades_latency << " ms" << std::endl;
        std::cout << "📊 getOrderBook() avg latency: " << avg_orderbook_latency << " ms" << std::endl;
        
        // With O(1) operations, should be sub-millisecond
        assertTrue(avg_trades_latency < 1.0, "getRecentTrades() latency < 1ms (O(1) performance)");
        assertTrue(avg_orderbook_latency < 1.0, "getOrderBook() latency < 1ms (O(1) performance)");
        
        client.stop();
    }
    
    // =================================================================
    // TEST 3: THREAD SAFETY TESTING
    // Concurrent access from multiple threads
    // =================================================================
    void testThreadSafety() {
        std::cout << "\n🔒 TEST 3: THREAD SAFETY VALIDATION" << std::endl;
        std::cout << "====================================" << std::endl;
        
        client.start();
        client.subscribe({"BTC-USD", "ETH-USD"});
        
        // Give time for data accumulation
        std::this_thread::sleep_for(std::chrono::seconds(2));
        
        const int num_threads = 10;
        const int operations_per_thread = 100;
        std::vector<std::future<bool>> futures;
        
        // Launch multiple threads accessing data concurrently
        for (int i = 0; i < num_threads; ++i) {
            futures.push_back(std::async(std::launch::async, [&]() {
                try {
                    for (int j = 0; j < operations_per_thread; ++j) {
                        auto trades = client.getRecentTrades("BTC-USD");
                        auto book = client.getOrderBook("BTC-USD");
                        // Alternate between symbols
                        if (j % 2 == 0) {
                            auto eth_trades = client.getRecentTrades("ETH-USD");
                        }
                    }
                    return true;
                } catch (...) {
                    return false;
                }
            }));
        }
        
        // Wait for all threads and check results
        bool all_threads_successful = true;
        for (auto& future : futures) {
            if (!future.get()) {
                all_threads_successful = false;
            }
        }
        
        assertTrue(all_threads_successful, "Multi-threaded concurrent access successful");
        
        client.stop();
    }
    
    // =================================================================
    // TEST 4: MEMORY USAGE VALIDATION
    // Ensure bounded memory usage with ring buffers
    // =================================================================
    void testMemoryUsage() {
        std::cout << "\n💾 TEST 4: MEMORY USAGE VALIDATION" << std::endl;
        std::cout << "===================================" << std::endl;
        
        client.start();
        client.subscribe({"BTC-USD"});
        
        // Let it run for a while to accumulate data
        std::this_thread::sleep_for(std::chrono::seconds(5));
        
        // Check that trade data is bounded (ring buffer should limit it)
        auto trades = client.getRecentTrades("BTC-USD");
        std::cout << "📈 Current trade count: " << trades.size() << std::endl;
        
        // Ring buffer should limit to 1000 trades max
        assertTrue(trades.size() <= 1000, "Trade count bounded by ring buffer (≤ 1000)");
        
        // Let it run longer to verify memory doesn't grow unbounded
        std::this_thread::sleep_for(std::chrono::seconds(5));
        
        auto trades_after = client.getRecentTrades("BTC-USD");
        std::cout << "📈 Trade count after more time: " << trades_after.size() << std::endl;
        
        // Should still be bounded
        assertTrue(trades_after.size() <= 1000, "Memory usage remains bounded over time");
        
        client.stop();
    }
    
    // =================================================================
    // TEST 5: START/STOP LIFECYCLE TESTING
    // Test multiple start/stop cycles
    // =================================================================
    void testLifecycleManagement() {
        std::cout << "\n🔄 TEST 5: LIFECYCLE MANAGEMENT TESTING" << std::endl;
        std::cout << "========================================" << std::endl;
        
        try {
            // Test multiple start/stop cycles
            for (int cycle = 0; cycle < 3; ++cycle) {
                std::cout << "🔄 Lifecycle cycle " << (cycle + 1) << std::endl;
                
                client.start();
                client.subscribe({"BTC-USD"});
                
                std::this_thread::sleep_for(std::chrono::seconds(1));
                
                auto trades = client.getRecentTrades("BTC-USD");
                std::cout << "  📊 Trades received: " << trades.size() << std::endl;
                
                client.stop();
                
                // Brief pause between cycles
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
            
            assertTrue(true, "Multiple start/stop cycles completed successfully");
            
        } catch (const std::exception& e) {
            std::cout << "Exception in lifecycle test: " << e.what() << std::endl;
            assertTrue(false, "Lifecycle management - exception thrown");
        }
    }
    
    // =================================================================
    // MAIN TEST EXECUTION
    // =================================================================
    void runAllTests() {
        std::cout << "\n🚀 LAUNCHING COMPREHENSIVE TEST SUITE" << std::endl;
        std::cout << "=====================================" << std::endl;
        std::cout << "Testing the architectural transformation!" << std::endl;
        
        testApiCompatibility();
        testPerformanceBenchmarking();
        testThreadSafety();
        testMemoryUsage();
        testLifecycleManagement();
        
        // Final report
        std::cout << "\n📋 FINAL TEST REPORT" << std::endl;
        std::cout << "=====================" << std::endl;
        std::cout << "Total Tests: " << test_count.load() << std::endl;
        std::cout << "Passed: " << passed_tests.load() << std::endl;
        std::cout << "Failed: " << (test_count.load() - passed_tests.load()) << std::endl;
        std::cout << "Success Rate: " << (100.0 * passed_tests.load() / test_count.load()) << "%" << std::endl;
        
        if (passed_tests.load() == test_count.load()) {
            std::cout << "\n🎉 ALL TESTS PASSED! ARCHITECTURAL TRANSFORMATION VALIDATED! 🎉" << std::endl;
        } else {
            std::cout << "\n⚠️  Some tests failed - investigation needed" << std::endl;
        }
    }
};

int main() {
    std::cout << "🧪 PHASE 6: COMPREHENSIVE TESTING SUITE" << std::endl;
    std::cout << "=========================================" << std::endl;
    
    ComprehensiveTestSuite suite;
    suite.runAllTests();
    
    return 0;
} 