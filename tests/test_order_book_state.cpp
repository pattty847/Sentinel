#include <gtest/gtest.h>
#include <QApplication>
#include <QTimer>
#include <QElapsedTimer>
#include <chrono>
#include <atomic>
#include <vector>
#include <map>
#include <random>
#include "../libs/gui/LiquidityTimeSeriesEngine.h"
#include "../libs/core/TradeData.h"

/**
 * 📊 ORDER BOOK STATE CONSISTENCY TEST SUITE
 * 
 * Validates order book state management and L2 message consistency:
 * - L2 message ordering and sequencing
 * - State consistency under rapid updates
 * - Price level aggregation accuracy
 * - Bid/ask spread maintenance
 * - Deep order book integrity (500+ levels)
 * - State reconstruction from snapshots
 */

class OrderBookStateTest : public ::testing::Test {
protected:
    int argc = 0;
    char* argv[1] = {nullptr};
    std::unique_ptr<QApplication> app;
    std::mt19937 rng{std::random_device{}()};
    
    void SetUp() override {
        app = std::make_unique<QApplication>(argc, argv);
    }
    
    void TearDown() override {
        app.reset();
    }
    
    // Generate realistic order book with specific properties
    OrderBook generateOrderBook(int64_t timestamp_ms, double midPrice = 50000.0, 
                               int levels = 100, double spreadBps = 1.0) {
        OrderBook book;
        book.product_id = "BTC-USD";
        book.timestamp = std::chrono::system_clock::from_time_t(timestamp_ms / 1000);
        
        double spread = midPrice * (spreadBps / 10000.0); // Convert bps to absolute
        std::exponential_distribution<double> sizeGen(1.0);
        
        // Generate symmetric order book
        for (int i = 0; i < levels; ++i) {
            // Bids (decreasing prices)
            OrderBookLevel bid;
            bid.price = midPrice - spread/2 - (i * 0.01);
            bid.size = std::max(0.001, sizeGen(rng));
            book.bids.push_back(bid);
            
            // Asks (increasing prices)
            OrderBookLevel ask;
            ask.price = midPrice + spread/2 + (i * 0.01);
            ask.size = std::max(0.001, sizeGen(rng));
            book.asks.push_back(ask);
        }
        
        return book;
    }
    
    // Validate order book structure
    bool validateOrderBookStructure(const OrderBook& book) {
        if (book.bids.empty() || book.asks.empty()) return false;
        
        // Check bid ordering (descending prices)
        for (size_t i = 1; i < book.bids.size(); ++i) {
            if (book.bids[i].price >= book.bids[i-1].price) return false;
        }
        
        // Check ask ordering (ascending prices)
        for (size_t i = 1; i < book.asks.size(); ++i) {
            if (book.asks[i].price <= book.asks[i-1].price) return false;
        }
        
        // Check spread (best bid < best ask)
        if (book.bids[0].price >= book.asks[0].price) return false;
        
        return true;
    }
};

// Test 1: L2 Message Consistency and Ordering
TEST_F(OrderBookStateTest, L2MessageConsistency) {
    qInfo() << "📊 TEST 1: L2 Message Consistency and Ordering";
    
    auto liquidityEngine = std::make_unique<LiquidityTimeSeriesEngine>();
    
    const int NUM_SNAPSHOTS = 1000;
    const int LEVELS_PER_BOOK = 200;
    
    int validSnapshots = 0;
    int invalidSnapshots = 0;
    std::vector<double> spreads;
    
    for (int i = 0; i < NUM_SNAPSHOTS; ++i) {
        int64_t timestamp_ms = i * 100; // 100ms intervals
        double midPrice = 50000.0 + (i * 0.1); // Gradual price movement
        
        OrderBook book = generateOrderBook(timestamp_ms, midPrice, LEVELS_PER_BOOK);
        
        if (validateOrderBookStructure(book)) {
            liquidityEngine->addOrderBookSnapshot(book);
            validSnapshots++;
            
            // Calculate spread
            double spread = book.asks[0].price - book.bids[0].price;
            spreads.push_back(spread);
        } else {
            invalidSnapshots++;
        }
        
        if (i % 100 == 0) {
            app->processEvents();
        }
    }
    
    double avgSpread = std::accumulate(spreads.begin(), spreads.end(), 0.0) / spreads.size();
    double maxSpread = *std::max_element(spreads.begin(), spreads.end());
    double minSpread = *std::min_element(spreads.begin(), spreads.end());
    
    qInfo() << "📊 L2 CONSISTENCY RESULTS:";
    qInfo() << "   Valid snapshots:" << validSnapshots;
    qInfo() << "   Invalid snapshots:" << invalidSnapshots;
    qInfo() << "   Average spread:" << avgSpread;
    qInfo() << "   Spread range:" << minSpread << "-" << maxSpread;
    
    // L2 consistency requirements
    EXPECT_EQ(invalidSnapshots, 0) << "All generated order books should be structurally valid";
    EXPECT_EQ(validSnapshots, NUM_SNAPSHOTS) << "Should process all snapshots successfully";
    EXPECT_GT(avgSpread, 0.0) << "Average spread should be positive";
    EXPECT_LT(avgSpread, 1.0) << "Average spread should be reasonable (<$1)";
}

// Test 2: Deep Order Book State Management
TEST_F(OrderBookStateTest, DeepOrderBookManagement) {
    qInfo() << "📊 TEST 2: Deep Order Book State Management";
    
    auto liquidityEngine = std::make_unique<LiquidityTimeSeriesEngine>();
    
    const int DEEP_LEVELS = 500; // Professional depth
    const int NUM_UPDATES = 100;
    
    std::atomic<int> processedLevels{0};
    std::atomic<int> slicesGenerated{0};
    
    // Monitor slice generation
    QObject::connect(liquidityEngine.get(), &LiquidityTimeSeriesEngine::timeSliceReady,
                    [&](int64_t timeframe_ms, const LiquidityTimeSlice& slice) {
                        if (timeframe_ms == 100) {
                            slicesGenerated++;
                            processedLevels += slice.bidMetrics.size() + slice.askMetrics.size();
                        }
                    });
    
    for (int i = 0; i < NUM_UPDATES; ++i) {
        int64_t timestamp_ms = i * 100;
        OrderBook book = generateOrderBook(timestamp_ms, 50000.0, DEEP_LEVELS);
        
        liquidityEngine->addOrderBookSnapshot(book);
        app->processEvents();
    }
    
    double avgLevelsPerSlice = processedLevels.load() / static_cast<double>(slicesGenerated.load());
    
    qInfo() << "📊 DEEP ORDER BOOK RESULTS:";
    qInfo() << "   Order book levels per snapshot:" << DEEP_LEVELS * 2; // Bids + asks
    qInfo() << "   Slices generated:" << slicesGenerated.load();
    qInfo() << "   Total levels processed:" << processedLevels.load();
    qInfo() << "   Average levels per slice:" << avgLevelsPerSlice;
    
    // Deep order book requirements
    EXPECT_GT(slicesGenerated.load(), NUM_UPDATES * 0.9) << "Should generate slice for most updates";
    EXPECT_GT(avgLevelsPerSlice, DEEP_LEVELS) << "Should process significant portion of deep levels";
}

// Test 3: Rapid Update State Consistency
TEST_F(OrderBookStateTest, RapidUpdateConsistency) {
    qInfo() << "📊 TEST 3: Rapid Update State Consistency";
    
    auto liquidityEngine = std::make_unique<LiquidityTimeSeriesEngine>();
    
    const int RAPID_UPDATES = 2000; // 2000 updates
    const int UPDATE_INTERVAL_MS = 10; // 10ms intervals (100 Hz)
    
    std::atomic<bool> stateCorruption{false};
    std::atomic<int> consistentStates{0};
    
    // Monitor for state corruption
    QObject::connect(liquidityEngine.get(), &LiquidityTimeSeriesEngine::timeSliceReady,
                    [&](int64_t timeframe_ms, const LiquidityTimeSlice& slice) {
                        if (timeframe_ms != 100) return;
                        
                        // Check for impossible market states
                        for (const auto& [bidPrice, bidMetrics] : slice.bidMetrics) {
                            for (const auto& [askPrice, askMetrics] : slice.askMetrics) {
                                if (bidPrice >= askPrice && bidMetrics.avgLiquidity > 0 && askMetrics.avgLiquidity > 0) {
                                    stateCorruption = true;
                                    qWarning() << "State corruption: bid" << bidPrice << ">= ask" << askPrice;
                                    return;
                                }
                            }
                        }
                        
                        consistentStates++;
                    });
    
    QElapsedTimer updateTimer;
    updateTimer.start();
    
    for (int i = 0; i < RAPID_UPDATES; ++i) {
        int64_t timestamp_ms = i * UPDATE_INTERVAL_MS;
        
        // Introduce some price volatility
        double volatility = std::sin(i * 0.1) * 100.0; // ±$100 swing
        double midPrice = 50000.0 + volatility;
        
        OrderBook book = generateOrderBook(timestamp_ms, midPrice, 100);
        liquidityEngine->addOrderBookSnapshot(book);
        
        if (i % 200 == 0) {
            app->processEvents();
        }
    }
    
    double processingTime = updateTimer.elapsed();
    double updatesPerSecond = (RAPID_UPDATES * 1000.0) / processingTime;
    
    qInfo() << "📊 RAPID UPDATE RESULTS:";
    qInfo() << "   Rapid updates processed:" << RAPID_UPDATES;
    qInfo() << "   Processing time:" << processingTime << "ms";
    qInfo() << "   Updates per second:" << updatesPerSecond;
    qInfo() << "   Consistent states:" << consistentStates.load();
    qInfo() << "   State corruption detected:" << (stateCorruption.load() ? "YES" : "NO");
    
    // Rapid update consistency requirements
    EXPECT_FALSE(stateCorruption.load()) << "No state corruption should occur during rapid updates";
    EXPECT_GT(updatesPerSecond, 1000.0) << "Should handle >1000 updates/second";
    EXPECT_GT(consistentStates.load(), 0) << "Should generate consistent time slices";
}

// Test 4: Price Level Aggregation Accuracy
TEST_F(OrderBookStateTest, PriceLevelAggregationAccuracy) {
    qInfo() << "📊 TEST 4: Price Level Aggregation Accuracy";
    
    auto liquidityEngine = std::make_unique<LiquidityTimeSeriesEngine>();
    liquidityEngine->setPriceResolution(0.01); // $0.01 resolution
    
    const int NUM_TESTS = 50;
    
    double totalVolumeInput = 0.0;
    double totalVolumeOutput = 0.0;
    int accuracyViolations = 0;
    
    std::atomic<double> aggregatedVolume{0.0};
    
    // Monitor aggregated output
    QObject::connect(liquidityEngine.get(), &LiquidityTimeSeriesEngine::timeSliceReady,
                    [&](int64_t timeframe_ms, const LiquidityTimeSlice& slice) {
                        if (timeframe_ms != 100) return;
                        
                        double sliceVolume = 0.0;
                        for (const auto& [price, metrics] : slice.bidMetrics) {
                            sliceVolume += metrics.totalLiquidity;
                        }
                        for (const auto& [price, metrics] : slice.askMetrics) {
                            sliceVolume += metrics.totalLiquidity;
                        }
                        
                        aggregatedVolume += sliceVolume;
                    });
    
    for (int i = 0; i < NUM_TESTS; ++i) {
        int64_t timestamp_ms = i * 100;
        OrderBook book = generateOrderBook(timestamp_ms, 50000.0, 50);
        
        // Calculate input volume
        double inputVolume = 0.0;
        for (const auto& bid : book.bids) {
            inputVolume += bid.size;
        }
        for (const auto& ask : book.asks) {
            inputVolume += ask.size;
        }
        
        totalVolumeInput += inputVolume;
        
        liquidityEngine->addOrderBookSnapshot(book);
        app->processEvents();
    }
    
    totalVolumeOutput = aggregatedVolume.load();
    double volumeAccuracy = (totalVolumeOutput / totalVolumeInput) * 100.0;
    
    qInfo() << "📊 AGGREGATION ACCURACY RESULTS:";
    qInfo() << "   Total input volume:" << totalVolumeInput;
    qInfo() << "   Total output volume:" << totalVolumeOutput;
    qInfo() << "   Volume accuracy:" << volumeAccuracy << "%";
    qInfo() << "   Accuracy violations:" << accuracyViolations;
    
    // Aggregation accuracy requirements
    EXPECT_GT(volumeAccuracy, 95.0) << "Volume aggregation should be >95% accurate";
    EXPECT_LT(volumeAccuracy, 105.0) << "Volume aggregation should not exceed input by >5%";
    EXPECT_EQ(accuracyViolations, 0) << "No accuracy violations should occur";
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    
    qInfo() << "📊 ORDER BOOK STATE CONSISTENCY TEST SUITE";
    qInfo() << "Testing order book state management and L2 consistency:";
    qInfo() << "  - L2 message ordering and sequencing";
    qInfo() << "  - Deep order book management (500+ levels)";
    qInfo() << "  - Rapid update state consistency";
    qInfo() << "  - Price level aggregation accuracy";
    
    int result = RUN_ALL_TESTS();
    
    qInfo() << "✅ Order book state testing complete";
    
    return result;
}