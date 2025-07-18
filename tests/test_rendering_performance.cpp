#include <gtest/gtest.h>
#include <QApplication>
#include <QElapsedTimer>
#include <QTimer>
#include "../libs/gui/gpuchartwidget.h"
#include "../libs/core/tradedata.h"

static const int NUM_TRADES = 1000000;

class RenderingBenchmark : public ::testing::Test {
protected:
    int argc = 0;
    char* argv[1] = {nullptr};
    std::unique_ptr<QApplication> app;
    
    void SetUp() override {
        app = std::make_unique<QApplication>(argc, argv);
    }
    
    void TearDown() override {
        app.reset();
    }
};

TEST_F(RenderingBenchmark, GPURenderer) {
    qInfo() << "🚀 Starting GPU benchmark with" << NUM_TRADES << "trades...";
    
    // Create and configure GPU widget for testing
    GPUChartWidget widget;
    widget.setWidth(800);         // Set width for proper coordinate calculation
    widget.setHeight(600);        // Set height for proper coordinate calculation
    widget.setVisible(true);      // Make widget visible (required for GPU rendering)
    
    // Configure for testing mode
    widget.setMaxPoints(NUM_TRADES + 1000);  // Allow room for all test points
    widget.setTimeSpan(60.0);                // 60 second time window
    widget.setPriceRange(100.0, 100.0 + NUM_TRADES * 0.01);  // Price range for test data
    
    // Process initial Qt events to ensure widget is ready
    app->processEvents();
    
    qInfo() << "🎯 GPU widget configured - starting trade processing...";
    
    QElapsedTimer timer;
    timer.start();
    
    // Add trades in batches with event processing
    const int BATCH_SIZE = 10000;
    for (int i = 0; i < NUM_TRADES; ++i) {
        Trade t{std::chrono::system_clock::now(), "BTC-USD", std::to_string(i), AggressorSide::Buy, 100.0 + i*0.01, 0.1};
        widget.onTradeReceived(t);
        
        // Process Qt events periodically to allow GPU updates
        if (i % BATCH_SIZE == 0) {
            app->processEvents();
            if (i > 0) {
                qInfo() << "📊 Processed" << i << "trades (" << (100.0 * i / NUM_TRADES) << "%)";
            }
        }
    }
    
    // Final event processing to ensure all GPU operations complete
    app->processEvents();
    
    qint64 elapsed = timer.elapsed();
    
    // Calculate performance metrics
    double tradesPerSecond = (NUM_TRADES * 1000.0) / elapsed;
    double msPerTrade = (double)elapsed / NUM_TRADES;
    
    qInfo() << "✅ [GPU Benchmark Results]";
    qInfo() << "   Total Time:" << elapsed << "ms";
    qInfo() << "   Trades/Second:" << QString::number(tradesPerSecond, 'f', 0);
    qInfo() << "   ms/Trade:" << QString::number(msPerTrade, 'f', 4);
    qInfo() << "   Total Trades:" << NUM_TRADES;
    
    // Performance assertions
    EXPECT_LT(msPerTrade, 0.01) << "GPU should process trades in <0.01ms each";
    EXPECT_GT(tradesPerSecond, 10000) << "GPU should handle >10k trades/second";
    
    qInfo() << "🎉 GPU benchmark completed successfully!";
}

int main(int argc, char** argv) {
    // Initialize Google Test first
    ::testing::InitGoogleTest(&argc, argv);
    
    // Create QApplication for all tests
    QApplication app(argc, argv);
    
    // Run tests
    int result = RUN_ALL_TESTS();
    
    // Give Qt a moment to clean up properly
    app.processEvents();
    
    return result;
}
