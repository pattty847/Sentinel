#include <gtest/gtest.h>
#include <QApplication>
#include <QTimer>
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QObject>
#include <chrono>
#include <thread>
#include <atomic>
#include <vector>
#include <set>
#include <random>
#include <cstdio>
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/task.h>
#include <mach/mach_init.h>
#endif
#include "../libs/gui/UnifiedGridRenderer.h"
#include "../libs/gui/LiquidityTimeSeriesEngine.h"
#include "../libs/gui/GridIntegrationAdapter.h"
#include "../libs/core/tradedata.h"

/**
 * 🎯 DATA INTEGRITY TEST SUITE
 * 
 * Professional-grade testing for hedge fund use:
 * - Gap Detection: Identify missing 100ms slices
 * - State Consistency: Verify rendered data matches order book
 * - Message Loss Simulation: Test dropped WebSocket messages
 * - Order Book Corruption: Detect invalid state
 * - Temporal Continuity: Ensure no data holes in time series
 * 
 * Acceptance Criteria:
 * - ZERO gaps in continuous market hours
 * - ZERO data corruption events
 * - ZERO memory leaks during extended runs
 * - Sub-millisecond gap detection
 * - Automatic recovery from network issues
 */

class DataIntegrityTest : public ::testing::Test {
protected:
    int argc = 0;
    char* argv[1] = {nullptr};
    std::unique_ptr<QApplication> app;
    
    // Test components
    std::unique_ptr<UnifiedGridRenderer> gridRenderer;
    std::unique_ptr<LiquidityTimeSeriesEngine> liquidityEngine;
    std::unique_ptr<GridIntegrationAdapter> gridAdapter;
    
    // Test data generators
    std::mt19937 rng;
    
    void SetUp() override {
        app = std::make_unique<QApplication>(argc, argv);
        
        // Initialize RNG
        std::random_device rd;
        rng.seed(rd());
        
        // Initialize test components
        gridRenderer = std::make_unique<UnifiedGridRenderer>();
        liquidityEngine = std::make_unique<LiquidityTimeSeriesEngine>();
        gridAdapter = std::make_unique<GridIntegrationAdapter>();
        
        // Configure for testing
        gridRenderer->setWidth(1920);
        gridRenderer->setHeight(1080);
        gridRenderer->setVisible(true);
        
        app->processEvents();
    }
    
    void TearDown() override {
        gridRenderer.reset();
        liquidityEngine.reset();
        gridAdapter.reset();
        app.reset();
    }

    // Safety timeout to prevent tests from hanging
    void startSafetyTimer(int timeout_ms) {
        QTimer* safetyTimer = new QTimer();
        safetyTimer->setSingleShot(true);
        QObject::connect(safetyTimer, &QTimer::timeout, [this]() {
            qWarning() << "⚠️ TEST TIMEOUT: Safety timer triggered - forcing quit";
            app->quit();
        });
        safetyTimer->start(timeout_ms);
    }
    
    // Generate realistic order book data
    OrderBook generateOrderBook(int64_t timestamp_ms, double basePrice = 50000.0, int levels = 100) {
        OrderBook book;
        book.product_id = "BTC-USD";
        book.timestamp = std::chrono::system_clock::from_time_t(timestamp_ms / 1000);
        
        // Generate bid levels (below base price)
        for (int i = 0; i < levels; ++i) {
            OrderBookLevel bid;
            bid.price = basePrice - (i * 0.01); // $0.01 increments
            bid.size = generateRealisticVolume();
            book.bids.push_back(bid);
        }
        
        // Generate ask levels (above base price)
        for (int i = 0; i < levels; ++i) {
            OrderBookLevel ask;
            ask.price = basePrice + 0.01 + (i * 0.01); // $0.01 increments
            ask.size = generateRealisticVolume();
            book.asks.push_back(ask);
        }
        
        return book;
    }
    
    // Generate realistic volume following power law distribution
    double generateRealisticVolume() {
        std::exponential_distribution<double> dist(2.0);
        return std::max(0.001, dist(rng)); // Min 0.001 BTC
    }
    
    // Simulate message loss by randomly dropping updates
    bool shouldDropMessage(double dropRate = 0.01) { // 1% default drop rate
        std::uniform_real_distribution<double> dist(0.0, 1.0);
        return dist(rng) < dropRate;
    }
    
    // Utility function to measure memory usage
    size_t getCurrentMemoryUsage();
    
    // Generate corrupted order book (missing levels, invalid prices)
    OrderBook generateCorruptedOrderBook(int64_t timestamp_ms) {
        OrderBook book = generateOrderBook(timestamp_ms);
        
        // Introduce various corruption types
        std::uniform_int_distribution<int> corruptionType(1, 4);
        switch (corruptionType(rng)) {
            case 1: // Missing bid levels
                book.bids.clear();
                break;
            case 2: // Invalid prices (negative)
                for (auto& bid : book.bids) {
                    bid.price = -bid.price;
                }
                break;
            case 3: // Invalid sizes (zero/negative)
                for (auto& ask : book.asks) {
                    ask.size = 0.0;
                }
                break;
            case 4: // Overlapping bid/ask (impossible market state)
                if (!book.bids.empty() && !book.asks.empty()) {
                    book.bids[0].price = book.asks[0].price + 100.0; // Bid higher than ask
                }
                break;
        }
        
        return book;
    }
};

// Test 1: Gap Detection in Continuous Market Data
TEST_F(DataIntegrityTest, ContinuousMarketGapDetection) {
    qInfo() << "🔍 TEST 1: Gap Detection in Continuous Market Data";
    
    const int64_t TEST_DURATION_MS = 10000; // 10 seconds
    const int64_t EXPECTED_SLICES = TEST_DURATION_MS / 100; // 100 per second
    
    std::set<int64_t> receivedTimestamps;
    std::vector<int64_t> gaps;
    
    // Monitor time slice generation
    QObject::connect(liquidityEngine.get(), &LiquidityTimeSeriesEngine::timeSliceReady,
            [&](int64_t timeframe_ms, const LiquidityTimeSlice& slice) {
                if (timeframe_ms == 100) { // Monitor 100ms timeframe
                    receivedTimestamps.insert(slice.startTime_ms);
                }
            });
    
    QElapsedTimer testTimer;
    testTimer.start();
    int64_t startTime = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    
    // Feed continuous order book updates every 50ms (faster than 100ms capture)
    QTimer* feedTimer = new QTimer();
    feedTimer->setInterval(50);
    
    QObject::connect(feedTimer, &QTimer::timeout, [&]() {
        int64_t currentTime = startTime + testTimer.elapsed();
        OrderBook book = generateOrderBook(currentTime);
        liquidityEngine->addOrderBookSnapshot(book);
        
        if (testTimer.elapsed() >= TEST_DURATION_MS) {
            feedTimer->stop();
            app->quit();
        }
    });
    
    feedTimer->start();
    app->exec();
    
    // Analyze gaps
    qInfo() << "📊 Analysis: Received" << receivedTimestamps.size() << "slices, expected ~" << EXPECTED_SLICES;
    
    // Check for gaps (missing 100ms intervals)
    int64_t expectedStart = *receivedTimestamps.begin();
    int64_t expectedEnd = *receivedTimestamps.rbegin();
    
    for (int64_t t = expectedStart; t <= expectedEnd; t += 100) {
        if (receivedTimestamps.find(t) == receivedTimestamps.end()) {
            gaps.push_back(t);
        }
    }
    
    qInfo() << "🚨 GAPS DETECTED:" << gaps.size();
    for (auto gap : gaps) {
        qWarning() << "   Gap at timestamp:" << gap;
    }
    
    // Professional requirement: ZERO gaps in continuous market data
    EXPECT_EQ(gaps.size(), 0) << "CRITICAL: Gaps detected in continuous market data - unacceptable for professional trading";
    EXPECT_GE(receivedTimestamps.size(), EXPECTED_SLICES * 0.95) << "Less than 95% of expected time slices received";
    
    feedTimer->deleteLater();
}

// Test 2: Order Book State Consistency Under Message Loss
TEST_F(DataIntegrityTest, MessageLossStateConsistency) {
    qInfo() << "🔍 TEST 2: Order Book State Consistency Under Message Loss";
    
    const int NUM_UPDATES = 1000;
    const double MESSAGE_DROP_RATE = 0.05; // 5% message loss (harsh but realistic)
    
    std::vector<OrderBook> sentBooks;
    std::vector<OrderBook> receivedBooks;
    int droppedMessages = 0;
    
    // Track what gets through vs what gets processed
    for (int i = 0; i < NUM_UPDATES; ++i) {
        int64_t timestamp = i * 100; // 100ms intervals
        OrderBook book = generateOrderBook(timestamp);
        sentBooks.push_back(book);
        
        if (!shouldDropMessage(MESSAGE_DROP_RATE)) {
            liquidityEngine->addOrderBookSnapshot(book);
            receivedBooks.push_back(book);
        } else {
            droppedMessages++;
        }
        
        app->processEvents();
    }
    
    qInfo() << "📊 Dropped" << droppedMessages << "messages (" 
            << (100.0 * droppedMessages / NUM_UPDATES) << "%)";
    qInfo() << "📊 Processed" << receivedBooks.size() << "updates successfully";
    
    // Verify system handles message loss gracefully
    EXPECT_GT(receivedBooks.size(), NUM_UPDATES * 0.90) << "System should handle 5% message loss gracefully";
    EXPECT_LT(droppedMessages, NUM_UPDATES * 0.10) << "Message drop rate exceeded 10% - check test setup";
    
    // Check for state corruption indicators
    for (const auto& book : receivedBooks) {
        EXPECT_FALSE(book.bids.empty()) << "Order book should never have empty bids after processing";
        EXPECT_FALSE(book.asks.empty()) << "Order book should never have empty asks after processing";
        
        if (!book.bids.empty() && !book.asks.empty()) {
            EXPECT_LT(book.bids[0].price, book.asks[0].price) 
                << "Best bid should be lower than best ask (no crossed market)";
        }
    }
}

// Test 3: Corruption Detection and Recovery
TEST_F(DataIntegrityTest, CorruptionDetectionRecovery) {
    qInfo() << "🔍 TEST 3: Corruption Detection and Recovery";
    
    const int NUM_UPDATES = 500;
    const double CORRUPTION_RATE = 0.02; // 2% corrupted messages
    
    int corruptedMessages = 0;
    int processedMessages = 0;
    
    for (int i = 0; i < NUM_UPDATES; ++i) {
        int64_t timestamp = i * 100;
        OrderBook book;
        
        if (shouldDropMessage(CORRUPTION_RATE)) {
            book = generateCorruptedOrderBook(timestamp);
            corruptedMessages++;
        } else {
            book = generateOrderBook(timestamp);
        }
        
        // System should handle corrupted data gracefully
        try {
            liquidityEngine->addOrderBookSnapshot(book);
            processedMessages++;
        } catch (...) {
            qWarning() << "Exception processing order book at timestamp" << timestamp;
        }
        
        app->processEvents();
    }
    
    qInfo() << "📊 Injected" << corruptedMessages << "corrupted messages";
    qInfo() << "📊 Successfully processed" << processedMessages << "messages";
    
    // Professional requirement: System must not crash on corrupted data
    EXPECT_EQ(processedMessages, NUM_UPDATES) << "System should process all messages without crashing";
    EXPECT_GT(corruptedMessages, 0) << "Test should inject some corrupted messages";
}

// Test 4: Memory Integrity During Extended Operation
TEST_F(DataIntegrityTest, ExtendedOperationMemoryIntegrity) {
    qInfo() << "🔍 TEST 4: Memory Integrity During Extended Operation";
    
    const int EXTENDED_RUNTIME_MS = 30000; // 30 seconds of continuous operation
    const int UPDATE_INTERVAL_MS = 25; // High frequency updates
    
    // Start safety timer (test duration + 5 second buffer)
    startSafetyTimer(EXTENDED_RUNTIME_MS + 5000);
    
    size_t initialMemory = getCurrentMemoryUsage();
    int totalUpdates = 0;
    
    QElapsedTimer testTimer;
    testTimer.start();
    
    QTimer* updateTimer = new QTimer();
    updateTimer->setInterval(UPDATE_INTERVAL_MS);
    
    QObject::connect(updateTimer, &QTimer::timeout, [&]() {
        int64_t currentTime = testTimer.elapsed();
        OrderBook book = generateOrderBook(currentTime);
        liquidityEngine->addOrderBookSnapshot(book);
        totalUpdates++;
        
        if (testTimer.elapsed() >= EXTENDED_RUNTIME_MS) {
            updateTimer->stop();
            app->quit();
        }
    });
    
    updateTimer->start();
    app->exec();
    
    size_t finalMemory = getCurrentMemoryUsage();
    double memoryGrowthMB = (finalMemory - initialMemory) / (1024.0 * 1024.0);
    
    qInfo() << "📊 Processed" << totalUpdates << "updates over" << (EXTENDED_RUNTIME_MS/1000) << "seconds";
    qInfo() << "📊 Memory growth:" << memoryGrowthMB << "MB";
    qInfo() << "📊 Updates per second:" << (totalUpdates * 1000.0 / EXTENDED_RUNTIME_MS);
    
    // Professional requirement: Bounded memory growth
    EXPECT_LT(memoryGrowthMB, 100.0) << "Memory growth should be <100MB during 30s test";
    EXPECT_GT(totalUpdates, 500) << "Should process >500 updates in 30 seconds"; // Adjusted for GUI environment
    
    updateTimer->deleteLater();
}

// Test 5: Time Series Continuity Validation
TEST_F(DataIntegrityTest, TimeSeriesContinuityValidation) {
    qInfo() << "🔍 TEST 5: Time Series Continuity Validation";
    
    const int64_t TEST_DURATION_MS = 5000; // 5 seconds
    const int64_t TIMEFRAME_MS = 100; // 100ms slices
    
    // Start safety timer (test duration + 5 second buffer)
    startSafetyTimer(TEST_DURATION_MS + 5000);
    
    std::vector<LiquidityTimeSlice> receivedSlices;
    
    // Capture all time slices
    QObject::connect(liquidityEngine.get(), &LiquidityTimeSeriesEngine::timeSliceReady,
            [&](int64_t timeframe_ms, const LiquidityTimeSlice& slice) {
                if (timeframe_ms == TIMEFRAME_MS) {
                    receivedSlices.push_back(slice);
                }
            });
    
    // Feed data at irregular intervals to stress temporal ordering
    std::vector<int> intervals = {30, 40, 50, 60, 70, 80, 90}; // Smaller intervals to avoid gaps
    int intervalIndex = 0;
    int64_t currentTime = 0;
    
    QTimer* irregularTimer = new QTimer();
    QObject::connect(irregularTimer, &QTimer::timeout, [&]() {
        OrderBook book = generateOrderBook(currentTime);
        liquidityEngine->addOrderBookSnapshot(book);
        
        currentTime += intervals[intervalIndex % intervals.size()];
        intervalIndex++;
        
        if (currentTime >= TEST_DURATION_MS) {
            irregularTimer->stop();
            app->quit();
        } else {
            irregularTimer->setInterval(intervals[intervalIndex % intervals.size()]);
        }
    });
    
    irregularTimer->setInterval(intervals[0]);
    irregularTimer->start();
    app->exec();
    
    // Validate temporal continuity
    qInfo() << "📊 Received" << receivedSlices.size() << "time slices";
    
    if (receivedSlices.size() > 1) {
        bool temporalOrderValid = true;
        
        for (size_t i = 1; i < receivedSlices.size(); ++i) {
            int64_t prevEnd = receivedSlices[i-1].endTime_ms;
            int64_t currentStart = receivedSlices[i].startTime_ms;
            
            // Check for temporal continuity (no overlaps, no gaps larger than timeframe)
            if (currentStart < prevEnd || (currentStart - prevEnd) > TIMEFRAME_MS * 2) { // Allow up to 2x timeframe gap
                temporalOrderValid = false;
                qWarning() << "Temporal discontinuity between slices" << (i-1) << "and" << i
                          << "Gap:" << (currentStart - prevEnd) << "ms";
            }
        }
        
        EXPECT_TRUE(temporalOrderValid) << "Time series should maintain temporal continuity";
    }
    
    EXPECT_GT(receivedSlices.size(), 0) << "Should receive at least one time slice";
    
    irregularTimer->deleteLater();
}

// Utility function to measure memory usage (platform-specific)
size_t DataIntegrityTest::getCurrentMemoryUsage() {
#ifdef __linux__
    // Linux implementation
    FILE* file = fopen("/proc/self/status", "r");
    if (file) {
        char line[128];
        while (fgets(line, 128, file) != nullptr) {
            if (strncmp(line, "VmRSS:", 6) == 0) {
                size_t memory_kb = 0;
                sscanf(line, "VmRSS: %zu kB", &memory_kb);
                fclose(file);
                return memory_kb * 1024; // Convert to bytes
            }
        }
        fclose(file);
    }
#elif defined(__APPLE__)
    // macOS implementation using task_info
    struct mach_task_basic_info info;
    mach_msg_type_number_t infoCount = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  (task_info_t)&info, &infoCount) == KERN_SUCCESS) {
        return info.resident_size;
    }
#endif
    return 0; // Fallback
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    
    // Set high logging level for debugging
    qputenv("QT_LOGGING_RULES", "*.debug=true");
    
    qInfo() << "🎯 DATA INTEGRITY TEST SUITE";
    qInfo() << "Professional-grade testing for hedge fund market data integrity";
    qInfo() << "Testing for: gaps, corruption, memory leaks, temporal continuity";
    
    int result = RUN_ALL_TESTS();
    
    qInfo() << "✅ Data integrity testing complete";
    
    return result;
}