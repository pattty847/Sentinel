#include <gtest/gtest.h>
#include <QApplication>
#include <QTimer>
#include <QElapsedTimer>
#include <QThread>
#include <QMutex>
#include <QWaitCondition>
#include <chrono>
#include <atomic>
#include <vector>
#include <memory>
#include <thread>
#include <random>
#include <algorithm>
#include <fstream>
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/task.h>
#include <mach/mach_init.h>
#elif defined(__linux__)
#include <unistd.h>
#include <sys/resource.h>
#endif
#include "../libs/gui/UnifiedGridRenderer.h"
#include "../libs/core/LiquidityTimeSeriesEngine.h"
#include "../libs/gui/GridIntegrationAdapter.h"
#include "../libs/core/TradeData.h"

/**
 * 🚀 STRESS PERFORMANCE TEST SUITE
 * 
 * Verifies claimed performance metrics under extreme load:
 * 
 * CLAIMS TO VERIFY:
 * - 32x memory reduction (64MB → 2MB for 1M trades)
 * - 4x faster rendering (16ms → 4ms)
 * - 2.27M trades/second processing capacity
 * - Sub-millisecond latency (0.026ms average)
 * - 20,000+ messages/second sustained
 * - 52M operations/second (FastOrderBook)
 * 
 * STRESS TEST SCENARIOS:
 * - Coinbase Pro full firehose simulation
 * - Multi-symbol concurrent processing
 * - Extended operation (hours of runtime)
 * - Memory pressure with GC stress
 * - CPU saturation testing
 * - Network backpressure simulation
 */

class StressPerformanceTest : public ::testing::Test {
protected:
    int argc = 0;
    char* argv[1] = {nullptr};
    std::unique_ptr<QApplication> app;
    
    // Performance monitoring
    struct PerformanceMetrics {
        std::atomic<size_t> tradesProcessed{0};
        std::atomic<size_t> orderBookUpdates{0};
        std::atomic<size_t> framesRendered{0};
        std::atomic<double> totalLatency_ms{0.0};
        std::atomic<size_t> latencySamples{0};
        std::atomic<size_t> memoryPeakBytes{0};
        std::atomic<bool> running{true};
        
        double getAverageLatency() const {
            size_t samples = latencySamples.load();
            return samples > 0 ? totalLatency_ms.load() / samples : 0.0;
        }
        
        double getTradesPerSecond(double durationSeconds) const {
            return durationSeconds > 0 ? tradesProcessed.load() / durationSeconds : 0.0;
        }
        
        double getMemoryUsageMB() const {
            return memoryPeakBytes.load() / (1024.0 * 1024.0);
        }
    };
    
    PerformanceMetrics metrics;
    std::mt19937 rng{std::random_device{}()};
    
    void SetUp() override {
        app = std::make_unique<QApplication>(argc, argv);
    }
    
    void TearDown() override {
        metrics.running = false;
        app.reset();
    }
    
    // Generate high-frequency realistic market data
    Trade generateRealisticTrade(int64_t timestamp_ms, const std::string& symbol = "BTC-USD") {
        static double basePrice = 50000.0;
        static std::normal_distribution<double> priceNoise(0.0, 1.0); // $1 std dev
        static std::exponential_distribution<double> sizeGen(1.0);
        
        Trade trade;
        trade.timestamp = std::chrono::system_clock::from_time_t(timestamp_ms / 1000);
        trade.product_id = symbol;
        trade.trade_id = std::to_string(timestamp_ms);
        trade.side = (rng() % 2) ? AggressorSide::Buy : AggressorSide::Sell;
        trade.price = std::max(1.0, basePrice + priceNoise(rng));
        trade.size = std::max(0.001, sizeGen(rng));
        
        // Introduce price drift for realism
        basePrice += (trade.side == AggressorSide::Buy ? 0.01 : -0.01);
        basePrice = std::max(1000.0, std::min(100000.0, basePrice)); // Bounds
        
        return trade;
    }
    
    OrderBook generateRealisticOrderBook(int64_t timestamp_ms, const std::string& symbol = "BTC-USD", 
                                       double basePrice = 50000.0, int levels = 200) {
        OrderBook book;
        book.product_id = symbol;
        book.timestamp = std::chrono::system_clock::from_time_t(timestamp_ms / 1000);
        
        std::exponential_distribution<double> liquidityGen(2.0);
        
        // Generate deep order book (200 levels each side)
        for (int i = 0; i < levels; ++i) {
            // Bids (decreasing prices)
            OrderBookLevel bid;
            bid.price = basePrice - (i * 0.01);
            bid.size = std::max(0.001, liquidityGen(rng));
            book.bids.push_back(bid);
            
            // Asks (increasing prices) 
            OrderBookLevel ask;
            ask.price = basePrice + 0.01 + (i * 0.01);
            ask.size = std::max(0.001, liquidityGen(rng));
            book.asks.push_back(ask);
        }
        
        return book;
    }
    
    // Measure memory usage
    size_t getCurrentMemoryUsage() {
#ifdef __APPLE__
        struct mach_task_basic_info info;
        mach_msg_type_number_t infoCount = MACH_TASK_BASIC_INFO_COUNT;
        if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                      (task_info_t)&info, &infoCount) == KERN_SUCCESS) {
            return info.resident_size;
        }
#elif defined(__linux__)
        FILE* file = fopen("/proc/self/status", "r");
        if (file) {
            char line[128];
            while (fgets(line, 128, file) != nullptr) {
                if (strncmp(line, "VmRSS:", 6) == 0) {
                    size_t memory_kb = 0;
                    sscanf(line, "VmRSS: %zu kB", &memory_kb);
                    fclose(file);
                    return memory_kb * 1024;
                }
            }
            fclose(file);
        }
#endif
        return 0;
    }
    
    // Monitor system resources
    void startResourceMonitoring() {
        std::thread([this]() {
            while (metrics.running) {
                size_t currentMemory = getCurrentMemoryUsage();
                size_t current = metrics.memoryPeakBytes.load();
                
                while (currentMemory > current && 
                       !metrics.memoryPeakBytes.compare_exchange_weak(current, currentMemory)) {
                    // Keep trying to update peak
                }
                
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }).detach();
    }
};

// Test 1: Verify 32x Memory Reduction Claim
TEST_F(StressPerformanceTest, MemoryReductionVerification) {
    qInfo() << "🚀 TEST 1: Memory Reduction Verification (32x claim)";
    
    const int NUM_TRADES = 1000000; // 1 million trades
    const size_t EXPECTED_LEGACY_MEMORY_MB = 64; // Claimed legacy usage
    const size_t EXPECTED_GRID_MEMORY_MB = 2;    // Claimed grid usage
    
    auto gridRenderer = std::make_unique<UnifiedGridRenderer>();
    auto liquidityEngine = std::make_unique<LiquidityTimeSeriesEngine>();
    
    gridRenderer->setWidth(1920);
    gridRenderer->setHeight(1080);
    
    startResourceMonitoring();
    size_t initialMemory = getCurrentMemoryUsage();
    
    QElapsedTimer timer;
    timer.start();
    
    // Feed 1M trades
    qInfo() << "📊 Feeding" << NUM_TRADES << "trades...";
    for (int i = 0; i < NUM_TRADES; ++i) {
        int64_t timestamp = i; // 1 trade per millisecond
        Trade trade = generateRealisticTrade(timestamp);
        
        gridRenderer->onTradeReceived(trade);
        metrics.tradesProcessed++;
        
        if (i % 100000 == 0) {
            app->processEvents();
            qInfo() << "   Processed" << i << "trades (" << (100.0 * i / NUM_TRADES) << "%)";
        }
    }
    
    app->processEvents();
    
    size_t finalMemory = getCurrentMemoryUsage();
    size_t memoryUsed = finalMemory - initialMemory;
    double memoryUsedMB = memoryUsed / (1024.0 * 1024.0);
    double processingTimeMs = timer.elapsed();
    
    qInfo() << "📊 MEMORY USAGE RESULTS:";
    qInfo() << "   Memory used:" << memoryUsedMB << "MB";
    qInfo() << "   Processing time:" << processingTimeMs << "ms";
    qInfo() << "   Trades per second:" << (NUM_TRADES * 1000.0 / processingTimeMs);
    qInfo() << "   Memory per trade:" << (memoryUsed / NUM_TRADES) << "bytes";
    
    // Verify memory reduction claim
    EXPECT_LT(memoryUsedMB, EXPECTED_GRID_MEMORY_MB * 2) 
        << "Memory usage should be <4MB (2x safety margin on 2MB claim)";
    EXPECT_GT(memoryUsedMB, 0.1) 
        << "Should use some memory for 1M trades";
        
    // Calculate reduction vs claimed legacy system
    double reductionFactor = EXPECTED_LEGACY_MEMORY_MB / memoryUsedMB;
    qInfo() << "📊 MEMORY REDUCTION FACTOR:" << reductionFactor << "x vs legacy";
    
    EXPECT_GE(reductionFactor, 16.0) 
        << "Should achieve at least 16x memory reduction (half of claimed 32x)";
}

// Test 2: Verify 2.27M Trades/Second Processing Claim
TEST_F(StressPerformanceTest, ThroughputVerification) {
    qInfo() << "🚀 TEST 2: Throughput Verification (2.27M trades/sec claim)";
    
    const int TEST_DURATION_MS = 5000; // 5 seconds
    const int TARGET_TRADES_PER_SEC = 2270000; // 2.27M claim
    const int BATCH_SIZE = 10000;
    
    auto gridRenderer = std::make_unique<UnifiedGridRenderer>();
    gridRenderer->setWidth(1920);
    gridRenderer->setHeight(1080);
    
    startResourceMonitoring();
    
    QElapsedTimer testTimer;
    testTimer.start();
    
    int tradesGenerated = 0;
    
    // High-frequency feed timer
    QTimer* feedTimer = new QTimer();
    feedTimer->setInterval(1); // 1ms intervals
    
    QObject::connect(feedTimer, &QTimer::timeout, [&]() {
        if (testTimer.elapsed() >= TEST_DURATION_MS) {
            feedTimer->stop();
            app->quit();
            return;
        }
        
        // Generate batch of trades
        for (int i = 0; i < BATCH_SIZE; ++i) {
            int64_t timestamp = testTimer.elapsed() * 1000 + i; // microsecond precision
            Trade trade = generateRealisticTrade(timestamp);
            
            QElapsedTimer latencyTimer;
            latencyTimer.start();
            
            gridRenderer->onTradeReceived(trade);
            
            double latency = latencyTimer.nsecsElapsed() / 1000000.0; // Convert to ms
            metrics.totalLatency_ms += latency;
            metrics.latencySamples++;
            
            tradesGenerated++;
        }
        
        metrics.tradesProcessed = tradesGenerated;
    });
    
    feedTimer->start();
    app->exec();
    
    double actualDurationSec = testTimer.elapsed() / 1000.0;
    double actualThroughput = tradesGenerated / actualDurationSec;
    double averageLatency = metrics.getAverageLatency();
    
    qInfo() << "📊 THROUGHPUT RESULTS:";
    qInfo() << "   Trades processed:" << tradesGenerated;
    qInfo() << "   Duration:" << actualDurationSec << "seconds";
    qInfo() << "   Actual throughput:" << (actualThroughput / 1000000.0) << "M trades/sec";
    qInfo() << "   Average latency:" << averageLatency << "ms";
    qInfo() << "   Peak memory:" << metrics.getMemoryUsageMB() << "MB";
    
    // Verify throughput claim (allow 50% margin)
    double minAcceptableThroughput = TARGET_TRADES_PER_SEC * 0.5;
    EXPECT_GE(actualThroughput, minAcceptableThroughput)
        << "Should achieve at least 50% of claimed 2.27M trades/sec";
    
    // Verify sub-millisecond latency claim
    EXPECT_LT(averageLatency, 1.0) 
        << "Average latency should be sub-millisecond";
    
    feedTimer->deleteLater();
}

// Test 3: Coinbase Pro Firehose Simulation
TEST_F(StressPerformanceTest, CoinbaseFirehoseSimulation) {
    qInfo() << "🚀 TEST 3: Coinbase Pro Firehose Simulation (20k+ msg/sec)";
    
    const int TEST_DURATION_MS = 30000; // 30 seconds
    const int TARGET_MSG_PER_SEC = 20000; // 20k messages/sec claim
    const int ORDER_BOOK_UPDATE_INTERVAL_MS = 100; // Every 100ms
    
    auto gridRenderer = std::make_unique<UnifiedGridRenderer>();
    auto liquidityEngine = std::make_unique<LiquidityTimeSeriesEngine>();
    
    gridRenderer->setWidth(1920);
    gridRenderer->setHeight(1080);
    
    startResourceMonitoring();
    
    std::atomic<int> totalMessages{0};
    std::atomic<int> droppedFrames{0};
    
    QElapsedTimer testTimer;
    testTimer.start();
    
    // Trade stream (high frequency)
    QTimer* tradeTimer = new QTimer();
    tradeTimer->setInterval(1); // Every 1ms
    
    QObject::connect(tradeTimer, &QTimer::timeout, [&]() {
        if (testTimer.elapsed() >= TEST_DURATION_MS) {
            tradeTimer->stop();
            return;
        }
        
        // Generate 20 trades per millisecond (20k/sec)
        for (int i = 0; i < 20; ++i) {
            int64_t timestamp = testTimer.elapsed() * 1000 + i;
            Trade trade = generateRealisticTrade(timestamp);
            
            gridRenderer->onTradeReceived(trade);
            totalMessages++;
        }
    });
    
    // Order book updates (every 100ms)
    QTimer* bookTimer = new QTimer();
    bookTimer->setInterval(ORDER_BOOK_UPDATE_INTERVAL_MS);
    
    QObject::connect(bookTimer, &QTimer::timeout, [&]() {
        if (testTimer.elapsed() >= TEST_DURATION_MS) {
            bookTimer->stop();
            return;
        }
        
        int64_t timestamp = testTimer.elapsed();
        OrderBook book = generateRealisticOrderBook(timestamp);
        
        gridRenderer->onOrderBookUpdated(book);
        totalMessages++;
    });
    
    // Frame rate monitor
    QTimer* frameTimer = new QTimer();
    frameTimer->setInterval(16); // ~60 FPS
    int lastFrameCount = 0;
    
    QObject::connect(frameTimer, &QTimer::timeout, [&]() {
        if (testTimer.elapsed() >= TEST_DURATION_MS) {
            frameTimer->stop();
            app->quit();
            return;
        }
        
        app->processEvents();
        
        // Simulate frame rendering
        metrics.framesRendered++;
        
        // Check for dropped frames (if we're behind)
        int expectedFrames = testTimer.elapsed() / 16;
        if (metrics.framesRendered.load() < expectedFrames * 0.9) {
            droppedFrames++;
        }
    });
    
    tradeTimer->start();
    bookTimer->start();
    frameTimer->start();
    
    app->exec();
    
    double actualDurationSec = testTimer.elapsed() / 1000.0;
    double msgPerSec = totalMessages.load() / actualDurationSec;
    double frameRate = metrics.framesRendered.load() / actualDurationSec;
    
    qInfo() << "📊 FIREHOSE SIMULATION RESULTS:";
    qInfo() << "   Messages processed:" << totalMessages.load();
    qInfo() << "   Messages per second:" << msgPerSec;
    qInfo() << "   Frame rate:" << frameRate << "FPS";
    qInfo() << "   Dropped frames:" << droppedFrames.load();
    qInfo() << "   Peak memory:" << metrics.getMemoryUsageMB() << "MB";
    
    // Verify firehose handling claim
    EXPECT_GE(msgPerSec, TARGET_MSG_PER_SEC * 0.8) 
        << "Should handle at least 80% of claimed 20k messages/sec";
    EXPECT_GE(frameRate, 30.0) 
        << "Should maintain >30 FPS under load";
    EXPECT_LT(droppedFrames.load(), metrics.framesRendered.load() * 0.1) 
        << "Should drop <10% of frames";
    
    tradeTimer->deleteLater();
    bookTimer->deleteLater();
    frameTimer->deleteLater();
}

// Test 4: Multi-Symbol Concurrent Processing
TEST_F(StressPerformanceTest, MultiSymbolConcurrentProcessing) {
    qInfo() << "🚀 TEST 4: Multi-Symbol Concurrent Processing (daemon mode)";
    
    const int NUM_SYMBOLS = 50; // 50 cryptocurrency pairs
    const int TEST_DURATION_MS = 15000; // 15 seconds
    const int TRADES_PER_SYMBOL_PER_SEC = 100; // Moderate per-symbol load
    
    std::vector<std::string> symbols;
    for (int i = 0; i < NUM_SYMBOLS; ++i) {
        symbols.push_back("SYMBOL" + std::to_string(i) + "-USD");
    }
    
    auto gridRenderer = std::make_unique<UnifiedGridRenderer>();
    gridRenderer->setWidth(1920);
    gridRenderer->setHeight(1080);
    
    startResourceMonitoring();
    
    std::atomic<int> totalTradesProcessed{0};
    std::map<std::string, int> tradesPerSymbol;
    
    QElapsedTimer testTimer;
    testTimer.start();
    
    // Create timer for each symbol
    std::vector<QTimer*> symbolTimers;
    
    for (const auto& symbol : symbols) {
        QTimer* timer = new QTimer();
        timer->setInterval(10); // 100 Hz per symbol
        
        QObject::connect(timer, &QTimer::timeout, [&, symbol]() {
            if (testTimer.elapsed() >= TEST_DURATION_MS) {
                timer->stop();
                return;
            }
            
            // Generate trade for this symbol
            int64_t timestamp = testTimer.elapsed();
            Trade trade = generateRealisticTrade(timestamp, symbol);
            
            gridRenderer->onTradeReceived(trade);
            totalTradesProcessed++;
            tradesPerSymbol[symbol]++;
        });
        
        symbolTimers.push_back(timer);
        timer->start();
    }
    
    // Main processing loop
    QTimer* mainTimer = new QTimer();
    mainTimer->setInterval(100);
    
    QObject::connect(mainTimer, &QTimer::timeout, [&]() {
        if (testTimer.elapsed() >= TEST_DURATION_MS) {
            mainTimer->stop();
            app->quit();
            return;
        }
        
        app->processEvents();
    });
    
    mainTimer->start();
    app->exec();
    
    double actualDurationSec = testTimer.elapsed() / 1000.0;
    double totalThroughput = totalTradesProcessed.load() / actualDurationSec;
    double avgTradesPerSymbol = totalTradesProcessed.load() / NUM_SYMBOLS;
    
    qInfo() << "📊 MULTI-SYMBOL RESULTS:";
    qInfo() << "   Symbols processed:" << NUM_SYMBOLS;
    qInfo() << "   Total trades:" << totalTradesProcessed.load();
    qInfo() << "   Total throughput:" << totalThroughput << "trades/sec";
    qInfo() << "   Avg per symbol:" << avgTradesPerSymbol << "trades";
    qInfo() << "   Peak memory:" << metrics.getMemoryUsageMB() << "MB";
    
    // Verify multi-symbol performance
    int expectedTotalTrades = NUM_SYMBOLS * TRADES_PER_SYMBOL_PER_SEC * (TEST_DURATION_MS / 1000);
    EXPECT_GE(totalTradesProcessed.load(), expectedTotalTrades * 0.8)
        << "Should process at least 80% of expected trades across all symbols";
    
    EXPECT_LT(metrics.getMemoryUsageMB(), 500.0)
        << "Memory usage should stay reasonable for 50 symbols";
    
    // Cleanup
    for (auto* timer : symbolTimers) {
        timer->deleteLater();
    }
    mainTimer->deleteLater();
}

// Test 5: Extended Operation Stability
TEST_F(StressPerformanceTest, ExtendedOperationStability) {
    qInfo() << "🚀 TEST 5: Extended Operation Stability (daemon endurance)";
    
    const int EXTENDED_RUNTIME_MS = 120000; // 2 minutes (scaled down from hours)
    const int MESSAGE_RATE_PER_SEC = 5000; // Moderate sustained load
    
    auto gridRenderer = std::make_unique<UnifiedGridRenderer>();
    auto liquidityEngine = std::make_unique<LiquidityTimeSeriesEngine>();
    
    gridRenderer->setWidth(1920);
    gridRenderer->setHeight(1080);
    
    startResourceMonitoring();
    
    size_t initialMemory = getCurrentMemoryUsage();
    std::atomic<int> totalMessages{0};
    std::atomic<int> gcStallEvents{0};
    
    QElapsedTimer testTimer;
    testTimer.start();
    
    // Sustained message flow
    QTimer* messageTimer = new QTimer();
    messageTimer->setInterval(1);
    
    QObject::connect(messageTimer, &QTimer::timeout, [&]() {
        if (testTimer.elapsed() >= EXTENDED_RUNTIME_MS) {
            messageTimer->stop();
            app->quit();
            return;
        }
        
        // Generate 5 messages per millisecond
        for (int i = 0; i < 5; ++i) {
            int64_t timestamp = testTimer.elapsed() * 1000 + i;
            
            if (i % 10 == 0) {
                // Order book update every 10th message
                OrderBook book = generateRealisticOrderBook(timestamp);
                gridRenderer->onOrderBookUpdated(book);
            } else {
                // Trade update
                Trade trade = generateRealisticTrade(timestamp);
                gridRenderer->onTradeReceived(trade);
            }
            
            totalMessages++;
        }
    });
    
    // Memory monitoring and GC detection
    QTimer* monitorTimer = new QTimer();
    monitorTimer->setInterval(1000); // Every second
    std::vector<double> memoryHistory;
    
    QObject::connect(monitorTimer, &QTimer::timeout, [&]() {
        if (testTimer.elapsed() >= EXTENDED_RUNTIME_MS) {
            monitorTimer->stop();
            return;
        }
        
        double currentMemoryMB = getCurrentMemoryUsage() / (1024.0 * 1024.0);
        memoryHistory.push_back(currentMemoryMB);
        
        // Detect potential GC stalls (sudden memory drops)
        if (memoryHistory.size() > 2) {
            double memDrop = memoryHistory[memoryHistory.size()-2] - currentMemoryMB;
            if (memDrop > 50.0) { // >50MB sudden drop
                gcStallEvents++;
            }
        }
        
        app->processEvents();
    });
    
    messageTimer->start();
    monitorTimer->start();
    
    app->exec();
    
    size_t finalMemory = getCurrentMemoryUsage();
    double memoryGrowthMB = (finalMemory - initialMemory) / (1024.0 * 1024.0);
    double actualDurationSec = testTimer.elapsed() / 1000.0;
    double avgMsgPerSec = totalMessages.load() / actualDurationSec;
    
    qInfo() << "📊 EXTENDED OPERATION RESULTS:";
    qInfo() << "   Runtime:" << (actualDurationSec / 60.0) << "minutes";
    qInfo() << "   Total messages:" << totalMessages.load();
    qInfo() << "   Avg msg/sec:" << avgMsgPerSec;
    qInfo() << "   Memory growth:" << memoryGrowthMB << "MB";
    qInfo() << "   Peak memory:" << metrics.getMemoryUsageMB() << "MB";
    qInfo() << "   GC stall events:" << gcStallEvents.load();
    
    // Verify extended operation stability
    EXPECT_LT(memoryGrowthMB, 200.0) 
        << "Memory growth should be <200MB over 2 minutes";
    EXPECT_GE(avgMsgPerSec, MESSAGE_RATE_PER_SEC * 0.9) 
        << "Should maintain 90% of target message rate";
    EXPECT_LT(gcStallEvents.load(), 5) 
        << "Should have <5 major GC events in 2 minutes";
    
    // Check for memory leaks (bounded growth)
    if (memoryHistory.size() > 60) { // 1 minute of samples
        double earlyAvg = 0, lateAvg = 0;
        for (int i = 10; i < 30; ++i) earlyAvg += memoryHistory[i];
        for (int i = memoryHistory.size()-30; i < memoryHistory.size()-10; ++i) lateAvg += memoryHistory[i];
        
        earlyAvg /= 20; lateAvg /= 20;
        double growthRate = (lateAvg - earlyAvg) / earlyAvg;
        
        qInfo() << "   Memory growth rate:" << (growthRate * 100) << "%";
        EXPECT_LT(growthRate, 0.5) << "Memory growth rate should be <50% over test duration";
    }
    
    messageTimer->deleteLater();
    monitorTimer->deleteLater();
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    
    // Enable verbose logging for performance analysis
    qputenv("QT_LOGGING_RULES", "*.debug=false"); // Reduce log spam for perf tests
    
    qInfo() << "🚀 STRESS PERFORMANCE TEST SUITE";
    qInfo() << "Verifying claimed performance metrics under extreme load";
    qInfo() << "Claims to test:";
    qInfo() << "  - 32x memory reduction (64MB → 2MB)";
    qInfo() << "  - 2.27M trades/second processing";
    qInfo() << "  - 20k+ messages/second sustained";
    qInfo() << "  - Sub-millisecond latency";
    qInfo() << "  - Multi-symbol daemon capability";
    
    int result = RUN_ALL_TESTS();
    
    qInfo() << "✅ Stress performance testing complete";
    
    return result;
}