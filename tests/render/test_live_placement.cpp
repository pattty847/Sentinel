#include "chart/core/LivePlacement.hpp"
#include "chart/core/ChartViewController.hpp"
#include "chart/core/PublishedFrame.hpp"
#include <gtest/gtest.h>
#include <limits>
#include <utility>
#include <vector>

namespace {
struct Environment final : chart::IChartEnvironment, chart::ITickOracle, chart::IHeatmapViewSink {
    chart::SurfaceFacts size{800, 400, 1};
    int64_t anchor = 10000;
    int syncs = 0;
    bool attached() const override { return true; }
    chart::SurfaceFacts surface() const override { return size; }
    double drawnTick() const override { return 1; }
    double priceScale() const override { return 1; }
    bool manualTick() const override { return false; }
    bool hasCurrentResolution() const override { return false; }
    bool priceScaleCurrent() const override { return true; }
    chart::SpanLimits limits() const override { return {}; }
    int64_t liveAnchorMs() const override { return anchor; }
    int64_t liveOpenEndMs() const override { return anchor; }
    int64_t oldestAvailableMs() const override { return 0; }
    double recentMidPrice() const override { return 100; }
    std::optional<heatmap::gpu::ViewWindow> restView() const override { return std::nullopt; }
    int64_t predictTickUnits(const heatmap::gpu::ViewWindow&) const override { return 1; }
    bool buildsTick(int64_t, const heatmap::gpu::ViewWindow&) const override { return true; }
    int64_t autoTickState() const override { return 1; }
    heatmap::AutoTickParams autoTickParams() const override { return {1, .1}; }
    bool setAutoPriceTick(std::optional<int64_t>) override { return false; }
    void setView(const heatmap::gpu::ViewWindow&, bool) override { ++syncs; }
};
using Placement = chart::LivePlacement;
struct Timer final : chart::ILivePlacementTimer {
    bool active = false;
    int interval = 0;
    std::function<void()> fire;
    void setInterval(int ms) override { interval = ms; }
    bool isActive() const override { return active; }
    void start() override { active = true; }
    void stop() override { active = false; }
};
struct LivePlacementTest : testing::Test {
    Environment env;
    chart::PublishedFrame published;
    chart::ChartViewController controller{env, env, env, published};
    Placement& live = controller.livePlacement();
    Timer* timer = nullptr;
    GridViewState& view = *controller.viewPtr();
    std::vector<std::string> events;
    void SetUp() override {
        controller.session().symbol = "TEST";
        controller.session().timeframeMs = 1000;
        controller.session().lastTradeMs = 9999;
        view.setViewportSize(800, 400);
        view.setAutoPriceScale(false);
        live.setTimingForTest([] { return 10000; }, [this](std::function<void()> timeout) {
            auto owned = std::make_unique<Timer>();
            timer = owned.get();
            timer->fire = std::move(timeout);
            return owned;
        });
        QObject::connect(&live, &Placement::liveRenderTick, &live, [this] { events.push_back("tick"); }, Qt::DirectConnection);
        QObject::connect(&live, &Placement::updateRequested, &live, [this] { events.push_back("update"); }, Qt::DirectConnection);
        QObject::connect(&live, &Placement::autoScrollEnabledChanged, &live, [this] { events.push_back("changed"); }, Qt::DirectConnection);
    }
    void window(qint64 start, qint64 end) {
        EXPECT_EQ(view.getVisibleTimeStart(), start);
        EXPECT_EQ(view.getVisibleTimeEnd(), end);
    }
};
TEST_F(LivePlacementTest, FollowTarget) {
    view.setViewport(1000, 11000, 90, 110);
    view.enableAutoScroll(true);
    controller.session().lastTradeMs = 11999;
    live.followGpuLiveTo(12000);
    window(3000, 13000);
    EXPECT_EQ(events, (std::vector<std::string>{"tick"}));
    controller.session().lastTradeMs = 10999;
    live.followGpuLiveTo(11000); // never step backwards
    window(3000, 13000);
    view.enableAutoScroll(false);
    controller.session().lastTradeMs = 19999;
    live.followGpuLiveTo(20000);
    window(3000, 13000);
    EXPECT_EQ(events.size(), 1);
}
TEST_F(LivePlacementTest, SeedViewport) {
    live.seedGpuViewport(99, 101);
    window(-82000, 18000);
    EXPECT_DOUBLE_EQ(view.getMinPrice(), 100 - 102.4 * .5);
    EXPECT_DOUBLE_EQ(view.getMaxPrice(), 100 + 102.4 * .5);
    EXPECT_TRUE(controller.flags().priceKnown);
    EXPECT_TRUE(controller.flags().pristine);
    EXPECT_FALSE(controller.flags().reseedPrice);
    EXPECT_EQ(env.syncs, 1);
    EXPECT_EQ(events, (std::vector<std::string>{"update"}));
    live.seedGpuViewport(199, 201);
    EXPECT_EQ(events.size(), 1);
}
TEST_F(LivePlacementTest, ReturnToLive) {
    view.setViewport(30000, 40000, 90, 110);
    view.setRasterAnchor(.3, .7);
    live.returnGpuToLive();
    window(1000, 11000);
    EXPECT_DOUBLE_EQ(view.rasterAnchor().fracX, 1);
    EXPECT_DOUBLE_EQ(view.rasterAnchor().fracY, .7);
    EXPECT_EQ(env.syncs, 1);
    EXPECT_EQ(events, (std::vector<std::string>{"tick"}));
}
TEST_F(LivePlacementTest, BootstrapTimeView) {
    live.bootstrapGpuTimeView();
    window(-82000, 18000);
    EXPECT_FALSE(controller.flags().priceKnown);
    EXPECT_TRUE(controller.flags().pristine);
    EXPECT_EQ(view.getMinPrice(), 0);
    EXPECT_EQ(view.getMaxPrice(), 1);
    EXPECT_EQ(env.syncs, 1);
    live.bootstrapGpuTimeView();
    EXPECT_EQ(env.syncs, 1);
}
TEST_F(LivePlacementTest, BootstrapTimer) {
    controller.session().timeframeMs = 60000;
    env.anchor = 0;
    live.bootstrapGpuTimeView();
    ASSERT_NE(timer, nullptr);
    EXPECT_EQ(timer->interval, 50);
    EXPECT_TRUE(timer->active);
    EXPECT_FALSE(view.isTimeWindowValid());
    auto* firstTimer = timer;
    live.bootstrapGpuTimeView();
    EXPECT_EQ(timer, firstTimer);
    env.anchor = 10001;
    timer->fire();
    window(-5460000, 540000);
    EXPECT_FALSE(timer->active);
}
TEST_F(LivePlacementTest, InitialSpan) {
    EXPECT_EQ(live.gpuInitialSpanMs(800), 100000);
    EXPECT_EQ(live.gpuInitialSpanMs(0), 100000);
    EXPECT_EQ(live.gpuInitialSpanMs(1), 16000);
    controller.initialViewSettings().columnPx = 10;
    EXPECT_EQ(live.gpuInitialSpanMs(999), 99000);
}
TEST_F(LivePlacementTest, FollowPadding) {
    view.setViewport(1000, 11000, 90, 110);
    view.enableAutoScroll(true);
    live.followPadding() = Placement::FollowPadding{4000, 1000, 11000, 1000, view.placementVersion()};
    env.anchor = 12000;
    controller.session().lastTradeMs = 11999;
    live.followGpuLive();
    window(6000, 16000);
    EXPECT_EQ(live.followPadding()->startMs, 6000);
    EXPECT_EQ(live.followPadding()->endMs, 16000);
    view.setViewport(6000, 16000, 90, 110); // explicit placement invalidates retained padding
    controller.session().lastTradeMs = 17999;
    live.followGpuLiveTo(18000);
    window(9000, 19000);
}
TEST_F(LivePlacementTest, TradeAndBookSeed) {
    live.onTrade("OTHER", 500, 10000);
    EXPECT_FALSE(view.isTimeWindowValid());
    live.onTrade("TEST", 100, 9999);
    EXPECT_EQ(controller.session().lastTrade, 100);
    EXPECT_DOUBLE_EQ(view.getMinPrice(), 48.8);
    live.setLiveBookTop(199, 201);
    EXPECT_EQ(controller.session().bookMid, 200);
    EXPECT_DOUBLE_EQ(view.getMinPrice(), 48.8); // only the first price seeds
    controller.flags().reseedPrice = true;
    live.setLiveBookTop(299, 301);
    EXPECT_DOUBLE_EQ(view.getMinPrice(), 248.8);
    live.onTrade("TEST", std::numeric_limits<double>::quiet_NaN(), 20000);
    EXPECT_EQ(controller.session().lastTrade, 100);
}
// A non-finite or crossed book top neither seeds nor consumes a symbol switch's pending
// carry: the viewport, the reseed state and the carry wait for a valid top.
TEST_F(LivePlacementTest, ANonFiniteTopKeepsThePendingCarryAndReseed) {
    view.setViewport(1000, 11000, 90, 110);
    controller.flags().markPriceKnown();
    controller.flags().reseedPrice = true; // a symbol switch
    controller.priceFitter().priceCarry() = chart::PriceFitter::PriceCarry{0.02, 0.25};
    const auto version = view.getViewportVersion();
    constexpr double nan = std::numeric_limits<double>::quiet_NaN(), inf = std::numeric_limits<double>::infinity();
    for (const auto& [bid, ask] : std::vector<std::pair<double, double>>{{nan, 1}, {1, nan}, {inf, inf}, {201, 199}}) {
        live.setLiveBookTop(bid, ask);
        EXPECT_EQ(view.getViewportVersion(), version) << bid << " " << ask;
        EXPECT_TRUE(controller.flags().reseedPrice);
        ASSERT_TRUE(controller.priceFitter().priceCarry().has_value()) << "the carry is still pending";
        EXPECT_EQ(controller.session().bookMid, 0.0);
    }
    EXPECT_DOUBLE_EQ(view.getMinPrice(), 90);
    live.setLiveBookTop(199, 201);
    EXPECT_FALSE(controller.flags().reseedPrice);
    EXPECT_NE(view.getViewportVersion(), version);
    EXPECT_NEAR(view.getMaxPrice() - view.getMinPrice(), 4.0, 1e-9) << "the carried span ratio at mid 200";
    EXPECT_NEAR(view.getMinPrice(), 199.0, 1e-9);
}
TEST_F(LivePlacementTest, PristineResize) {
    live.seedGpuViewport(99, 101);
    live.resize(400);
    window(-32000, 18000);
    controller.flags().pristine = false;
    live.resize(100);
    window(-32000, 18000);
}
TEST_F(LivePlacementTest, EnableAutoScroll) {
    view.setViewport(30000, 40000, 90, 110);
    live.enableAutoScroll(true);
    window(1000, 11000);
    EXPECT_TRUE(view.isAutoScrollEnabled());
    EXPECT_EQ(events, (std::vector<std::string>{"tick", "update", "changed"}));
    events.clear();
    live.enableAutoScroll(false);
    EXPECT_FALSE(view.isAutoScrollEnabled());
    EXPECT_EQ(events, (std::vector<std::string>{"update", "changed"}));
}
TEST_F(LivePlacementTest, SeedMatchesFollowAtBucketBoundary) {
    live.seedGpuViewport(99, 101);
    const qint64 seedEnd = view.getVisibleTimeEnd();
    const qint64 span = live.gpuInitialSpanMs(800);
    view.setViewport(-span, 0, 90, 110);
    view.enableAutoScroll(true);
    controller.session().lastTradeMs = 9999;
    live.followGpuLiveTo(10000);
    EXPECT_EQ(seedEnd, view.getVisibleTimeEnd());
    EXPECT_EQ(seedEnd, 18000);
}
TEST_F(LivePlacementTest, BootstrapMatchesFollowAtBucketBoundary) {
    live.bootstrapGpuTimeView();
    const qint64 bootstrapEnd = view.getVisibleTimeEnd();
    const qint64 span = live.gpuInitialSpanMs(800);
    view.setViewport(-span, 0, 90, 110);
    view.enableAutoScroll(true);
    live.followGpuLiveTo(env.anchor);
    EXPECT_EQ(bootstrapEnd, view.getVisibleTimeEnd());
}
TEST_F(LivePlacementTest, NonBoundarySeedAndBootstrapAreUnchanged) {
    controller.session().lastTradeMs = 10000;
    live.setTimingForTest([] { return 10001; }, {});
    live.seedGpuViewport(99, 101);
    window(-81000, 19000);
    chart::ChartViewController bootstrap{env, env, env, published};
    bootstrap.session().symbol = "TEST";
    bootstrap.session().timeframeMs = 1000;
    bootstrap.session().lastTradeMs = 10000;
    env.anchor = 10001;
    bootstrap.livePlacement().bootstrapGpuTimeView();
    EXPECT_EQ(bootstrap.viewPtr()->getVisibleTimeStart(), -81000);
    EXPECT_EQ(bootstrap.viewPtr()->getVisibleTimeEnd(), 19000);
}
TEST_F(LivePlacementTest, LiveEndHelpers) {
    EXPECT_EQ(Placement::liveEndCeil(10000, 1000), 10000);
    EXPECT_EQ(Placement::liveEndCeil(10001, 1000), 11000);
    EXPECT_EQ(Placement::liveEndCeil(-1000, 1000), -1000);
    EXPECT_EQ(Placement::liveEndCeil(-1001, 1000), -1000);
}
} // namespace
