// =============================================================================
// PERFORMANCE BASELINE TEST - PRE-OPTIMIZATION MEASUREMENTS
// Target Hardware: Intel UHD Graphics 630 (perfect test platform)
// Purpose: Establish baseline metrics before GPU-first refactor
// =============================================================================

#include <iostream>
#include <chrono>
#include <thread>
#include <vector>
#include <random>
#include <fstream>
#include <numeric>  
#include <algorithm>
#include <cassert>
#include <map>

// Test current implementation
#include "../libs/core/coinbasestreamclient.h"

// 🎭 NEW: Include GUI for rendering baseline!
#include "../libs/gui/tradechartwidget.h"

// Simple Qt application for testing (if we need it)
class QApplication; // Forward declaration

// Stub data structures for testing
struct PerformanceMetrics {
    std::vector<double> frameTimes;
    std::vector<double> renderTimes;
    double avgFPS = 0.0;
    double worstFrameTime = 0.0;
    double p95FrameTime = 0.0;
    size_t pointCount = 0;
    std::string testName;
    
    // 🎨 NEW: GUI-specific metrics
    double totalPaintTime = 0.0;
    size_t orderBookLevels = 0;
    size_t orderBookSnapshots = 0;
};

class PerformanceBaselineTest {
private:
    CoinbaseStreamClient client;
    std::vector<PerformanceMetrics> results;
    
    // Helper for high-precision timing
    template<typename Func>
    double measureTime(Func&& func) {
        auto start = std::chrono::high_resolution_clock::now();
        func();
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration<double, std::milli>(end - start);
        return duration.count();
    }
    
    void generateTestData(size_t pointCount, std::vector<Trade>& testTrades) {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_real_distribution<> priceDist(50000.0, 51000.0);
        std::uniform_int_distribution<> sideDist(0, 1);
        
        testTrades.clear();
        testTrades.reserve(pointCount);
        
        auto baseTime = std::chrono::system_clock::now();
        for (size_t i = 0; i < pointCount; ++i) {
            Trade trade;
            trade.price = priceDist(gen);
            trade.size = 0.01 + (i % 100) * 0.001; // Varying sizes
            trade.timestamp = baseTime + std::chrono::milliseconds(i * 10);
            trade.side = sideDist(gen) == 0 ? AggressorSide::Buy : AggressorSide::Sell;
            trade.product_id = "BTC-USD";
            trade.trade_id = std::to_string(i);
            testTrades.push_back(trade);
        }
    }
    
    // 🎭 NEW: Generate realistic order book data for heatmap testing
    void generateOrderBookData(size_t snapshotCount, size_t levelsPerSnapshot, 
                              std::vector<OrderBook>& orderBooks) {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_real_distribution<> priceDist(49900.0, 50100.0); // Tight spread
        std::uniform_real_distribution<> sizeDist(0.1, 10.0);
        
        orderBooks.clear();
        orderBooks.reserve(snapshotCount);
        
        auto baseTime = std::chrono::system_clock::now();
        
        for (size_t snapshot = 0; snapshot < snapshotCount; ++snapshot) {
            OrderBook book;
            book.product_id = "BTC-USD";
            book.timestamp = baseTime + std::chrono::milliseconds(snapshot * 100);
            
            // Generate bid levels (below mid price)
            double midPrice = 50000.0;
            for (size_t level = 0; level < levelsPerSnapshot; ++level) {
                OrderBookLevel bidLevel;
                bidLevel.price = midPrice - (level + 1) * 0.01; // $0.01 increments
                bidLevel.size = sizeDist(gen);
                book.bids.push_back(bidLevel);
                
                OrderBookLevel askLevel;
                askLevel.price = midPrice + (level + 1) * 0.01; // $0.01 increments  
                askLevel.size = sizeDist(gen);
                book.asks.push_back(askLevel);
            }
            
            orderBooks.push_back(book);
        }
    }

public:
    PerformanceBaselineTest() = default;

    // =================================================================
    // TEST 1: DATA ACCESS PERFORMANCE SCALING
    // Measure performance degradation as data count increases
    // =================================================================
    void testDataAccessScaling() {
        std::cout << "\n📊 BASELINE TEST 1: Data Access Performance Scaling" << std::endl;
        std::cout << "====================================================" << std::endl;
        std::cout << "Hardware: Intel UHD Graphics 630 (integrated graphics)" << std::endl;
        std::cout << "Focus: Current data structures and access patterns" << std::endl;
        
        std::vector<size_t> pointCounts = {1000, 5000, 10000, 25000, 50000, 100000, 500000, 1000000};
        
        for (size_t pointCount : pointCounts) {
            std::cout << "\n🔍 Testing data access with " << pointCount << " points..." << std::endl;
            
            std::vector<Trade> testTrades;
            generateTestData(pointCount, testTrades);
            
            PerformanceMetrics metrics;
            metrics.testName = "DataAccess_" + std::to_string(pointCount);
            metrics.pointCount = pointCount;
            
            // Test 1a: Sequential access (current typical usage)
            double sequentialTime = measureTime([&]() {
                for (int iter = 0; iter < 100; ++iter) {
                    double sum = 0.0;
                    for (const auto& trade : testTrades) {
                        sum += trade.price * trade.size;
                    }
                    // Prevent optimization
                    volatile double dummy = sum;
                    (void)dummy;
                }
            });
            
            // Test 1b: Random access (zoom/pan scenarios)
            std::vector<size_t> randomIndices;
            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_int_distribution<> indexDist(0, pointCount - 1);
            for (int i = 0; i < 10000; ++i) {
                randomIndices.push_back(indexDist(gen));
            }
            
            double randomTime = measureTime([&]() {
                double sum = 0.0;
                for (size_t idx : randomIndices) {
                    sum += testTrades[idx].price;
                }
                volatile double dummy = sum;
                (void)dummy;
            });
            
            // Test 1c: Filtering (common chart operation)
            double filterTime = measureTime([&]() {
                for (int iter = 0; iter < 10; ++iter) {
                    std::vector<Trade> buyTrades;
                    buyTrades.reserve(pointCount / 2);
                    for (const auto& trade : testTrades) {
                        if (trade.side == AggressorSide::Buy) {
                            buyTrades.push_back(trade);
                        }
                    }
                }
            });
            
            std::cout << "  ⏱️  Sequential access (100 iterations): " << sequentialTime << " ms" << std::endl;
            std::cout << "  🎯 Random access (10k lookups): " << randomTime << " ms" << std::endl;
            std::cout << "  🔍 Filtering (10 iterations): " << filterTime << " ms" << std::endl;
            
            // Store results for comparison
            metrics.worstFrameTime = std::max({sequentialTime, randomTime, filterTime});
            metrics.avgFPS = pointCount / metrics.worstFrameTime; // Points per ms as "FPS"
            results.push_back(metrics);
            
            // Early exit if performance becomes unusable (>1 second for basic operations)
            if (metrics.worstFrameTime > 1000.0) {
                std::cout << "  ⚠️  Performance degraded beyond 1 second - stopping test" << std::endl;
                break;
            }
        }
    }
    
    // =================================================================
    // 🎭 NEW TEST 2: GUI RENDERING BASELINE - THE REAL BOTTLENECKS!
    // Uses the clean TradeChartWidget::measureRenderingPerformance() interface
    // =================================================================
    void testGUIRenderingBaseline() {
        std::cout << "\n🎨 BASELINE TEST 2: GUI Rendering Performance - QPainter Bottlenecks" << std::endl;
        std::cout << "====================================================================" << std::endl;
        std::cout << "Hardware: Intel UHD Graphics 630 (integrated graphics)" << std::endl;
        std::cout << "Focus: Current QPainter::drawEllipse() loops that kill performance" << std::endl;
        std::cout << "🎯 TARGET: Find the exact point counts where we hit 60 FPS (16ms) and 144 FPS (7ms) limits" << std::endl;
        
        // Create a minimal chart widget for testing (without Qt app overhead)
        TradeChartWidget testWidget;
        testWidget.setSymbol("BTC-USD");
        
        // Test configurations: trade points + order book scenarios
        std::vector<std::tuple<size_t, size_t, size_t>> testConfigs = {
            // (trade_points, orderbook_snapshots, levels_per_snapshot)
            {1000, 50, 20},      // Light load
            {5000, 100, 30},     // Moderate load  
            {10000, 150, 40},    // Heavy load
            {25000, 200, 50},    // Stress test
            {50000, 300, 60},    // Breaking point
            {100000, 400, 70},   // Unusable territory
        };
        
        for (const auto& [tradeCount, snapshotCount, levelsPerSnapshot] : testConfigs) {
            std::cout << "\n🔍 Testing rendering: " << tradeCount << " trades, " 
                     << snapshotCount << " order book snapshots, " 
                     << levelsPerSnapshot << " levels each" << std::endl;
            
            // Generate test data
            std::vector<Trade> testTrades;
            std::vector<OrderBook> testOrderBooks;
            generateTestData(tradeCount, testTrades);
            generateOrderBookData(snapshotCount, levelsPerSnapshot, testOrderBooks);
            
            // 🎭 USE THE CLEAN TESTING INTERFACE!
            std::string testName = "GUIRender_" + std::to_string(tradeCount) + "t_" + 
                                  std::to_string(snapshotCount) + "s_" + 
                                  std::to_string(levelsPerSnapshot) + "l";
                                  
            auto guiMetrics = testWidget.measureRenderingPerformance(testTrades, testOrderBooks, testName);
            
            // Convert to our metrics format
            PerformanceMetrics metrics;
            metrics.testName = guiMetrics.testConfiguration;
            metrics.pointCount = guiMetrics.tradeCount;
            metrics.orderBookSnapshots = guiMetrics.orderBookSnapshots;
            metrics.orderBookLevels = guiMetrics.orderBookLevels;
            metrics.totalPaintTime = guiMetrics.totalPaintTime_ms;
            metrics.worstFrameTime = guiMetrics.worstFrameTime_ms;
            metrics.avgFPS = guiMetrics.avgFPS;
            
            // Report results
            std::cout << "  🎨 RENDERING RESULTS:" << std::endl;
            std::cout << "    ⏱️  Total paint time (avg): " << metrics.totalPaintTime << " ms" << std::endl;
            std::cout << "    🐌 Worst frame time: " << metrics.worstFrameTime << " ms" << std::endl;
            std::cout << "    📊 Average FPS: " << metrics.avgFPS << std::endl;
            std::cout << "    💡 Total drawEllipse() calls: " << (tradeCount + snapshotCount * levelsPerSnapshot * 2) << std::endl;
            
            // Performance analysis
            std::cout << "  🎯 PERFORMANCE ANALYSIS:" << std::endl;
            if (metrics.totalPaintTime <= 7.0) {
                std::cout << "    ✅ 144Hz capable (≤7ms)" << std::endl;
            } else if (metrics.totalPaintTime <= 16.0) {
                std::cout << "    ✅ 60Hz capable (≤16ms)" << std::endl;
            } else if (metrics.totalPaintTime <= 33.0) {
                std::cout << "    ⚠️  30Hz capable (≤33ms)" << std::endl;
            } else {
                std::cout << "    ❌ Unusable (>33ms per frame)" << std::endl;
            }
            
            // GPU optimization potential
            std::cout << "  🚀 GPU OPTIMIZATION POTENTIAL:" << std::endl;
            std::cout << "    🎯 Current render time: " << metrics.totalPaintTime << " ms" << std::endl;
            std::cout << "    🎯 GPU target (<1ms): " << (metrics.totalPaintTime > 1.0 ? (metrics.totalPaintTime / 1.0) : 1.0) << "x improvement potential" << std::endl;
            
            results.push_back(metrics);
            
            // Early exit if completely unusable (>100ms per frame)
            if (metrics.totalPaintTime > 100.0) {
                std::cout << "  ⚠️  Rendering time exceeded 100ms - stopping GUI tests" << std::endl;
                break;
            }
        }
    }
    
    // =================================================================
    // TEST 3: MEMORY ALLOCATION PERFORMANCE
    // =================================================================
    void testMemoryAllocationPerformance() {
        std::cout << "\n💾 BASELINE TEST 3: Memory Allocation Performance" << std::endl;
        std::cout << "=================================================" << std::endl;
        
        std::vector<size_t> allocationSizes = {1000, 10000, 100000, 1000000};
        
        for (size_t size : allocationSizes) {
            std::cout << "\n🔍 Testing memory allocation for " << size << " elements..." << std::endl;
            
            // Test continuous allocation/deallocation (what happens during live updates)
            double allocTime = measureTime([&]() {
                for (int i = 0; i < 100; ++i) {
                    std::vector<Trade> trades;
                    trades.reserve(size);
                    for (size_t j = 0; j < size; ++j) {
                        Trade trade;
                        trade.price = 50000.0 + j;
                        trade.size = 0.01;
                        trade.side = AggressorSide::Buy;
                        trades.push_back(trade);
                    }
                }
            });
            
            // Test pre-allocated vs growing vector
            double growingTime = measureTime([&]() {
                for (int i = 0; i < 100; ++i) {
                    std::vector<Trade> trades; // No reserve
                    for (size_t j = 0; j < size; ++j) {
                        Trade trade;
                        trade.price = 50000.0 + j;
                        trade.size = 0.01;
                        trade.side = AggressorSide::Buy;
                        trades.push_back(trade);
                    }
                }
            });
            
            std::cout << "  📈 Pre-allocated vectors: " << allocTime << " ms" << std::endl;
            std::cout << "  📊 Growing vectors: " << growingTime << " ms" << std::endl;
            std::cout << "  💡 Performance difference: " << (growingTime - allocTime) << " ms" << std::endl;
        }
    }
    
    // =================================================================
    // TEST 4: LIVE DATA STREAMING PERFORMANCE
    // Simulate live data processing performance
    // =================================================================
    void testStreamingPerformance() {
        std::cout << "\n📡 BASELINE TEST 4: Live Streaming Performance" << std::endl;
        std::cout << "===============================================" << std::endl;
        
        try {
            client.start();
            client.subscribe({"BTC-USD"});
            
            // Let some real data accumulate
            std::cout << "⏳ Accumulating live data for 5 seconds..." << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(5));
            
            // Measure data access performance during live streaming
            auto startTime = std::chrono::steady_clock::now();
            size_t initialTradeCount = 0;
            size_t dataAccessCount = 0;
            
            // Simulate GUI updates for 10 seconds
            auto testEndTime = startTime + std::chrono::seconds(10);
            std::vector<double> accessTimes;
            
            while (std::chrono::steady_clock::now() < testEndTime) {
                double accessTime = measureTime([&]() {
                    auto trades = client.getRecentTrades("BTC-USD");
                    dataAccessCount++;
                    
                    // Simulate processing that a chart would do
                    if (!trades.empty()) {
                        double minPrice = trades[0].price;
                        double maxPrice = trades[0].price;
                        for (const auto& trade : trades) {
                            minPrice = std::min(minPrice, trade.price);
                            maxPrice = std::max(maxPrice, trade.price);
                        }
                        volatile double dummy = minPrice + maxPrice;
                        (void)dummy;
                    }
                });
                
                accessTimes.push_back(accessTime);
                
                // Simulate 60 FPS update rate
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
            }
            
            auto endTime = std::chrono::steady_clock::now();
            double testDuration = std::chrono::duration<double>(endTime - startTime).count();
            
            // Calculate statistics
            if (!accessTimes.empty()) {
                double avgAccessTime = std::accumulate(accessTimes.begin(), accessTimes.end(), 0.0) / accessTimes.size();
                double maxAccessTime = *std::max_element(accessTimes.begin(), accessTimes.end());
                
                std::cout << "  📊 Data access operations: " << dataAccessCount << std::endl;
                std::cout << "  ⏱️  Average access time: " << avgAccessTime << " ms" << std::endl;
                std::cout << "  🐌 Worst access time: " << maxAccessTime << " ms" << std::endl;
                std::cout << "  📈 Access rate: " << (dataAccessCount / testDuration) << " ops/sec" << std::endl;
                
                // Target from optimization plan: <1ms data access for smooth 144Hz
                if (maxAccessTime > 1.0) {
                    std::cout << "  ⚠️  Access time exceeds 1ms target for 144Hz rendering" << std::endl;
                }
            }
            
            client.stop();
            
        } catch (const std::exception& e) {
            std::cout << "  ❌ Streaming test failed: " << e.what() << std::endl;
        }
    }
    
    // =================================================================
    // TEST 5: COMPUTE-HEAVY OPERATIONS
    // Test operations that will be moved to GPU
    // =================================================================
    void testComputeOperations() {
        std::cout << "\n🧮 BASELINE TEST 5: CPU Compute Operations" << std::endl;
        std::cout << "===========================================" << std::endl;
        std::cout << "Testing operations that will be GPU-accelerated" << std::endl;
        
        // Generate large dataset
        std::vector<Trade> testTrades;
        generateTestData(100000, testTrades);
        
        // Test 1: Price/time coordinate calculations (done per frame)
        double coordCalcTime = measureTime([&]() {
            std::vector<std::pair<double, double>> coordinates;
            coordinates.reserve(testTrades.size());
            
            auto minTime = testTrades.front().timestamp;
            auto maxTime = testTrades.back().timestamp;
            auto timeRange = std::chrono::duration<double>(maxTime - minTime).count();
            
            for (const auto& trade : testTrades) {
                double x = std::chrono::duration<double>(trade.timestamp - minTime).count() / timeRange;
                double y = (trade.price - 50000.0) / 1000.0; // Normalize price
                coordinates.emplace_back(x, y);
            }
        });
        
        // Test 2: Color calculations based on trade properties
        double colorCalcTime = measureTime([&]() {
            std::vector<std::tuple<float, float, float, float>> colors;
            colors.reserve(testTrades.size());
            
            for (const auto& trade : testTrades) {
                float r = trade.side == AggressorSide::Buy ? 0.0f : 1.0f;
                float g = trade.side == AggressorSide::Buy ? 1.0f : 0.0f;
                float b = 0.0f;
                float a = std::min(1.0f, static_cast<float>(trade.size * 100.0)); // Size-based alpha
                colors.emplace_back(r, g, b, a);
            }
        });
        
        // Test 3: Aggregation operations (OHLC, volume bars)
        double aggregationTime = measureTime([&]() {
            std::map<int, std::vector<double>> timeBuckets;
            
            // Group trades into 1-minute buckets
            for (const auto& trade : testTrades) {
                auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                    trade.timestamp.time_since_epoch()).count();
                int bucket = static_cast<int>(seconds / 60); // 1-minute buckets
                timeBuckets[bucket].push_back(trade.price);
            }
            
            // Calculate OHLC for each bucket
            std::vector<std::tuple<double, double, double, double>> ohlc;
            for (const auto& bucket : timeBuckets) {
                if (!bucket.second.empty()) {
                    double open = bucket.second.front();
                    double close = bucket.second.back();
                    double high = *std::max_element(bucket.second.begin(), bucket.second.end());
                    double low = *std::min_element(bucket.second.begin(), bucket.second.end());
                    ohlc.emplace_back(open, high, low, close);
                }
            }
        });
        
        std::cout << "  📐 Coordinate calculations: " << coordCalcTime << " ms" << std::endl;
        std::cout << "  🎨 Color calculations: " << colorCalcTime << " ms" << std::endl;
        std::cout << "  📊 OHLC aggregation: " << aggregationTime << " ms" << std::endl;
        std::cout << "  💡 Total CPU compute time: " << (coordCalcTime + colorCalcTime + aggregationTime) << " ms" << std::endl;
        
        // Target: Move all this to GPU shaders for <1ms total time
        double totalTime = coordCalcTime + colorCalcTime + aggregationTime;
        if (totalTime > 3.0) { // Current target from optimization plan
            std::cout << "  🎯 GPU optimization potential: " << totalTime << " ms -> <3ms target" << std::endl;
        }
    }
    
    // =================================================================
    // SYSTEM INFO COLLECTION
    // =================================================================
    void collectSystemInfo() {
        std::cout << "\n💻 SYSTEM INFORMATION" << std::endl;
        std::cout << "=====================" << std::endl;
        
        // Basic system info
        std::cout << "  🎯 Target Hardware: Intel UHD Graphics 630" << std::endl;
        std::cout << "  📊 Test Configuration: CPU-based baseline" << std::endl;
        std::cout << "  🔧 Focus: Data structures, algorithms, and QPainter rendering" << std::endl;
        
        // Performance context
        std::cout << "\n🎯 OPTIMIZATION TARGETS:" << std::endl;
        std::cout << "  • Current: CPU-based rendering and calculations" << std::endl;
        std::cout << "  • Target: 1M+ points @ 144Hz (6.9ms per frame)" << std::endl;
        std::cout << "  • Goal: <3ms GPU render time" << std::endl;
        std::cout << "  • Interaction: <5ms latency" << std::endl;
    }
    
    // =================================================================
    // RESULTS EXPORT
    // =================================================================
    void exportResults() {
        std::cout << "\n💾 EXPORTING BASELINE RESULTS" << std::endl;
        std::cout << "==============================" << std::endl;
        
        std::ofstream csvFile("performance_baseline_results.csv");
        csvFile << "TestName,PointCount,WorstTime_ms,PointsPerMs,OptimizationPotential,"
                << "TotalPaintTime_ms,AvgFPS,OrderBookSnapshots,OrderBookLevels,DrawEllipseCalls\n";
        
        std::ofstream reportFile("performance_baseline_report.txt");
        reportFile << "PERFORMANCE BASELINE REPORT\n";
        reportFile << "===========================\n";
        reportFile << "Hardware: Intel UHD Graphics 630\n";
        reportFile << "Test Focus: Pre-GPU-optimization CPU + QPainter performance\n";
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        reportFile << "Test Date: " << std::ctime(&time_t) << "\n";
        
        for (const auto& result : results) {
            // CSV export with GUI metrics
            double pointsPerMs = (result.worstFrameTime > 0) ? 
                               (static_cast<double>(result.pointCount) / result.worstFrameTime) : 0.0;
            bool needsOptimization = result.worstFrameTime > 16.0; // 60 FPS threshold
            size_t drawEllipseCalls = result.pointCount + (result.orderBookSnapshots * result.orderBookLevels);
            
            csvFile << result.testName << "," 
                   << result.pointCount << ","
                   << result.worstFrameTime << ","
                   << pointsPerMs << ","
                   << (needsOptimization ? "HIGH" : "MODERATE") << ","
                   << result.totalPaintTime << ","
                   << result.avgFPS << ","
                   << result.orderBookSnapshots << ","
                   << result.orderBookLevels << ","
                   << drawEllipseCalls << "\n";
            
            // Detailed report
            reportFile << "Test: " << result.testName << "\n";
            reportFile << "  Point Count: " << result.pointCount << "\n";
            reportFile << "  Worst Time: " << result.worstFrameTime << " ms\n";
            reportFile << "  Points/ms: " << pointsPerMs << "\n";
            
            // GUI-specific metrics
            if (result.totalPaintTime > 0) {
                reportFile << "  GUI RENDERING METRICS:\n";
                reportFile << "    Total Paint Time: " << result.totalPaintTime << " ms\n";
                reportFile << "    Average FPS: " << result.avgFPS << "\n";
                reportFile << "    DrawEllipse Calls: " << drawEllipseCalls << "\n";
                reportFile << "    GPU Optimization Potential: " << (result.totalPaintTime > 1.0 ? (result.totalPaintTime / 1.0) : 1.0) << "x improvement\n";
            }
            reportFile << "\n";
        }
        
        std::cout << "  📄 Results exported to: performance_baseline_results.csv" << std::endl;
        std::cout << "  📋 Detailed report: performance_baseline_report.txt" << std::endl;
    }
    
    // =================================================================
    // MAIN TEST EXECUTION
    // =================================================================
    void runAllTests() {
        std::cout << "\n🚀 PERFORMANCE BASELINE TESTING - INCLUDING GUI!" << std::endl;
        std::cout << "=================================================" << std::endl;
        std::cout << "Measuring current performance before GPU optimization" << std::endl;
        std::cout << "Target Hardware: Intel UHD Graphics 630" << std::endl;
        std::cout << "🎭 Now includes QPainter rendering bottleneck measurements!" << std::endl;
        
        collectSystemInfo();
        testDataAccessScaling();
        testGUIRenderingBaseline();  // 🎭 THE STAR TEST!
        testMemoryAllocationPerformance();
        testStreamingPerformance();
        testComputeOperations();
        exportResults();
        
        // Summary with GUI focus
        std::cout << "\n📋 BASELINE TEST SUMMARY" << std::endl;
        std::cout << "=========================" << std::endl;
        std::cout << "Total test configurations: " << results.size() << std::endl;
        
        if (!results.empty()) {
            // Find the worst GUI performance
            auto worstGUI = std::max_element(results.begin(), results.end(),
                [](const PerformanceMetrics& a, const PerformanceMetrics& b) {
                    return a.totalPaintTime < b.totalPaintTime;
                });
            
            if (worstGUI->totalPaintTime > 0) {
                std::cout << "\n🎨 GUI RENDERING SUMMARY:" << std::endl;
                std::cout << "Slowest GUI render: " << worstGUI->totalPaintTime << " ms (" 
                         << worstGUI->testName << ")" << std::endl;
                std::cout << "Total drawEllipse calls: " << 
                             (worstGUI->pointCount + worstGUI->orderBookSnapshots * worstGUI->orderBookLevels) << std::endl;
            }
        }
        
        std::cout << "\n🎯 OPTIMIZATION TARGETS:" << std::endl;
        std::cout << "  • Target: 1M+ points @ 144Hz" << std::endl;
        std::cout << "  • GPU render time: <3ms" << std::endl;
        std::cout << "  • Zero dropped frames @ 20k msg/s" << std::endl;
        
        std::cout << "\n✅ Baseline established - ready for GPU optimization!" << std::endl;
        std::cout << "🔥 The QPainter bottlenecks have been measured - time for GPU revolution!" << std::endl;
    }
};

// We need to include QApplication for GUI testing
#include <QApplication>

int main(int argc, char *argv[]) {
    std::cout << "🧪 PERFORMANCE BASELINE TEST - NOW WITH GUI!" << std::endl;
    std::cout << "=============================================" << std::endl;
    std::cout << "Measuring pre-optimization performance on Intel UHD Graphics 630" << std::endl;
    std::cout << "🎭 Including QPainter rendering bottleneck measurements" << std::endl;
    
    // 🎭 Initialize Qt Application for GUI testing
    QApplication app(argc, argv);
    
    try {
        PerformanceBaselineTest test;
        test.runAllTests();
    } catch (const std::exception& e) {
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
} 