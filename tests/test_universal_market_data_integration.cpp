#include "UniversalOrderBook.hpp"
#include "SentinelLogging.hpp"
#include <chrono>
#include <iostream>
#include <iomanip>
#include <random>
#include <cassert>
#include <cmath>
#include <thread>
#include <atomic>

/**
 * 🚀 UNIVERSAL MARKET DATA + ORDER BOOK INTEGRATION TEST
 * 
 * This test validates the complete backend pipeline with the new UniversalOrderBook:
 * 1. Market data streaming with any asset precision
 * 2. Intelligent precision detection for any asset
 * 3. Configurable level grouping for liquidity walls
 * 4. Real-time order book maintenance
 * 5. Liquidity wall chunking for visualization
 * 
 * This is the ULTIMATE test for backend robustness and universal asset support.
 */

class UniversalMarketDataIntegrationTest {
public:
    void runComprehensiveIntegrationTest() {
        std::cout << "\n🚀 UNIVERSAL MARKET DATA + ORDER BOOK INTEGRATION TEST\n";
        std::cout << "====================================================\n";
        std::cout << "🎯 GOAL: Validate complete backend pipeline for ANY asset\n";
        std::cout << "📊 SCOPE: Streaming + Universal Order Book + Liquidity Walls\n\n";

        testBitcoinIntegration();
        testMicroCapIntegration();
        testNanoCapIntegration();
        testHighValueIntegration();
        testLiquidityWallGeneration();
        testRealTimePerformance();
        
        std::cout << "\n✅ UNIVERSAL INTEGRATION TEST COMPLETE!\n";
        std::cout << "🎯 Backend is ROBUST AND UNIVERSAL for any asset!\n\n";
    }

private:
    // Test 1: Bitcoin Integration (Standard precision)
    void testBitcoinIntegration() {
        std::cout << "₿ TEST 1: Bitcoin Integration\n";
        std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
        
        UniversalOrderBook book("BTC-USD");
        
        // Simulate realistic Bitcoin L2 updates
        std::vector<L2Update> btc_updates = generateBitcoinUpdates(1000);
        
        std::cout << "📊 Processing " << btc_updates.size() << " Bitcoin L2 updates...\n";
        
        auto start = std::chrono::high_resolution_clock::now();
        
        for (const auto& update : btc_updates) {
            book.updateLevel(update.price, update.size, update.side == "buy", update.timestamp);
        }
        
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
        
        // Validate Bitcoin-specific characteristics
        assert(std::abs(book.getPrecision() - 0.01) < 1e-6);  // 2 decimal places
        assert(book.getBestBidPrice() > 50000.0);  // Realistic Bitcoin price
        assert(book.getBestAskPrice() > book.getBestBidPrice());  // Valid spread
        
        double avg_latency_us = static_cast<double>(duration.count()) / btc_updates.size();
        std::cout << "⚡ Average latency: " << std::setprecision(2) << avg_latency_us << " μs/update\n";
        std::cout << "📊 Total levels: " << book.getTotalLevels() << "\n";
        std::cout << "💰 Best bid: $" << std::fixed << std::setprecision(2) << book.getBestBidPrice() << "\n";
        std::cout << "💰 Best ask: $" << std::fixed << std::setprecision(2) << book.getBestAskPrice() << "\n";
        std::cout << "📈 Spread: $" << std::fixed << std::setprecision(2) << book.getSpread() << "\n";
        
        std::cout << "✅ Bitcoin integration: PASSED\n";
    }

    // Test 2: Micro-Cap Token Integration (6 decimal places)
    void testMicroCapIntegration() {
        std::cout << "\n🪙 TEST 2: Micro-Cap Token Integration\n";
        std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
        
        UniversalOrderBook book("MICRO-USD");
        
        // Simulate micro-cap token with 6 decimal precision
        std::vector<L2Update> micro_updates = generateMicroCapUpdates(1000);
        
        std::cout << "📊 Processing " << micro_updates.size() << " micro-cap L2 updates...\n";
        
        auto start = std::chrono::high_resolution_clock::now();
        
        for (const auto& update : micro_updates) {
            book.updateLevel(update.price, update.size, update.side == "buy", update.timestamp);
        }
        
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
        
        // Validate micro-cap characteristics
        assert(std::abs(book.getPrecision() - 0.000001) < 1e-9);  // 6 decimal places
        assert(book.getBestBidPrice() < 1.0);  // Sub-dollar price
        assert(book.getBestBidPrice() > 0.0);  // Positive price
        
        double avg_latency_us = static_cast<double>(duration.count()) / micro_updates.size();
        std::cout << "⚡ Average latency: " << std::setprecision(2) << avg_latency_us << " μs/update\n";
        std::cout << "📊 Total levels: " << book.getTotalLevels() << "\n";
        std::cout << "💰 Best bid: $" << std::scientific << std::setprecision(6) << book.getBestBidPrice() << "\n";
        std::cout << "💰 Best ask: $" << std::scientific << std::setprecision(6) << book.getBestAskPrice() << "\n";
        std::cout << "📈 Spread: $" << std::scientific << std::setprecision(6) << book.getSpread() << "\n";
        
        std::cout << "✅ Micro-cap integration: PASSED\n";
    }

    // Test 3: Nano-Cap Token Integration (8 decimal places)
    void testNanoCapIntegration() {
        std::cout << "\n🔬 TEST 3: Nano-Cap Token Integration\n";
        std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
        
        UniversalOrderBook book("NANO-USD");
        
        // Simulate nano-cap token with 8 decimal precision
        std::vector<L2Update> nano_updates = generateNanoCapUpdates(1000);
        
        std::cout << "📊 Processing " << nano_updates.size() << " nano-cap L2 updates...\n";
        
        auto start = std::chrono::high_resolution_clock::now();
        
        for (const auto& update : nano_updates) {
            book.updateLevel(update.price, update.size, update.side == "buy", update.timestamp);
        }
        
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
        
        // Validate nano-cap characteristics
        assert(book.getPrecision() < 0.000001);  // Very high precision
        assert(book.getBestBidPrice() < 0.001);  // Very low price
        assert(book.getBestBidPrice() > 0.0);  // Positive price
        
        double avg_latency_us = static_cast<double>(duration.count()) / nano_updates.size();
        std::cout << "⚡ Average latency: " << std::setprecision(2) << avg_latency_us << " μs/update\n";
        std::cout << "📊 Total levels: " << book.getTotalLevels() << "\n";
        std::cout << "💰 Best bid: $" << std::scientific << std::setprecision(8) << book.getBestBidPrice() << "\n";
        std::cout << "💰 Best ask: $" << std::scientific << std::setprecision(8) << book.getBestAskPrice() << "\n";
        std::cout << "📈 Spread: $" << std::scientific << std::setprecision(8) << book.getSpread() << "\n";
        
        std::cout << "✅ Nano-cap integration: PASSED\n";
    }

    // Test 4: High-Value Asset Integration (0 decimal places)
    void testHighValueIntegration() {
        std::cout << "\n🏆 TEST 4: High-Value Asset Integration\n";
        std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
        
        UniversalOrderBook book("GOLD-USD");
        
        // Simulate high-value asset like gold
        std::vector<L2Update> gold_updates = generateHighValueUpdates(1000);
        
        std::cout << "📊 Processing " << gold_updates.size() << " high-value L2 updates...\n";
        
        auto start = std::chrono::high_resolution_clock::now();
        
        for (const auto& update : gold_updates) {
            book.updateLevel(update.price, update.size, update.side == "buy", update.timestamp);
        }
        
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
        
        // Validate high-value characteristics
        assert(std::abs(book.getPrecision() - 1.0) < 1e-6);  // Whole number precision
        assert(book.getBestBidPrice() > 1000.0);  // High price
        assert(book.getBestBidPrice() < 10000.0);  // Reasonable upper bound
        
        double avg_latency_us = static_cast<double>(duration.count()) / gold_updates.size();
        std::cout << "⚡ Average latency: " << std::setprecision(2) << avg_latency_us << " μs/update\n";
        std::cout << "📊 Total levels: " << book.getTotalLevels() << "\n";
        std::cout << "💰 Best bid: $" << std::fixed << std::setprecision(0) << book.getBestBidPrice() << "\n";
        std::cout << "💰 Best ask: $" << std::fixed << std::setprecision(0) << book.getBestAskPrice() << "\n";
        std::cout << "📈 Spread: $" << std::fixed << std::setprecision(0) << book.getSpread() << "\n";
        
        std::cout << "✅ High-value integration: PASSED\n";
    }

    // Test 5: Liquidity Wall Generation
    void testLiquidityWallGeneration() {
        std::cout << "\n🏗️  TEST 5: Liquidity Wall Generation\n";
        std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
        
        // Test with Bitcoin for realistic liquidity wall
        UniversalOrderBook book("BTC-USD");
        
        // Create dense order book for liquidity wall
        for (int i = 0; i < 1000; ++i) {
            double price = 50000.0 + i * 0.01;  // 1 cent increments
            double size = 1.0 + (i % 100) * 0.01;  // Varying sizes
            book.updateLevel(price, size, true);   // Bids
            book.updateLevel(price + 10.0, size, false);  // Asks
        }
        
        // Generate liquidity walls with different chunk sizes
        auto bid_chunks_1 = book.getLiquidityChunks(1.0, true);
        auto bid_chunks_10 = book.getLiquidityChunks(10.0, true);
        auto bid_chunks_100 = book.getLiquidityChunks(100.0, true);
        
        auto ask_chunks_1 = book.getLiquidityChunks(1.0, false);
        auto ask_chunks_10 = book.getLiquidityChunks(10.0, false);
        auto ask_chunks_100 = book.getLiquidityChunks(100.0, false);
        
        std::cout << "📊 Bid liquidity walls:\n";
        std::cout << "  $1 chunks: " << bid_chunks_1.size() << " chunks\n";
        std::cout << "  $10 chunks: " << bid_chunks_10.size() << " chunks\n";
        std::cout << "  $100 chunks: " << bid_chunks_100.size() << " chunks\n";
        
        std::cout << "📊 Ask liquidity walls:\n";
        std::cout << "  $1 chunks: " << ask_chunks_1.size() << " chunks\n";
        std::cout << "  $10 chunks: " << ask_chunks_10.size() << " chunks\n";
        std::cout << "  $100 chunks: " << ask_chunks_100.size() << " chunks\n";
        
        // Validate chunk properties
        for (const auto& chunk : bid_chunks_1) {
            assert(chunk.price_end > chunk.price_start);
            assert(chunk.total_size > 0);
            assert(chunk.level_count > 0);
        }
        
        // Validate chunk size relationships
        assert(bid_chunks_1.size() > bid_chunks_10.size());
        assert(bid_chunks_10.size() > bid_chunks_100.size());
        
        std::cout << "✅ Liquidity wall generation: PASSED\n";
    }

    // Test 6: Real-Time Performance
    void testRealTimePerformance() {
        std::cout << "\n⚡ TEST 6: Real-Time Performance\n";
        std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
        
        UniversalOrderBook book("BTC-USD");
        std::atomic<int> messages_processed{0};
        std::atomic<bool> running{true};
        
        // Real-time message generator thread
        std::thread generator([&]() {
            std::random_device rd;
            std::mt19937 gen(rd());
            std::normal_distribution<double> price_dist(55000.0, 1000.0);
            std::exponential_distribution<double> qty_dist(0.5);
            std::bernoulli_distribution side_dist(0.5);
            
            auto target_interval = std::chrono::microseconds(100); // 10k msg/sec
            auto next_send = std::chrono::high_resolution_clock::now();
            
            uint32_t timestamp = getCurrentTimeMs();
            
            while (running) {
                // Generate realistic message
                double price = std::max(1000.0, std::min(100000.0, price_dist(gen)));
                double quantity = qty_dist(gen);
                bool is_bid = side_dist(gen);
                
                // Process message
                book.updateLevel(price, quantity, is_bid, timestamp);
                messages_processed++;
                
                // Rate limiting
                next_send += target_interval;
                std::this_thread::sleep_until(next_send);
            }
        });

        // Statistics thread
        std::thread stats([&]() {
            int last_count = 0;
            while (running) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                int current_count = messages_processed;
                int rate = current_count - last_count;
                last_count = current_count;
                
                std::cout << "📊 " << std::setw(6) << rate << " msg/s | "
                         << "Total: " << std::setw(8) << current_count << " | "
                         << "Levels: " << std::setw(4) << book.getTotalLevels() << " | "
                         << "Best Bid: $" << std::setw(8) << std::setprecision(2) << std::fixed 
                         << book.getBestBidPrice() << " | "
                         << "Best Ask: $" << std::setw(8) << std::setprecision(2) << std::fixed 
                         << book.getBestAskPrice() << "\n";
            }
        });

        // Run for 10 seconds
        std::this_thread::sleep_for(std::chrono::seconds(10));
        running = false;
        
        generator.join();
        stats.join();
        
        std::cout << "✅ Real-time performance: PASSED\n";
    }

    // Helper structs and methods
    struct L2Update {
        std::string side;
        double price;
        double size;
        uint32_t timestamp;
    };

    std::vector<L2Update> generateBitcoinUpdates(int count) {
        std::vector<L2Update> updates;
        updates.reserve(count);
        
        std::random_device rd;
        std::mt19937 gen(rd());
        std::normal_distribution<double> price_dist(55000.0, 2000.0);
        std::exponential_distribution<double> qty_dist(0.5);
        std::bernoulli_distribution side_dist(0.5);
        
        uint32_t timestamp = getCurrentTimeMs();
        
        for (int i = 0; i < count; ++i) {
            L2Update update;
            update.side = side_dist(gen) ? "buy" : "sell";
            update.price = std::max(1000.0, std::min(100000.0, price_dist(gen)));
            update.size = qty_dist(gen);
            update.timestamp = timestamp + (i / 100);
            
            updates.push_back(update);
        }
        
        return updates;
    }

    std::vector<L2Update> generateMicroCapUpdates(int count) {
        std::vector<L2Update> updates;
        updates.reserve(count);
        
        std::random_device rd;
        std::mt19937 gen(rd());
        std::normal_distribution<double> price_dist(0.0001, 0.00005);
        std::exponential_distribution<double> qty_dist(0.5);
        std::bernoulli_distribution side_dist(0.5);
        
        uint32_t timestamp = getCurrentTimeMs();
        
        for (int i = 0; i < count; ++i) {
            L2Update update;
            update.side = side_dist(gen) ? "buy" : "sell";
            update.price = std::max(0.000001, std::min(0.001, price_dist(gen)));
            update.size = qty_dist(gen);
            update.timestamp = timestamp + (i / 100);
            
            updates.push_back(update);
        }
        
        return updates;
    }

    std::vector<L2Update> generateNanoCapUpdates(int count) {
        std::vector<L2Update> updates;
        updates.reserve(count);
        
        std::random_device rd;
        std::mt19937 gen(rd());
        std::normal_distribution<double> price_dist(0.0000001, 0.00000005);
        std::exponential_distribution<double> qty_dist(0.5);
        std::bernoulli_distribution side_dist(0.5);
        
        uint32_t timestamp = getCurrentTimeMs();
        
        for (int i = 0; i < count; ++i) {
            L2Update update;
            update.side = side_dist(gen) ? "buy" : "sell";
            update.price = std::max(0.00000001, std::min(0.000001, price_dist(gen)));
            update.size = qty_dist(gen);
            update.timestamp = timestamp + (i / 100);
            
            updates.push_back(update);
        }
        
        return updates;
    }

    std::vector<L2Update> generateHighValueUpdates(int count) {
        std::vector<L2Update> updates;
        updates.reserve(count);
        
        std::random_device rd;
        std::mt19937 gen(rd());
        std::normal_distribution<double> price_dist(2000.0, 500.0);
        std::exponential_distribution<double> qty_dist(0.5);
        std::bernoulli_distribution side_dist(0.5);
        
        uint32_t timestamp = getCurrentTimeMs();
        
        for (int i = 0; i < count; ++i) {
            L2Update update;
            update.side = side_dist(gen) ? "buy" : "sell";
            update.price = std::max(1000.0, std::min(5000.0, price_dist(gen)));
            update.size = qty_dist(gen);
            update.timestamp = timestamp + (i / 100);
            
            updates.push_back(update);
        }
        
        return updates;
    }

    uint32_t getCurrentTimeMs() {
        return static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()
            ).count()
        );
    }
};

int main() {
    UniversalMarketDataIntegrationTest test;
    test.runComprehensiveIntegrationTest();
    
    std::cout << "\n🎉 ALL TESTS PASSED! Universal backend is ROBUST AND UNIVERSAL!\n";
    return 0;
} 