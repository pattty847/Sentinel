#include "chart/core/PriceFitter.hpp"
#include "chart/core/ChartViewController.hpp"
#include "chart/core/PublishedFrame.hpp"
#include <gtest/gtest.h>
#include <limits>

namespace {
struct Environment final : chart::IChartEnvironment, chart::ITickOracle, chart::IHeatmapViewSink {
    chart::SurfaceFacts size{100, 100, 1};
    double tick = 1, mid = 100;
    bool manual = false, resolution = true, buildable = true;
    int64_t anchor = 10000, oldest = 1000;
    std::optional<int64_t> decision;
    int decisions = 0, updates = 0, syncs = 0;
    bool attached() const override { return true; }
    chart::SurfaceFacts surface() const override { return size; }
    double drawnTick() const override { return tick; }
    double priceScale() const override { return 1; }
    bool manualTick() const override { return manual; }
    bool hasCurrentResolution() const override { return resolution; }
    bool priceScaleCurrent() const override { return true; }
    chart::SpanLimits limits() const override { return {}; }
    int64_t liveAnchorMs() const override { return anchor; }
    int64_t liveOpenEndMs() const override { return anchor; }
    int64_t oldestAvailableMs() const override { return oldest; }
    double recentMidPrice() const override { return mid; }
    std::optional<heatmap::gpu::ViewWindow> restView() const override { return std::nullopt; }
    int64_t predictTickUnits(const heatmap::gpu::ViewWindow&) const override { return 1; }
    bool buildsTick(int64_t, const heatmap::gpu::ViewWindow&) const override { return buildable; }
    int64_t autoTickState() const override { return 1; }
    heatmap::AutoTickParams autoTickParams() const override { return {1, .1}; }
    bool setAutoPriceTick(std::optional<int64_t> units) override {
        ++decisions;
        const bool changed = decision != units;
        decision = units;
        return changed;
    }
    void setView(const heatmap::gpu::ViewWindow&, bool) override { ++syncs; }
};
struct Candles final : chart::ICandleSource {
    std::vector<chart::CandleBar> bars;
    mutable qint64 from = 0, to = 0;
    mutable int64_t interval = 0, oldestInterval = 0;
    void visibleBars(const QString&, int64_t tf, int64_t start, int64_t end,
                     std::vector<chart::CandleBar>& out) const override {
        from = start; to = end; interval = tf;
        out = bars;
    }
    int64_t oldestTimeMs(const QString&, int64_t tf) const override { oldestInterval = tf; return 1000; }
};
struct PriceFitterTest : testing::Test {
    Environment env;
    chart::PublishedFrame published;
    chart::ChartViewController controller{env, env, env, published};
    chart::PriceFitter& fitter = controller.priceFitter();
    Candles candles;
    GridViewState& view = *controller.viewPtr();
    void SetUp() override {
        controller.session().symbol = "TEST";
        controller.session().timeframeMs = 1000;
        view.setViewportSize(100, 100);
        view.setViewport(1000, 11000, 90, 110);
        view.setAutoPriceScale(true);
        fitter.setCandleSource(&candles);
        fitter.setRequestUpdate([this] { ++env.updates; });
    }
    void bar(qint64 start, double lo, double hi, double close = 100) {
        chart::CandleBar b;
        b.timeStartMs = start; b.timeEndMs = start + 1000;
        b.low = lo; b.high = hi; b.close = close;
        candles.bars.push_back(b);
    }
    void expectFit(double lo, double hi, bool only = true) {
        const auto fit = fitter.gpuFitPriceWindow(1000, 11000, only);
        ASSERT_TRUE(fit);
        EXPECT_EQ(fit->first, lo);
        EXPECT_EQ(fit->second, hi);
    }
};

TEST_F(PriceFitterTest, LiveStepSolvesAutoPriceOnce) {
    bar(4000, 80, 120);
    view.setPriceFit([this](qint64 start, qint64 end, double &lo, double &hi) {
        return fitter.autoPriceFit(start, end, lo, hi);
    });
    view.enableAutoScroll(true);
    env.anchor = 12000;
    const auto before = fitter.autoPriceSolveCountForTest();
    controller.commands().onHeatmapLive();
    const double lo = view.getMinPrice(), hi = view.getMaxPrice();
    controller.commands().refitAfterHeatmapLive();
    EXPECT_EQ(fitter.autoPriceSolveCountForTest() - before, 1);
    EXPECT_EQ(view.getVisibleTimeEnd(), 13000);
    EXPECT_DOUBLE_EQ(view.getMinPrice(), lo);
    EXPECT_DOUBLE_EQ(view.getMaxPrice(), hi);
    EXPECT_DOUBLE_EQ(lo, 75);
    EXPECT_DOUBLE_EQ(hi, 125);
    // A live revision inside the same bucket still refits changed candle bounds.
    candles.bars.front().low = 70;
    controller.commands().onHeatmapLive();
    controller.commands().refitAfterHeatmapLive();
    EXPECT_EQ(fitter.autoPriceSolveCountForTest() - before, 2);
    EXPECT_LT(view.getMinPrice(), lo);
    view.enableAutoScroll(false);
    controller.commands().onHeatmapLive();
    controller.commands().refitAfterHeatmapLive();
    EXPECT_EQ(fitter.autoPriceSolveCountForTest() - before, 3);
}

TEST_F(PriceFitterTest, AutoVisibleBars) {
    bar(0, 1, 1000); // ends at the drawn left edge
    bar(1000, 90, 110);
    bar(10000, 80, 120);
    bar(11000, 1, 1000); // starts at the drawn right edge
    expectFit(75, 125);
    EXPECT_EQ(candles.from, 1000);
    EXPECT_EQ(candles.to, 11000);
    EXPECT_EQ(candles.interval, 1000);
    EXPECT_EQ(env.decision, 1);
    EXPECT_EQ(env.updates, 1);
}

TEST_F(PriceFitterTest, ManualRowRule) {
    env.manual = true;
    bar(1000, 90, 110);
    expectFit(87.5, 112.5);
    env.size.heightLogical = 7;
    expectFit(97, 104); // frozen P=1 clamp can crop
    EXPECT_EQ(env.decisions, 0);
}

TEST_F(PriceFitterTest, SpanLimitsAndClamps) {
    env.tick = 0; env.manual = true;
    bar(1000, 90, 110, 101);
    view.setMinSpans(0, 40);
    expectFit(80, 120);
    view.setMaxSpans(0, 10);
    expectFit(96, 106);
    controller.session().lastTrade = 102;
    expectFit(97, 107);
    controller.session().bookMid = 103;
    expectFit(98, 108);
    view.setMinSpans(0, 200); view.setMaxSpans(0, 0);
    expectFit(88.8, 111.2); // stale minimum from a different symbol ignored
}

TEST_F(PriceFitterTest, SnapToRowsAndFallback) {
    bar(1000, 90, 110);
    env.buildable = false;
    expectFit(87.5, 112.5);
    EXPECT_FALSE(env.decision);
    env.resolution = false;
    expectFit(88.8, 111.2);
    EXPECT_EQ(env.decisions, 1);
}

TEST_F(PriceFitterTest, EmptyAndNaNCases) {
    EXPECT_FALSE(fitter.gpuFitPriceWindow(1000, 11000, true));
    env.tick = 0; env.manual = true;
    expectFit(90, 110, false);
    bar(1000, 0, 100);
    bar(2000, 110, 90);
    bar(3000, 90, std::numeric_limits<double>::quiet_NaN());
    bar(4000, std::numeric_limits<double>::quiet_NaN(), 110);
    EXPECT_FALSE(fitter.gpuFitPriceWindow(1000, 11000, true));
    env.mid = std::numeric_limits<double>::quiet_NaN();
    EXPECT_FALSE(fitter.gpuFitPriceWindow(1000, 11000, false));
    double lo = 3, hi = 4;
    env.decision = 1;
    EXPECT_FALSE(fitter.autoPriceFit(1000, 11000, lo, hi));
    EXPECT_EQ(lo, 3); EXPECT_EQ(hi, 4);
    EXPECT_FALSE(env.decision);
}

TEST_F(PriceFitterTest, OnePriceKeepsSpanAndTick) {
    bar(1000, 109, 109);
    env.decision = 7;
    expectFit(99, 119);
    EXPECT_EQ(env.decision, 7);
    EXPECT_EQ(env.decisions, 0);
}

TEST_F(PriceFitterTest, CandleDirtyRefitHook) {
    bar(1000, 80, 120);
    view.setPriceFit([this](qint64 a, qint64 b, double& lo, double& hi) {
        return fitter.autoPriceFit(a, b, lo, hi);
    });
    fitter.priceCarry().emplace();
    controller.flags().reseedPrice = true;
    const auto version = view.getViewportVersion();
    fitter.candlesDirty("OTHER", 1000, 1000, 2000);
    fitter.candlesDirty("TEST", 2000, 1000, 2000);
    fitter.candlesDirty("TEST", 1000, 0, 1000);
    fitter.candlesDirty("TEST", 1000, 11000, 12000);
    controller.flags().limitsDeferred = true;
    fitter.candlesDirty("TEST", 1000, 1000, 2000);
    EXPECT_EQ(view.getViewportVersion(), version);
    controller.flags().limitsDeferred = false;
    fitter.candlesDirty("TEST", 1000, 1000, 1000); // fallback end = start + tf
    EXPECT_EQ(view.getViewportVersion(), version + 1);
    EXPECT_EQ(view.getMinPrice(), 75);
    EXPECT_EQ(view.getMaxPrice(), 125);
    EXPECT_TRUE(controller.flags().priceKnown);
    EXPECT_FALSE(controller.flags().reseedPrice);
    EXPECT_FALSE(fitter.priceCarry());
    EXPECT_EQ(env.syncs, 1);
    fitter.candlesDirty("TEST", 1000, 1000, 2000);
    EXPECT_EQ(view.getViewportVersion(), version + 1);
    view.setAutoPriceScale(false);
    bar(2000, 1, 1000);
    fitter.candlesDirty("TEST", 1000, 1000, 2000);
    EXPECT_EQ(view.getViewportVersion(), version + 1);
}

TEST_F(PriceFitterTest, TimeFitPaddingAndLimits) {
    auto fit = fitter.gpuFitTimeWindow();
    ASSERT_TRUE(fit);
    EXPECT_EQ(*fit, (std::pair<qint64, qint64>{1000, 11000}));
    view.setMaxSpans(3000, 0);
    fit = fitter.gpuFitTimeWindow();
    ASSERT_TRUE(fit);
    EXPECT_EQ(fit->second - fit->first, 3000);
    env.oldest = 0;
    EXPECT_EQ(fitter.gpuFitTimeWindow(), fit);
    env.anchor = 0;
    EXPECT_FALSE(fitter.gpuFitTimeWindow());
}
TEST_F(PriceFitterTest, NonWholeSecondIntervalsRemainMilliseconds) {
    view.setPriceFit([this](qint64 start, qint64 end, double& lo, double& hi) {
        return fitter.autoPriceFit(start, end, lo, hi);
    });
    bar(4000, 80, 120);
    for (int64_t tf : {100, 1500}) {
        controller.session().timeframeMs = tf;
        ASSERT_TRUE(fitter.gpuFitPriceWindow(1000, 11000, true));
        EXPECT_EQ(candles.interval, tf);
        env.oldest = 0;
        ASSERT_TRUE(fitter.gpuFitTimeWindow());
        EXPECT_EQ(candles.oldestInterval, tf);
        const auto before = fitter.autoPriceSolveCountForTest();
        fitter.candlesDirty("TEST", tf, 4000, 5000);
        EXPECT_EQ(fitter.autoPriceSolveCountForTest(), before + 1);
        fitter.candlesDirty("TEST", tf == 100 ? 1000 : 2000, 4000, 5000);
        EXPECT_EQ(fitter.autoPriceSolveCountForTest(), before + 1);
    }
}
} // namespace
