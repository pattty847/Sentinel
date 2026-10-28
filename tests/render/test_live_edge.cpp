#include "chart/core/LiveEdge.hpp"
#include "chart/core/ChartViewController.hpp"
#include "chart/core/PublishedFrame.hpp"
#include <gtest/gtest.h>
#include <limits>

namespace {
struct Environment final : chart::IChartEnvironment, chart::ITickOracle, chart::IHeatmapViewSink {
    int64_t anchor = 180000;
    chart::SurfaceFacts size{800, 400, 1};
    bool present = true;
    mutable int cameraReads = 0;
    bool attached() const override { return present; }
    chart::SurfaceFacts surface() const override { return size; }
    double drawnTick() const override { ++cameraReads; return 1; }
    double priceScale() const override { return 1; }
    bool manualTick() const override { return true; }
    bool hasCurrentResolution() const override { return true; }
    bool priceScaleCurrent() const override { return true; }
    chart::SpanLimits limits() const override { return {}; }
    int64_t liveAnchorMs() const override { return anchor; }
    int64_t liveOpenEndMs() const override { return anchor; }
    int64_t oldestAvailableMs() const override { return 120000; }
    double recentMidPrice() const override { return 100; }
    std::optional<heatmap::gpu::ViewWindow> restView() const override { return std::nullopt; }
    int64_t predictTickUnits(const heatmap::gpu::ViewWindow&) const override { return 1; }
    bool buildsTick(int64_t, const heatmap::gpu::ViewWindow&) const override { return true; }
    int64_t autoTickState() const override { return 1; }
    heatmap::AutoTickParams autoTickParams() const override { return {1, .1}; }
    bool setAutoPriceTick(std::optional<int64_t>) override { return false; }
    void setView(const heatmap::gpu::ViewWindow&, bool) override {}
};
struct Timer final : chart::ILivePlacementTimer {
    bool active = false;
    void setInterval(int) override {}
    bool isActive() const override { return active; }
    void start() override { active = true; }
    void stop() override { active = false; }
};
struct LiveEdgeTest : testing::TestWithParam<int64_t> {
    Environment env;
    chart::PublishedFrame published;
    chart::ChartViewController c{env, env, env, published};
    GridViewState& view = *c.viewPtr();
    chart::LivePlacement& live = c.livePlacement();
    int64_t clock = 170000;
    int64_t tf() const { return GetParam(); }
    void SetUp() override {
        c.session().setSymbol("TEST");
        c.session().timeframeMs = tf();
        c.setTimeframe(int(tf())); // an explicitly selected interval, including 100 ms
        view.setViewportSize(800, 400);
        view.setAutoPriceScale(false);
        view.enableAutoScroll(true);
        live.setTimingForTest([this] { return clock; }, [](std::function<void()>) { return std::make_unique<Timer>(); });
    }
    void trade(int64_t time, const std::string& symbol = "TEST", double price = 100) {
        live.onTrade(symbol, price, time);
    }
    int64_t end() const { return view.getVisibleTimeEnd(); }
    int64_t span() const { return end() - view.getVisibleTimeStart(); }
    int64_t firstBucketEnd() const { return tf() == 100 ? 137400 : 138000; }
    int64_t padded(int64_t bucketEnd) const {
        return bucketEnd + std::max<int64_t>(tf(), int64_t(span() * .08));
    }
    void placed() {
        view.setViewport(120000, 120000 + 10 * tf(), 90, 110);
        c.flags().markPriceKnown();
    }
};

TEST(LiveEdge, BoundaryConversionAndMinuteAuthority) {
    EXPECT_EQ(chart::LiveEdge::bucketEnd(1000, 1000), 2000);
    EXPECT_EQ(chart::LiveEdge::bucketEnd(1001, 1000), 2000);
    EXPECT_EQ(chart::LiveEdge::bucketEnd(-1000, 1000), 0);
    EXPECT_EQ(chart::LiveEdge::bucketEnd(-1001, 1000), -1000);
    chart::ChartSession session;
    Environment env;
    session.lastTradeMs = 137321;
    EXPECT_EQ(chart::LiveEdge::anchorMs(session, env, 1000), 137321);
    EXPECT_EQ(chart::LiveEdge::endMs(session, env, 1000), 138000);
    EXPECT_EQ(chart::LiveEdge::anchorMs(session, env, 60000), 180000);
    EXPECT_EQ(chart::LiveEdge::endMs(session, env, 60000), 180000);
    env.anchor = 180001;
    EXPECT_EQ(chart::LiveEdge::endMs(session, env, 60000), 240000);
}

TEST_P(LiveEdgeTest, NoSymbolWaitsAndNoAvailabilityStillAllowsProvisionalPlacement) {
    c.session().setSymbol("");
    live.bootstrapGpuTimeView();
    EXPECT_FALSE(view.isTimeWindowValid());
    c.session().setSymbol("TEST");
    env.anchor = 0;
    live.bootstrapGpuTimeView();
    EXPECT_EQ(end(), padded(clock + tf()));
    EXPECT_FALSE(c.commands().resetView()) << "a provisional view is not a trade anchor";
    trade(137321);
    EXPECT_EQ(end(), padded(firstBucketEnd()));
}

TEST_P(LiveEdgeTest, AcceptedTradesFollowTheirBucketAndRespectGates) {
    placed();
    trade(137321);
    EXPECT_EQ(end(), padded(firstBucketEnd()));
    trade(140000); // exactly on a boundary belongs to the next bucket
    EXPECT_EQ(end(), padded(140000 + tf()));
    const auto held = end();
    trade(137000);
    trade(150000, "OTHER");
    trade(150000, "TEST", std::numeric_limits<double>::quiet_NaN());
    EXPECT_EQ(c.session().lastTradeMs, 140000);
    EXPECT_EQ(end(), held);
    view.enableAutoScroll(false);
    trade(155000);
    EXPECT_EQ(c.session().lastTradeMs, 155000);
    EXPECT_EQ(end(), held);
    live.enableAutoScroll(true);
    EXPECT_EQ(end(), padded(155000 + tf()));
    view.handlePanStart({0, 0});
    trade(160000);
    EXPECT_EQ(end(), padded(155000 + tf()));
}

TEST_P(LiveEdgeTest, IdleOrDisconnectedHoldsThenOneTradeCatchesUp) {
    placed();
    trade(137321);
    const auto held = end();
    const auto version = view.getViewportVersion();
    clock += 50000;
    env.anchor += 60000;
    live.bootstrapGpuTimeView();
    live.followGpuLive(); // availability or a heatmap signal cannot run ahead
    EXPECT_EQ(end(), held);
    EXPECT_EQ(view.getViewportVersion(), version);
    env.present = false;
    live.followGpuLive();
    EXPECT_EQ(end(), held);
    env.present = true;
    trade(190000);
    EXPECT_EQ(end(), padded(190000 + tf()));
    EXPECT_EQ(view.getViewportVersion(), version + 1);
}

TEST_P(LiveEdgeTest, ReturnDefaultAndTimeFitUseTradeEndAfterPan) {
    placed();
    trade(137321);
    c.commands().beginPanAt(10, 10);
    c.commands().updatePanAt(110, 10);
    c.commands().endPanAt();
    live.enableAutoScroll(true);
    EXPECT_EQ(end(), padded(firstBucketEnd()));
    view.enableAutoScroll(false);
    view.setViewport(0, 10 * tf(), 90, 110);
    chart::ViewportRequest request;
    request.fit = "default";
    EXPECT_TRUE(c.commands().applyViewportRequest(request).isEmpty());
    EXPECT_EQ(end(), padded(firstBucketEnd()));
    EXPECT_TRUE(view.isAutoScrollEnabled());
    const auto fit = c.priceFitter().gpuFitTimeWindow();
    ASSERT_TRUE(fit);
    const auto fitSpan = fit->second - fit->first;
    EXPECT_EQ(fit->second, firstBucketEnd() +
              std::max<int64_t>(tf(), int64_t(fitSpan * .08)));
}

TEST_P(LiveEdgeTest, AvailabilityBeforeTradeIsProvisionalAndFirstTradeCanMoveBack) {
    live.bootstrapGpuTimeView();
    ASSERT_TRUE(view.isTimeWindowValid());
    EXPECT_EQ(c.session().lastTradeMs, 0);
    EXPECT_EQ(chart::LiveEdge::anchorMs(c.session(), env, tf()), 0);
    EXPECT_EQ(end(), padded(clock + tf()));
    const auto provisional = end();
    clock += 100000;
    live.bootstrapGpuTimeView();
    live.followGpuLive();
    EXPECT_EQ(end(), provisional);
    trade(137321); // delayed relative to both availability and the provisional clock
    EXPECT_LT(end(), provisional);
    EXPECT_EQ(end(), padded(firstBucketEnd()));
    trade(140000);
    EXPECT_EQ(end(), padded(140000 + tf()));
}

TEST_P(LiveEdgeTest, BookBeforeTradeAndSymbolReplacementDoNotKeepTheOldEdge) {
    live.setLiveBookTop(99, 101);
    EXPECT_EQ(c.session().lastTradeMs, 0);
    EXPECT_EQ(end(), padded(clock + tf()));
    trade(137321);
    EXPECT_EQ(end(), padded(firstBucketEnd()));
    c.session().setSymbol("NEW"); // the shell uses this same reset before changing the source
    c.flags().reseedPrice = true;
    EXPECT_EQ(c.session().lastTradeMs, 0);
    EXPECT_FALSE(c.session().bookMidValid);
    EXPECT_EQ(chart::LiveEdge::endMs(c.session(), env, tf()), 0);
    const auto oldEnd = end();
    trade(200000); // queued old-symbol trade
    live.followGpuLive();
    live.setLiveBookTop(199, 201);
    EXPECT_EQ(end(), oldEnd);
    trade(125000, "NEW", 200);
    EXPECT_EQ(end(), padded(125000 + tf()));
}

TEST_P(LiveEdgeTest, FirstTradePlacesFromEventTimeAndFollowOffKeepsProvisionalView) {
    trade(137321);
    EXPECT_EQ(end(), padded(firstBucketEnd()));
    c.session().setSymbol("NEW");
    view.enableAutoScroll(false);
    const auto held = end();
    trade(125000, "NEW");
    EXPECT_EQ(end(), held);
    live.enableAutoScroll(true);
    EXPECT_EQ(end(), padded(125000 + tf()));
}

TEST_P(LiveEdgeTest, TimeframeRoundTripCapturesBothLiveColumns) {
    placed();
    trade(137321); // well inside [120000,180000), not at the minute edge
    for (bool follow : {true, false}) {
        view.enableAutoScroll(follow);
        const auto before = c.camera().rasterCameraNow(false);
        const double oldCentre = firstBucketEnd() - tf() * .5;
        const double x = before.xDev(oldCentre);
        c.setTimeframe(60000);
        const auto minute = c.camera().rasterCameraNow(false);
        EXPECT_NEAR(minute.xDev(150000), x, 1.0);
        c.setTimeframe(int(tf()));
        EXPECT_NEAR(c.camera().rasterCameraNow(false).xDev(oldCentre), x, 1.0);
    }
}

TEST_P(LiveEdgeTest, MinuteFollowRemainsHeatmapDriven) {
    c.session().timeframeMs = 60000;
    view.setViewport(-420000, 180000, 90, 110);
    c.flags().markPriceKnown();
    clock = 240000;
    trade(237321);
    EXPECT_EQ(end(), 180000); // trades never drive minute follow
    live.followGpuLiveTo(240000);
    EXPECT_EQ(end(), 300000); // exact heatmap boundary stays 240000, then 60000 padding
    EXPECT_EQ(c.priceFitter().gpuLiveEndMs(span()), 240000); // anchor still 180000
}

TEST_P(LiveEdgeTest, CappedSwitchUsesTradeEndAndNoSameBucketShift) {
    env.size.widthLogical = 8000;
    view.setViewportSize(8000, 400);
    c.session().timeframeMs = 60000;
    view.setViewport(0, 240000, 90, 110);
    c.flags().markPriceKnown();
    trade(137321);
    ASSERT_EQ(c.camera().restCameraNow(false).colPx, 2000);
    c.setTimeframe(int(tf()));
    EXPECT_EQ(end(), padded(firstBucketEnd()));
    const auto camera = c.camera().restCameraNow(false);
    ASSERT_EQ(camera.colPx, tf()); // one millisecond per pixel caps the column width
    const double padPx = double(end() - firstBucketEnd()) * camera.colPx / tf();
    EXPECT_EQ(camera.xDev(firstBucketEnd() - tf() * .5), 8000.0 - padPx - camera.colPx * .5);
    const auto beforeEnd = end();
    trade(137322);
    EXPECT_EQ(end(), beforeEnd);
    c.setTimeframe(60000);
    EXPECT_EQ(end(), 240000); // the minute leg still follows the heatmap anchor
    EXPECT_EQ(c.camera().restCameraNow(false).colPx, 2000);
}

TEST_P(LiveEdgeTest, SameBucketTradesDoNoCameraOrViewportWork) {
    placed();
    trade(137321);
    const int reads = env.cameraReads;
    const auto version = view.getViewportVersion();
    int writes = 0;
    QObject::connect(&view, &GridViewState::viewportChanged, &c, [&] { ++writes; });
    trade(137322, "TEST", 101);
    EXPECT_EQ(c.session().lastTradeMs, 137322);
    EXPECT_EQ(c.session().lastTrade, 101);
    EXPECT_EQ(env.cameraReads, reads);
    EXPECT_EQ(view.getViewportVersion(), version);
    EXPECT_EQ(writes, 0);
    trade(140000);
    EXPECT_GT(env.cameraReads, reads);
    EXPECT_EQ(writes, 1);
}

TEST_P(LiveEdgeTest, AutomaticFirstTradeKeepsPristineAndResizePlacement) {
    QObject::connect(&view, &GridViewState::viewportChanged, &c.commands(), &chart::ViewCommands::onViewportChanged);
    live.bootstrapGpuTimeView();
    ASSERT_TRUE(c.flags().pristine);
    trade(137321);
    EXPECT_TRUE(c.flags().pristine);
    EXPECT_EQ(end(), padded(firstBucketEnd()));
    const auto beforeEnd = end();
    live.resize(400);
    EXPECT_EQ(span(), 50 * tf());
    EXPECT_EQ(end(), beforeEnd);
    view.setViewport(0, 10 * tf(), 90, 110);
    ASSERT_FALSE(c.flags().pristine);
    live.returnGpuToLive();
    EXPECT_FALSE(c.flags().pristine) << "the user's return remains an explicit placement";
}

TEST_P(LiveEdgeTest, FutureTradeCannotPoisonTheAnchorOrConsumeTheFirstTrade) {
    live.bootstrapGpuTimeView();
    const auto provisional = end();
    trade(clock + chart::LivePlacement::kMaxTradeLeadMs + 1, "TEST", 999);
    EXPECT_EQ(c.session().lastTradeMs, 0);
    EXPECT_EQ(c.session().lastTrade, 0);
    EXPECT_EQ(end(), provisional);
    trade(137321);
    EXPECT_EQ(end(), padded(firstBucketEnd()));
    trade(std::numeric_limits<int64_t>::max());
    EXPECT_EQ(c.session().lastTradeMs, 137321);
    trade(clock); // a valid trade still catches up after the malformed one
    EXPECT_EQ(end(), padded(clock + tf()));
    trade(clock + chart::LivePlacement::kMaxTradeLeadMs); // inclusive limit
    EXPECT_EQ(c.session().lastTradeMs, clock + chart::LivePlacement::kMaxTradeLeadMs);
    clock += 6000;
    trade(clock);
    EXPECT_EQ(end(), padded(clock + tf()));
}

TEST_P(LiveEdgeTest, NoTradeMinuteToSubMinuteHoldsUntilFirstTradeReplacesIt) {
    c.session().timeframeMs = 60000;
    view.setViewport(-420000, 240000, 90, 110);
    c.flags().markPriceKnown();
    c.setTimeframe(int(tf()));
    EXPECT_EQ(end(), 240000) << "no sub-minute authority yet: keep the previous end";
    const auto expectedSpan = 11 * tf();
    EXPECT_EQ(span(), expectedSpan);
    trade(137321);
    EXPECT_EQ(end(), padded(firstBucketEnd()));
    EXPECT_EQ(span(), expectedSpan);
}

TEST_P(LiveEdgeTest, StartupDefaultToPersistedMinuteMatchesMinuteBootstrap) {
    c.session().timeframeMs = 100;
    c.session().timeframeChosen = false;
    live.bootstrapGpuTimeView();
    live.setLiveBookTop(99, 101); // even a book cannot choose the implicit time window
    EXPECT_FALSE(view.isTimeWindowValid());
    c.setTimeframe(60000);
    live.bootstrapGpuTimeView(); // the injected bootstrap timer's next tick
    EXPECT_EQ(view.getVisibleTimeStart(), -5340000);
    EXPECT_EQ(end(), 660000); // heatmap end 180000 + 8% of 100 minutes
    EXPECT_TRUE(c.flags().pristine);
    chart::ChartViewController baseline{env, env, env, published};
    baseline.session().setSymbol("TEST");
    baseline.session().timeframeMs = 60000;
    baseline.livePlacement().bootstrapGpuTimeView();
    EXPECT_EQ(view.getVisibleTimeStart(), baseline.viewPtr()->getVisibleTimeStart());
    EXPECT_EQ(end(), baseline.viewPtr()->getVisibleTimeEnd());
}

TEST_P(LiveEdgeTest, PristineProvisionalTimeDoesNotExpressChosenColumns) {
    live.bootstrapGpuTimeView();
    const auto beforeStart = view.getVisibleTimeStart(), beforeEnd = end();
    c.setTimeframe(2000);
    EXPECT_EQ(view.getVisibleTimeStart(), beforeStart);
    EXPECT_EQ(end(), beforeEnd);
}

INSTANTIATE_TEST_SUITE_P(SubMinute, LiveEdgeTest, testing::Values(100LL, 1000LL));
} // namespace
