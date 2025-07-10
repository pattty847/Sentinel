#include "UniversalOrderBook.hpp"
#include "SentinelLogging.hpp"
#include <chrono>
#include <iostream>
#include <iomanip>
#include <random>
#include <cassert>
#include <cmath>

/**
 * 🚀 UNIVERSAL ORDER BOOK COMPREHENSIVE TEST
 * 
 * This test validates the UniversalOrderBook's ability to handle:
 * 1. ANY asset with ANY precision ($0.0000005, $50k, etc.)
 * 2. Configurable level grouping for liquidity walls
 * 3. Intelligent precision detection
 * 4. Memory efficiency and performance
 * 5. Liquidity wall chunking functionality
 */

class UniversalOrderBookTest {
public:
    void runComprehensiveTest() {
        std::cout << "\n🚀 UNIVERSAL ORDER BOOK COMPREHENSIVE TEST\n";
        std::cout << "===========================================\n";
        std::cout << "🎯 GOAL: Validate universal order book for any asset\n";
        std::cout << "📊 SCOPE: Precision detection, level grouping, liquidity walls\n\n";

        testBasicFunctionality();
        testPrecisionDetection();
        testLevelGrouping();
        testLiquidityWallChunking();
        testPerformanceAndMemory();
        testEdgeCases();
        
        std::cout << "\n✅ UNIVERSAL ORDER BOOK TEST COMPLETE!\n";
        std::cout << "🎯 Ready for ANY asset with intelligent precision and grouping.\n\n";
    }

private:
    // Test 1: Basic Functionality
    void testBasicFunctionality() {
        std::cout << "🧪 TEST 1: Basic Functionality\n";
        std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
        
        UniversalOrderBook book("BTC-USD");
        
        // Test empty state
        assert(book.getBestBidPrice() == 0.0);
        assert(book.getBestAskPrice() == std::numeric_limits<double>::max());
        assert(book.isEmpty());
        
        // Test basic bid/ask insertion
        book.updateLevel(50000.00, 1.0, true);
        book.updateLevel(50010.00, 2.0, false);
        
        assert(book.getBestBidPrice() == 50000.00);
        assert(book.getBestBidQuantity() == 1.0);
        assert(book.getBestAskPrice() == 50010.00);
        assert(book.getBestAskQuantity() == 2.0);
        assert(book.getSpread() == 10.00);
        
        // Test level updates
        book.updateLevel(50000.00, 3.0, true);
        assert(book.getBestBidQuantity() == 3.0);
        
        // Test level removal
        book.updateLevel(50000.00, 0.0, true);
        assert(book.getBestBidPrice() == 0.0);
        
        std::cout << "✅ Basic functionality: PASSED\n";
    }

    // Test 2: Precision Detection
    void testPrecisionDetection() {
        std::cout << "\n🎯 TEST 2: Precision Detection\n";
        std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
        
        // Test Bitcoin (2 decimal places)
        UniversalOrderBook btc_book("BTC-USD");
        btc_book.updateLevel(50000.01, 1.0, true);
        assert(std::abs(btc_book.getPrecision() - 0.01) < 1e-6);
        std::cout << "✅ Bitcoin precision: " << btc_book.getPrecision() << "\n";
        
        // Test micro-cap token (6 decimal places)
        UniversalOrderBook micro_book("MICRO-USD");
        micro_book.updateLevel(0.000001, 1.0, true);
        assert(std::abs(micro_book.getPrecision() - 0.000001) < 1e-9);
        std::cout << "✅ Micro-cap precision: " << micro_book.getPrecision() << "\n";
        
        // Test nano-cap token (8 decimal places)
        UniversalOrderBook nano_book("NANO-USD");
        nano_book.updateLevel(0.00000001, 1.0, true);
        assert(std::abs(nano_book.getPrecision() - 0.00000001) < 1e-11);
        std::cout << "✅ Nano-cap precision: " << nano_book.getPrecision() << "\n";
        
        // Test high-value asset (0 decimal places)
        UniversalOrderBook high_book("GOLD-USD");
        high_book.updateLevel(2000, 1.0, true);
        assert(std::abs(high_book.getPrecision() - 1.0) < 1e-6);
        std::cout << "✅ High-value precision: " << high_book.getPrecision() << "\n";
        
        std::cout << "✅ Precision detection: PASSED\n";
    }

    // Test 3: Level Grouping
    void testLevelGrouping() {
        std::cout << "\n🎯 TEST 3: Level Grouping\n";
        std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
        
        // Test $1 grouping
        UniversalOrderBook::Config config1(0.0, 100000.0, 1.0);
        UniversalOrderBook book1("BTC-USD", config1);
        
        // Add levels that should be grouped
        book1.updateLevel(50000.25, 1.0, true);  // Should group to 50000
        book1.updateLevel(50000.75, 2.0, true);  // Should group to 50000
        book1.updateLevel(50001.25, 3.0, true);  // Should group to 50001
        
        auto bids1 = book1.getBids();
        assert(bids1.size() == 2);  // Should be grouped into 2 levels
        assert(std::abs(bids1[0].price - 50001.0) < 1e-6);  // 50001
        assert(std::abs(bids1[1].price - 50000.0) < 1e-6);  // 50000
        assert(std::abs(bids1[1].size - 3.0) < 1e-6);  // Combined size (1+2)
        
        std::cout << "✅ $1 grouping: " << bids1.size() << " levels (vs 3 original)\n";
        
        // Test $0.01 grouping for micro-precision
        UniversalOrderBook::Config config2(0.0, 1.0, 0.01);
        UniversalOrderBook book2("MICRO-USD", config2);
        
        book2.updateLevel(0.000001, 1.0, true);
        book2.updateLevel(0.000002, 2.0, true);
        book2.updateLevel(0.000003, 3.0, true);
        
        auto bids2 = book2.getBids();
        assert(bids2.size() == 1);  // All should group to 0.00
        assert(std::abs(bids2[0].size - 6.0) < 1e-6);  // Combined size
        
        std::cout << "✅ $0.01 grouping: " << bids2.size() << " levels (vs 3 original)\n";
        
        std::cout << "✅ Level grouping: PASSED\n";
    }

    // Test 4: Liquidity Wall Chunking
    void testLiquidityWallChunking() {
        std::cout << "\n🎯 TEST 4: Liquidity Wall Chunking\n";
        std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
        
        UniversalOrderBook book("BTC-USD");
        
        // Create a realistic order book with many levels
        for (int i = 0; i < 100; ++i) {
            double price = 50000.0 + i * 0.01;  // 1 cent increments
            double size = 1.0 + (i % 10) * 0.1;  // Varying sizes
            book.updateLevel(price, size, true);   // Bids
            book.updateLevel(price + 10.0, size, false);  // Asks
        }
        
        // Test $1 chunks
        auto bid_chunks = book.getLiquidityChunks(1.0, true);
        auto ask_chunks = book.getLiquidityChunks(1.0, false);
        
        std::cout << "📊 Bid chunks ($1): " << bid_chunks.size() << " chunks\n";
        std::cout << "📊 Ask chunks ($1): " << ask_chunks.size() << " chunks\n";
        
        // Validate chunk properties
        for (const auto& chunk : bid_chunks) {
            assert(chunk.price_end > chunk.price_start);
            assert(chunk.total_size > 0);
            assert(chunk.level_count > 0);
            std::cout << "  💰 Chunk $" << std::fixed << std::setprecision(2) 
                     << chunk.price_start << "-$" << chunk.price_end 
                     << ": " << chunk.total_size << " size, " 
                     << chunk.level_count << " levels\n";
        }
        
        // Test $10 chunks
        auto bid_chunks_10 = book.getLiquidityChunks(10.0, true);
        std::cout << "📊 Bid chunks ($10): " << bid_chunks_10.size() << " chunks\n";
        assert(bid_chunks_10.size() < bid_chunks.size());  // Fewer, larger chunks
        
        std::cout << "✅ Liquidity wall chunking: PASSED\n";
    }

    // Test 5: Performance and Memory
    void testPerformanceAndMemory() {
        std::cout << "\n⚡ TEST 5: Performance and Memory\n";
        std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
        
        UniversalOrderBook book("BTC-USD");
        
        // Generate high-frequency updates
        std::vector<std::tuple<double, double, bool>> updates;
        updates.reserve(100000);
        
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_real_distribution<double> price_dist(50000.0, 60000.0);
        std::uniform_real_distribution<double> qty_dist(0.1, 10.0);
        std::bernoulli_distribution side_dist(0.5);
        
        for (int i = 0; i < 100000; ++i) {
            updates.emplace_back(price_dist(gen), qty_dist(gen), side_dist(gen));
        }
        
        std::cout << "🔥 Processing " << updates.size() << " updates...\n";
        
        auto start = std::chrono::high_resolution_clock::now();
        
        for (const auto& [price, qty, is_bid] : updates) {
            book.updateLevel(price, qty, is_bid);
        }
        
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
        
        double avg_latency_ns = static_cast<double>(duration.count()) / updates.size();
        double throughput_ops_per_sec = (updates.size() * 1e9) / duration.count();
        
        std::cout << "⚡ Average latency: " << std::setprecision(1) << avg_latency_ns << " ns/update\n";
        std::cout << "🚀 Throughput: " << std::setprecision(0) << throughput_ops_per_sec << " updates/sec\n";
        std::cout << "💾 Memory usage: ~" << (book.getTotalLevels() * sizeof(OrderBookLevel)) / 1024 << " KB\n";
        
        // Performance requirements
        assert(avg_latency_ns < 10000.0); // Sub-10μs requirement
        assert(throughput_ops_per_sec > 100000); // 100k+ updates/sec minimum
        
        std::cout << "✅ Performance and memory: PASSED\n";
    }

    // Test 6: Edge Cases
    void testEdgeCases() {
        std::cout << "\n🎯 TEST 6: Edge Cases\n";
        std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
        
        // Test extreme precision
        UniversalOrderBook::Config config(0.0, 1.0, 0.0, false);
        config.manual_precision = 0.000000001;  // 9 decimal places
        UniversalOrderBook book("EXTREME-USD", config);
        
        book.updateLevel(0.000000001, 1.0, true);
        book.updateLevel(0.000000002, 2.0, true);
        
        assert(book.getBidLevels() == 2);  // Should maintain separate levels
        std::cout << "✅ Extreme precision: " << book.getBidLevels() << " levels\n";
        
        // Test price bounds
        UniversalOrderBook::Config bounded_config(1000.0, 2000.0);
        UniversalOrderBook bounded_book("BOUNDED-USD", bounded_config);
        
        bounded_book.updateLevel(500.0, 1.0, true);   // Below min - should be ignored
        bounded_book.updateLevel(2500.0, 1.0, true);  // Above max - should be ignored
        bounded_book.updateLevel(1500.0, 1.0, true);  // Within bounds - should work
        
        assert(bounded_book.getBidLevels() == 1);  // Only the valid price
        std::cout << "✅ Price bounds: " << bounded_book.getBidLevels() << " levels\n";
        
        // Test zero and negative quantities
        UniversalOrderBook zero_book("ZERO-USD");
        zero_book.updateLevel(1000.0, 1.0, true);
        zero_book.updateLevel(1000.0, 0.0, true);   // Should remove level
        zero_book.updateLevel(1001.0, -1.0, true);  // Should be ignored
        
        assert(zero_book.getBidLevels() == 0);
        std::cout << "✅ Zero/negative quantities: " << zero_book.getBidLevels() << " levels\n";
        
        // Test very large numbers
        UniversalOrderBook large_book("LARGE-USD");
        large_book.updateLevel(1e12, 1e6, true);  // $1 trillion, 1M size
        large_book.updateLevel(1e12 + 1, 1e6, false);
        
        assert(large_book.getBestBidPrice() == 1e12);
        assert(large_book.getBestAskPrice() == 1e12 + 1);
        std::cout << "✅ Large numbers: Best bid $" << std::scientific << large_book.getBestBidPrice() << "\n";
        
        std::cout << "✅ Edge cases: PASSED\n";
    }
};

int main() {
    UniversalOrderBookTest test;
    test.runComprehensiveTest();
    
    std::cout << "\n🎉 ALL TESTS PASSED! Universal Order Book is ready for production.\n";
    return 0;
} 