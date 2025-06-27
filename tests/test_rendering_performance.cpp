#include <gtest/gtest.h>
#include <QApplication>
#include <QElapsedTimer>
#include "libs/gui/tradechartwidget.h"
#include "libs/gui/gpuchartwidget.h"
#include "libs/core/tradedata.h"

static const int NUM_TRADES = 100000;

class RenderingBenchmark : public ::testing::Test {
protected:
    int argc = 0;
    char* argv[1] = {nullptr};
    std::unique_ptr<QApplication> app;
    void SetUp() override {
        app = std::make_unique<QApplication>(argc, argv);
    }
};

TEST_F(RenderingBenchmark, QPainterBaseline) {
    TradeChartWidget widget;
    widget.setSymbol("BTC-USD");
    QElapsedTimer timer;
    timer.start();
    for (int i = 0; i < NUM_TRADES; ++i) {
        Trade t{std::chrono::system_clock::now(), "BTC-USD", std::to_string(i), AggressorSide::Buy, 100.0 + i*0.01, 0.1};
        widget.addTrade(t);
        if (i % 1000 == 0)
            QCoreApplication::processEvents();
    }
    qint64 elapsed = timer.elapsed();
    qInfo() << "[Benchmark] QPainter Total Time for" << NUM_TRADES << "trades:" << elapsed << "ms";
}

TEST_F(RenderingBenchmark, GPURenderer) {
    GPUChartWidget widget;
    QElapsedTimer timer;
    timer.start();
    for (int i = 0; i < NUM_TRADES; ++i) {
        Trade t{std::chrono::system_clock::now(), "BTC-USD", std::to_string(i), AggressorSide::Buy, 100.0 + i*0.01, 0.1};
        widget.onTradeReceived(t);
    }
    qint64 elapsed = timer.elapsed();
    qInfo() << "[Benchmark] GPUChartWidget Total Time for" << NUM_TRADES << "trades:" << elapsed << "ms";
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
