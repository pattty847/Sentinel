#include <gtest/gtest.h>
#include <QCoreApplication>
#include <QTimer>
#include "../libs/gui/gpudataadapter.h"
#include "../libs/gui/chartmodecontroller.h"
#include "../libs/gui/candlestickbatched.h"
#include "../libs/gui/candlelod.h"
#include <type_traits>

template <typename, typename = void>
struct has_addTrade : std::false_type {};
template <typename T>
struct has_addTrade<T, std::void_t<decltype(std::declval<T>().addTrade(std::declval<const Trade&>()))>> : std::true_type {};
static_assert(!has_addTrade<CandlestickBatched>::value, "CandlestickBatched should not expose addTrade");

TEST(MultiModeArchitecture, CandleBatchingAndModeController) {
    int argc = 0;
    char** argv = nullptr;
    QCoreApplication app(argc, argv);

    GPUDataAdapter adapter;
    ChartModeController controller;

    int candleSignals = 0;
    bool saw100ms = false;
    QObject::connect(&adapter, &GPUDataAdapter::candlesReady,
                     [&candleSignals, &saw100ms](const std::vector<CandleUpdate>& candles) {
                         candleSignals++;
                         EXPECT_GT(candles.size(), 0u);
                         for (const auto& c : candles) {
                             if (c.timeframe == CandleLOD::TF_100ms)
                                 saw100ms = true;
                         }
                     });

    auto base = std::chrono::system_clock::now();
    for (int i = 0; i < 10; ++i) {
        Trade t{base + std::chrono::milliseconds(i * 60), "TEST-USD", std::to_string(i), AggressorSide::Buy, 100.0 + i, 1.0};
        adapter.pushTrade(t);
    }

    QTimer::singleShot(30, &app, &QCoreApplication::quit);
    app.exec();

    EXPECT_GE(candleSignals, 1);
    EXPECT_TRUE(saw100ms);

    int modeChanged = 0;
    QObject::connect(&controller, &ChartModeController::modeChanged,
                     [&modeChanged](ChartMode){ modeChanged++; });
    controller.setMode(ChartMode::HIGH_FREQ_CANDLES);
    controller.setMode(ChartMode::TRADITIONAL_CANDLES);
    EXPECT_EQ(modeChanged, 2);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
