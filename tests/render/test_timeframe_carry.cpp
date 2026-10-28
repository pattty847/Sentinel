#include "chart/core/ChartViewController.hpp"
#include "chart/core/PublishedFrame.hpp"
#include "render/FrameContextBuilder.hpp"
#include "servermodel/RecordingCodec.hpp"
#include "SentinelLogging.hpp"
#include <gtest/gtest.h>
#include <cmath>
#include <limits>

namespace {
struct Environment final : chart::IChartEnvironment, chart::ITickOracle, chart::IHeatmapViewSink {
    chart::SurfaceFacts size{800, 400, 1};
    chart::SpanLimits spans;
    int64_t anchor = 5000;
    double mid = 100;
    bool current = false;
    int syncs = 0;
    std::function<void()> onTimeframe;
    bool attached() const override { return true; }
    chart::SurfaceFacts surface() const override { return size; }
    double drawnTick() const override { return 1; }
    double priceScale() const override { return 1; }
    bool manualTick() const override { return false; }
    bool hasCurrentResolution() const override { return false; }
    bool priceScaleCurrent() const override { return current; }
    chart::SpanLimits limits() const override { return spans; }
    int64_t liveAnchorMs() const override { return anchor; }
    int64_t liveOpenEndMs() const override { return anchor; }
    int64_t oldestAvailableMs() const override { return 0; }
    double recentMidPrice() const override { return mid; }
    std::optional<heatmap::gpu::ViewWindow> restView() const override { return std::nullopt; }
    int64_t predictTickUnits(const heatmap::gpu::ViewWindow&) const override { return 1; }
    bool buildsTick(int64_t, const heatmap::gpu::ViewWindow&) const override { return true; }
    int64_t autoTickState() const override { return 1; }
    heatmap::AutoTickParams autoTickParams() const override { return {1, .1}; }
    bool setAutoPriceTick(std::optional<int64_t>) override { return false; }
    void setView(const heatmap::gpu::ViewWindow&, bool) override { ++syncs; }

};
struct Timer {
    bool active = false;
    int starts = 0;
    int64_t now = 0, deadline = 0;
    std::function<void()> fire;
    bool isActive() const { return active; }
    void start(int ms) { active = true; deadline = now + ms; ++starts; }
    void stop() { active = false; }
    void advance(int ms) { now += ms; if (active && now >= deadline) { active = false; fire(); } }
};
struct TimerHandle final : chart::IPriceCarryTimer {
    Timer& timer;
    explicit TimerHandle(Timer& value) : timer(value) {}
    bool isActive() const override { return timer.isActive(); }
    void start(int ms) override { timer.start(ms); }
    void stop() override { timer.stop(); }
};
struct Facade {
    Environment& env;
    chart::PublishedFrame published;
    chart::ChartViewController controller{env, env, env, published};
    const chart::ICandleSource* candles = nullptr;
    Timer timer;
    std::vector<std::string> events;
    explicit Facade(Environment& e) : env(e) {
        controller.timeframeSwitch().setHooks([](int) {}, [this](int) {
            events.push_back(controller.flags().limitsDeferred ? "deferred" : "immediate");
            controller.applyGpuLimits();
            limitsChanged();
        });
        QObject::connect(&controller, &chart::ChartViewController::liveRenderTick, &controller,
                         [this] { events.push_back("tick"); }, Qt::DirectConnection);
        QObject::connect(&controller, &chart::ChartViewController::timeframeChanged, &controller,
                         [this](int) { events.insert(events.end(), {"processor", "update", "changed"}); }, Qt::DirectConnection);
        QObject::connect(controller.viewPtr(), &GridViewState::priceInteracted, &controller,
                         [this] { controller.priceCarry().priceInteracted(); }, Qt::DirectConnection);
        controller.priceCarry().setTimerFactoryForTest([this](std::function<void()> fire) {
            timer.fire = std::move(fire);
            return std::make_unique<TimerHandle>(timer);
        });
    }
    void setTimeframe(int next) { controller.setTimeframe(next); }
    void capture() { controller.priceFitter().setCandleSource(candles); controller.priceCarry().capture(); }
    bool applyPriceCarry(double now) { return controller.priceCarry().apply(now); }
    void stop() { controller.priceCarry().stop(); }
    void limitsChanged() { controller.priceCarry().limitsChanged(); }
    void priceInteracted() { emit controller.viewPtr()->priceInteracted(); }
};
struct TimeframeCarryTest : testing::Test {
    Environment env;
    Facade f{env};
    chart::ChartViewController& c = f.controller;
    GridViewState& v = *c.viewPtr();
    void SetUp() override {
        c.session().symbol = "TEST";
        c.session().timeframeMs = 1000;
        c.session().lastTradeMs = 4999;
        v.setViewportSize(800, 400);
        v.setAutoPriceScale(false);
        v.setViewport(0, 10000, 90, 110);
        v.enableAutoScroll(false);
        v.setRasterAnchor(1, .7);
        c.flags().priceKnown = true;
    }
    void window(int64_t start, int64_t end) {
        EXPECT_EQ(v.getVisibleTimeStart(), start);
        EXPECT_EQ(v.getVisibleTimeEnd(), end);
    }
};
TEST_F(TimeframeCarryTest, SameTimeframeIsNoOp) {
    c.glide().gesture = true;
    const auto version = v.getViewportVersion();
    f.setTimeframe(1000);
    EXPECT_TRUE(c.glide().gesture);
    EXPECT_EQ(v.getViewportVersion(), version);
    EXPECT_TRUE(f.events.empty());
}
TEST_F(TimeframeCarryTest, ManualDrawnAnchorAndGlideEnd) {
    c.glide().gesture = true;
    c.glide().time.active = true;
    c.glide().price.active = true;
    f.setTimeframe(2000);
    window(-4000, 16000);
    EXPECT_DOUBLE_EQ(v.rasterAnchor().fracX, .45);
    EXPECT_DOUBLE_EQ(v.rasterAnchor().fracY, .7);
    EXPECT_FALSE(c.glide().gesture);
    EXPECT_FALSE(c.glide().time.active);
    EXPECT_FALSE(c.glide().price.active);
    EXPECT_EQ(f.events, (std::vector<std::string>{"deferred", "processor", "update", "changed"}));
}
TEST_F(TimeframeCarryTest, InvalidDrawnCameraUsesStoredAnchor) {
    env.size.widthLogical = 0;
    f.setTimeframe(2000);
    window(-4000, 16000);
    EXPECT_DOUBLE_EQ(v.rasterAnchor().fracX, .45);
}
TEST_F(TimeframeCarryTest, HistoricalOrMissingAnchorKeepsEnd) {
    for (auto anchor : {int64_t(20000), int64_t(0)}) {
        c.session().timeframeMs = 1000;
        v.setViewport(0, 10000, 90, 110);
        env.anchor = anchor;
        c.session().lastTradeMs = anchor > 0 ? anchor - 1 : 0;
        f.setTimeframe(2000);
        window(-10000, 10000);
        EXPECT_DOUBLE_EQ(v.rasterAnchor().fracX, 1);
    }
}
TEST_F(TimeframeCarryTest, FollowPreservesDrawnColumnAndPadding) {
    v.enableAutoScroll(true);
    const auto version = v.getViewportVersion();
    f.setTimeframe(2000);
    window(-4000, 16000);
    ASSERT_TRUE(c.livePlacement().followPadding());
    EXPECT_EQ(c.livePlacement().followPadding()->padMs, 10000);
    EXPECT_EQ(c.livePlacement().followPadding()->placement, v.placementVersion());
    EXPECT_EQ(v.getViewportVersion(), version + 1);
    EXPECT_EQ(f.events, (std::vector<std::string>{"deferred", "tick", "processor", "update", "changed"}));
}
TEST_F(TimeframeCarryTest, LimitsDeferredAndCappedFollowUsesNominal) {
    v.enableAutoScroll(true);
    env.spans.maxTimeMs = 5000.9;
    const auto version = v.getViewportVersion();
    f.setTimeframe(2000);
    window(3000, 8000);
    EXPECT_FALSE(c.livePlacement().followPadding());
    EXPECT_FALSE(c.flags().limitsDeferred);
    EXPECT_EQ(v.getViewportVersion(), version + 1);
}
TEST_F(TimeframeCarryTest, FollowWithoutDrawnCameraOrLiveAnchor) {
    v.enableAutoScroll(true);
    env.size.widthLogical = 0;
    f.setTimeframe(2000);
    window(-12000, 8000);
    EXPECT_FALSE(c.livePlacement().followPadding());
    c.session().timeframeMs = 1000;
    v.setViewport(0, 10000, 90, 110);
    env.anchor = 0;
    c.session().lastTradeMs = 0;
    f.setTimeframe(2000);
    window(-10000, 10000);
}
TEST_F(TimeframeCarryTest, StartupAndInvalidNextSkipColumns) {
    c.session().timeframeMs = 100;
    f.setTimeframe(2000);
    window(0, 10000);
    ASSERT_FALSE(f.events.empty());
    EXPECT_EQ(f.events.front(), "immediate");
    f.events.clear();
    f.setTimeframe(0);
    window(0, 10000);
    EXPECT_EQ(f.events, (std::vector<std::string>{"processor", "update", "changed"}));
}
TEST_F(TimeframeCarryTest, InvalidWindowSkipsColumnSolve) {
    Environment emptyEnv;
    Facade empty{emptyEnv};
    empty.controller.session().timeframeMs = 1000;
    empty.setTimeframe(2000);
    EXPECT_FALSE(empty.controller.viewPtr()->isTimeWindowValid());
    EXPECT_EQ(empty.events, (std::vector<std::string>{"immediate", "processor", "update", "changed"}));
}
struct Candles final : chart::ICandleSource {
    mutable int reads = 0;
    mutable int64_t interval = 0;
    int64_t expectedInterval = 1000;
    void visibleBars(const QString& symbol, int64_t tf, int64_t start, int64_t end,
                     std::vector<chart::CandleBar>& out) const override {
        EXPECT_EQ(symbol, "TEST"); EXPECT_EQ(tf, expectedInterval); interval = tf; EXPECT_EQ(start, 0); EXPECT_EQ(end, 10000);
        ++reads;
        out.resize(3); out[0].close = 90; out[1].close = 100;
        out[2].close = std::numeric_limits<double>::quiet_NaN();
    }
    int64_t oldestTimeMs(const QString&, int64_t) const override { return 0; }
};
TEST_F(TimeframeCarryTest, SymbolCarryUsesNewestUsableCandle) {
    Candles candles;
    f.candles = &candles;
    env.mid = 0;
    f.capture();
    ASSERT_TRUE(c.priceFitter().priceCarry());
    EXPECT_DOUBLE_EQ(c.priceFitter().priceCarry()->spanRatio, .2);
    EXPECT_EQ(candles.reads, 1);
    c.flags().priceKnown = false;
    f.capture();
    EXPECT_FALSE(c.priceFitter().priceCarry());
}
TEST_F(TimeframeCarryTest, SymbolCarryOffOnAndUnpricedSwitch) {
    f.capture();
    ASSERT_TRUE(c.priceFitter().priceCarry());
    EXPECT_DOUBLE_EQ(c.priceFitter().priceCarry()->spanRatio, .2);
    EXPECT_DOUBLE_EQ(c.priceFitter().priceCarry()->heightFrac, .5);
    c.flags().reseedPrice = true;
    env.mid = 0;
    f.capture();
    ASSERT_TRUE(c.priceFitter().priceCarry());
    EXPECT_DOUBLE_EQ(c.priceFitter().priceCarry()->spanRatio, .2);
    v.setAutoPriceScale(true);
    f.capture();
    EXPECT_FALSE(c.priceFitter().priceCarry());
}
TEST_F(TimeframeCarryTest, CarryTimerUsesLatestPriceOnceAndStopsPerSymbol) {
    f.capture();
    ASSERT_TRUE(f.applyPriceCarry(200));
    EXPECT_DOUBLE_EQ(v.getMinPrice(), 180);
    EXPECT_DOUBLE_EQ(v.getMaxPrice(), 220);
    EXPECT_TRUE(c.priceFitter().priceCarry());
    f.timer.advance(1999);
    env.mid = 300;
    ASSERT_TRUE(f.applyPriceCarry(250));
    EXPECT_EQ(f.timer.starts, 1);
    f.timer.advance(1);
    EXPECT_DOUBLE_EQ(v.getMinPrice(), 270);
    EXPECT_DOUBLE_EQ(v.getMaxPrice(), 330);
    EXPECT_FALSE(c.priceFitter().priceCarry());
    EXPECT_FALSE(f.timer.active);
    c.priceFitter().priceCarry() = chart::PriceFitter::PriceCarry{.2, .5};
    f.applyPriceCarry(100);
    f.stop();
    f.timer.advance(2000);
    EXPECT_TRUE(c.priceFitter().priceCarry());
}
TEST_F(TimeframeCarryTest, LimitsAndPriceInteractionOwnPendingCarry) {
    f.capture();
    f.applyPriceCarry(200);
    env.current = true;
    env.mid = 300;
    c.flags().limitsDeferred = true;
    f.limitsChanged();
    EXPECT_DOUBLE_EQ(v.getMinPrice(), 180);
    c.flags().limitsDeferred = false;
    c.flags().reseedPrice = true;
    f.limitsChanged();
    EXPECT_TRUE(c.priceFitter().priceCarry());
    c.flags().reseedPrice = false;
    f.limitsChanged();
    EXPECT_DOUBLE_EQ(v.getMinPrice(), 270);
    EXPECT_FALSE(c.priceFitter().priceCarry());
    EXPECT_FALSE(f.timer.active);
    f.capture();
    f.priceInteracted();
    EXPECT_FALSE(c.priceFitter().priceCarry());
}
TEST_F(TimeframeCarryTest, InvalidCarryAndTimerReseedGuard) {
    f.capture();
    EXPECT_FALSE(f.applyPriceCarry(0));
    EXPECT_FALSE(f.applyPriceCarry(std::numeric_limits<double>::infinity()));
    f.applyPriceCarry(200);
    c.flags().reseedPrice = true;
    f.timer.advance(2000);
    EXPECT_TRUE(c.priceFitter().priceCarry());
    EXPECT_DOUBLE_EQ(v.getMinPrice(), 180);
    ASSERT_TRUE(c.priceFitter().priceCarry());
    c.priceFitter().priceCarry()->spanRatio = -1;
    EXPECT_FALSE(f.applyPriceCarry(100));
    EXPECT_FALSE(c.priceFitter().priceCarry());
}
} // namespace

TEST_F(TimeframeCarryTest, CandleFallbackKeepsNonWholeSecondMilliseconds) {
    Candles candles;
    f.candles = &candles;
    env.mid = 0;
    for (int64_t tf : {100, 1500}) {
        c.session().timeframeMs = tf;
        candles.expectedInterval = tf;
        f.capture();
        EXPECT_EQ(candles.interval, tf);
        ASSERT_TRUE(c.priceFitter().priceCarry());
        EXPECT_DOUBLE_EQ(c.priceFitter().priceCarry()->spanRatio, .2);
    }
}
