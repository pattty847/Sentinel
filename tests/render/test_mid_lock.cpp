// Lock to mid (chart::MidLock): the spring on the committed price window and the
// owner's route rules (2026-10-28, D1-D7). Qt-free: chart core + QtCore only.
#include "chart/core/ChartViewController.hpp"
#include "chart/core/PublishedFrame.hpp"
#include <gtest/gtest.h>
#include <cmath>
#include <functional>
#include <memory>
#include <vector>

namespace {
struct Environment final : chart::IChartEnvironment, chart::ITickOracle, chart::IHeatmapViewSink {
    chart::SurfaceFacts size{800, 400, 1};
    double tick = 1;
    int64_t anchor = 100000;
    bool attached() const override { return true; }
    chart::SurfaceFacts surface() const override { return size; }
    double drawnTick() const override { return tick; }
    double priceScale() const override { return 1; }
    bool manualTick() const override { return true; }
    bool hasCurrentResolution() const override { return true; }
    bool priceScaleCurrent() const override { return true; }
    chart::SpanLimits limits() const override { return {}; }
    int64_t liveAnchorMs() const override { return anchor; }
    int64_t liveOpenEndMs() const override { return anchor; }
    int64_t oldestAvailableMs() const override { return 10000; }
    double recentMidPrice() const override { return 100; }
    std::optional<heatmap::gpu::ViewWindow> restView() const override { return std::nullopt; }
    int64_t predictTickUnits(const heatmap::gpu::ViewWindow&) const override { return 1; }
    bool buildsTick(int64_t, const heatmap::gpu::ViewWindow&) const override { return true; }
    int64_t autoTickState() const override { return 1; }
    heatmap::AutoTickParams autoTickParams() const override { return {1, .1}; }
    bool setAutoPriceTick(std::optional<int64_t>) override { return false; }
    void setView(const heatmap::gpu::ViewWindow&, bool) override {}
};

struct ManualTimer final : chart::IZoomTimer {
    bool active = false;
    bool isActive() const override { return active; }
    void start(int) override { active = true; }
    void stop() override { active = false; }
};

struct MidLockTest : testing::Test {
    Environment env;
    chart::PublishedFrame published;
    chart::ChartViewController c{env, env, env, published};
    GridViewState& view = *c.viewPtr();
    chart::MidLock& lock = c.midLock();
    qint64 now = 1000;
    int polishes = 0;
    void SetUp() override {
        c.session().symbol = "TEST";
        c.session().timeframeMs = 1000;
        view.setViewportSize(800, 400);
        view.setAutoPriceScale(false);
        view.enableAutoScroll(false);
        view.setViewport(0, 100000, 50, 150); // 100 price on 400 device px: 4 px per row of 1
        c.flags().markPriceKnown();
        c.zoomGlide().setClock([this] { return now; });
        c.zoom().setTimerFactoryForTest([](std::function<void()>) { return std::make_unique<ManualTimer>(); });
        QObject::connect(&lock, &chart::MidLock::polishRequested, &lock, [this] { ++polishes; });
        // The shell refits through these connections (UGR binds the same hooks).
        view.setPanShift([this](QPointF p, qint64& t, double& price) { return c.camera().panShift(p, t, price); });
        view.setPriceFit([this](qint64 s, qint64 e, double& lo, double& hi) { return c.priceFitter().autoPriceFit(s, e, lo, hi); });
    }
    void book(double mid) { c.livePlacement().setLiveBookTop(mid - 0.5, mid + 0.5); }
    double centre() const { return (view.getMinPrice() + view.getMaxPrice()) * 0.5; }
    double span() const { return view.getMaxPrice() - view.getMinPrice(); }
    int topRow() const { return c.camera().restCameraNow(false).topRowIndex; }
    int framesRun = 0;
    // One frame of the shell's polish loop: advance while a frame is pending. A spring
    // that never rests fails the test (capped) instead of spinning.
    bool frame(qint64 ms = 16) {
        now += ms;
        if (!c.framePending()) return false;
        if (++framesRun > 2000) {
            ADD_FAILURE() << "the spring never came to rest";
            return false;
        }
        c.advanceFrame(now);
        return true;
    }
    int settle(int maxFrames = 400) {
        int n = 0;
        while (frame() && n < maxFrames) ++n;
        return n;
    }
};

// CE-4 (1): enabling turns the auto price scale off and keeps the placement (no
// explicit placement, no viewport write); follow-live is not the lock's business.
TEST_F(MidLockTest, EnableTurnsAutoScaleOffAndKeepsPlacement) {
    view.setAutoPriceScale(true);
    view.enableAutoScroll(true);
    const auto placement = view.placementVersion();
    const auto version = view.getViewportVersion();
    lock.setEnabled(true);
    EXPECT_TRUE(lock.enabled());
    EXPECT_FALSE(view.autoPriceScale());
    EXPECT_TRUE(view.priceLocked());
    EXPECT_EQ(view.placementVersion(), placement);
    EXPECT_EQ(view.getViewportVersion(), version);
    EXPECT_TRUE(view.isAutoScrollEnabled());
    EXPECT_EQ(c.snapshot().lockMid, true);
    lock.setEnabled(false);
    EXPECT_FALSE(view.priceLocked());
    EXPECT_TRUE(view.isAutoScrollEnabled());
}

// CE-4 (2): mid 100 -> 110 in 16 ms frames: the centre rises monotonically, the
// span stays, the drawn origin moves by whole device pixels the same way, and the
// spring comes to rest exactly on the mid.
TEST_F(MidLockTest, GlidesMonotonicallyByWholeDevicePixels) {
    lock.setEnabled(true);
    book(100);
    EXPECT_FALSE(c.framePending()) << "already centred";
    book(110);
    EXPECT_TRUE(c.framePending());
    EXPECT_EQ(polishes, 1);
    book(110);
    EXPECT_EQ(polishes, 1) << "a moving spring is not woken again per book update";
    const auto placement = view.placementVersion();
    double last = centre();
    int lastRow = topRow(), frames = 0, maxStep = 0;
    const int firstRow = lastRow;
    while (frame()) {
        ++frames;
        ASSERT_GE(centre(), last) << "frame " << frames;
        ASSERT_LE(centre(), 110.0);
        EXPECT_NEAR(span(), 100.0, 1e-9);
        const int row = topRow();
        ASSERT_GE((row - lastRow) * (firstRow <= row ? 1 : -1), 0) << "the drawn origin never steps back";
        maxStep = std::max(maxStep, std::abs(row - lastRow));
        last = centre();
        lastRow = row;
        ASSERT_LT(frames, 200);
    }
    EXPECT_EQ(centre(), 110.0);
    EXPECT_EQ(std::abs(topRow() - firstRow), 40) << "10 price at 4 device px per price";
    EXPECT_EQ(view.placementVersion(), placement) << "the lock's writes keep the placement";
    EXPECT_LT(frames, 60) << "settles within a second at 60 Hz";
    EXPECT_LE(maxStep, 8) << "no frame jumps more than 8 device px";
    std::printf("mid 100->110 (40 device px): %d frames of 16 ms, largest step %d px\n", frames, maxStep);
}

// CE-4 (3): a reversal mid-flight retargets with no step back toward the old target.
TEST_F(MidLockTest, ReversalRetargetsWithoutBackwardStep) {
    lock.setEnabled(true);
    book(110);
    for (int i = 0; i < 5; ++i) frame();
    const double turned = centre();
    ASSERT_GT(turned, 101.0);
    ASSERT_GT(lock.velocity(), 0.0);
    book(95);
    double last = centre();
    while (frame()) {
        ASSERT_LE(centre(), last) << "never back up toward 110";
        ASSERT_GE(centre(), 95.0);
        last = centre();
    }
    EXPECT_EQ(centre(), 95.0);
    EXPECT_LT(centre(), turned);
}

// CE-4 (4): a tick or device pixel ratio change mid-motion: still monotone, the
// span kept, rest on the mid.
TEST_F(MidLockTest, TickOrDprChangeMidMotionKeepsGliding) {
    for (int variant = 0; variant < 2; ++variant) {
        env.tick = 1;
        env.size.dpr = 1;
        lock.setEnabled(true);
        const double from = centre(), to = from + 10;
        book(to);
        for (int i = 0; i < 4; ++i) frame();
        if (variant == 0) env.tick = 0.5; else env.size.dpr = 2;
        double last = centre();
        int lastRow = topRow();
        while (frame()) {
            ASSERT_GE(centre(), last);
            ASSERT_GE(topRow(), lastRow) << "variant " << variant;
            EXPECT_NEAR(span(), 100.0, 1e-9);
            last = centre();
            lastRow = topRow();
        }
        EXPECT_EQ(centre(), to) << "variant " << variant;
        lock.setEnabled(false);
    }
}

// CE-4 (5): at rest a mid inside half a device pixel wakes nothing (no frames, no
// writes); a move past it settles once, without oscillating about the boundary.
TEST_F(MidLockTest, IdleSettlesWithoutOscillationAtThePixelBoundary) {
    lock.setEnabled(true);
    book(110);
    settle();
    ASSERT_EQ(centre(), 110.0);
    const auto version = view.getViewportVersion();
    const int woke = polishes;
    for (double jitter : {110.1, 109.9, 110.12, 109.88, 110.0}) { // 0.125 price per half pixel
        book(jitter);
        EXPECT_FALSE(c.framePending()) << jitter;
    }
    for (int i = 0; i < 10; ++i) frame();
    EXPECT_EQ(view.getViewportVersion(), version) << "no per-frame writes at rest";
    EXPECT_EQ(polishes, woke);
    book(110.25); // one device pixel
    ASSERT_TRUE(c.framePending());
    std::vector<double> path{centre()};
    while (frame()) path.push_back(centre());
    path.push_back(centre());
    EXPECT_EQ(centre(), 110.25);
    for (size_t i = 1; i < path.size(); ++i) EXPECT_GE(path[i], path[i - 1]) << "no oscillation";
    EXPECT_FALSE(lock.moving());
}

// CE-4 (6): follow-live steps time under the lock without disturbing the spring.
TEST_F(MidLockTest, FollowLiveIsUnchangedByTheLock) {
    view.setViewport(0, 100000, 50, 150);
    view.enableAutoScroll(true);
    lock.setEnabled(true);
    EXPECT_TRUE(view.isAutoScrollEnabled());
    book(110);
    for (int i = 0; i < 4; ++i) frame();
    const double v = lock.velocity();
    ASSERT_GT(v, 0.0);
    c.livePlacement().followGpuLiveTo(150000);
    EXPECT_GT(view.getVisibleTimeEnd(), 100000) << "follow still steps time";
    double last = centre();
    frame();
    EXPECT_GT(centre(), last) << "the follow write is not adopted as a new window (velocity kept)";
    settle();
    EXPECT_EQ(centre(), 110.0);
    EXPECT_TRUE(view.isAutoScrollEnabled());
    lock.setEnabled(false);
    EXPECT_TRUE(view.isAutoScrollEnabled());
}

// D7: a one-sided or withdrawn book centres on the last trade; the next two-sided
// top returns to the mid; the lock never drops.
TEST_F(MidLockTest, OneSidedBookFallsBackToLastTradeThenReturnsToMid) {
    lock.setEnabled(true);
    book(100);
    c.livePlacement().onTrade("TEST", 120);
    EXPECT_FALSE(c.framePending()) << "two-sided: the mid wins";
    c.livePlacement().setLiveBookTop(99.5, 0); // the ask side is gone
    ASSERT_TRUE(c.framePending());
    settle();
    EXPECT_EQ(centre(), 120.0);
    EXPECT_TRUE(lock.enabled());
    c.livePlacement().setLiveBookTop(0, 0); // withdrawn
    c.livePlacement().onTrade("TEST", 125);
    settle();
    EXPECT_EQ(centre(), 125.0);
    book(104);
    settle();
    EXPECT_EQ(centre(), 104.0);
    EXPECT_TRUE(lock.enabled());
}

// D1: a chart drag pans time only while locked (dy suppressed) and turns follow-live
// off; the lock stays.
TEST_F(MidLockTest, D1ChartDragPansTimeOnlyAndKeepsTheLock) {
    lock.setEnabled(true);
    view.enableAutoScroll(true);
    const double lo = view.getMinPrice(), hi = view.getMaxPrice();
    const qint64 start = view.getVisibleTimeStart();
    c.commands().beginPanAt(100, 100);
    c.commands().updatePanAt(180, 220);
    EXPECT_EQ(view.getPanVisualOffset().y(), 0.0);
    EXPECT_EQ(view.getPanVisualOffset().x(), 80.0);
    c.commands().endPanAt();
    EXPECT_NE(view.getVisibleTimeStart(), start);
    EXPECT_EQ(view.getMinPrice(), lo);
    EXPECT_EQ(view.getMaxPrice(), hi);
    EXPECT_TRUE(lock.enabled());
    EXPECT_FALSE(view.isAutoScrollEnabled());
}

// D2: the chart wheel and pinch zoom time and price; price zooms about the locked
// centre (not the cursor) and the lock takes the new span.
TEST_F(MidLockTest, D2ChartWheelZoomsSpanAboutTheLockedCentre) {
    lock.setEnabled(true);
    const double before = centre(), span0 = span();
    const qint64 time0 = view.getVisibleTimeEnd() - view.getVisibleTimeStart();
    c.zoom().wheelZoom(120, true, 0, false, 600, 40, true, true, chart::ZoomController::WheelRoute::Chart);
    EXPECT_TRUE(lock.enabled());
    EXPECT_LT(span(), span0) << "price zoomed";
    EXPECT_NE(view.getVisibleTimeEnd() - view.getVisibleTimeStart(), time0) << "time zoomed";
    EXPECT_NEAR(centre(), before, 0.25) << "about the centre (within a device pixel), not the cursor at y=40";
    while (frame()) {} // the glide lands
    book(before + 5); // the spring runs from the zoomed window with its span
    settle();
    EXPECT_EQ(centre(), before + 5);
    EXPECT_NEAR(lock.spanPrice(), span(), 1e-12);
    EXPECT_LT(lock.spanPrice(), span0);
    EXPECT_TRUE(lock.enabled());
}

TEST_F(MidLockTest, D2PinchZoomsSpanAndSettlesAboutTheLockedCentre) {
    lock.setEnabled(true);
    const double before = centre(), span0 = span();
    c.zoom().zoomContinuousFor(1.3, 600, 40, true, true, chart::ZoomController::kGestureSafetyMs);
    c.zoom().zoomContinuousFor(1.2, 600, 30, true, true, chart::ZoomController::kGestureSafetyMs);
    c.zoom().endZoomGesture();
    EXPECT_TRUE(lock.enabled());
    EXPECT_LT(span(), span0);
    EXPECT_NEAR(centre(), before, 0.25);
    book(130);
    settle();
    EXPECT_EQ(centre(), 130.0) << "still locked after the settle";
    EXPECT_LT(span(), span0);
}

// D3: price-axis wheel, drag and clicks release the lock, then zoom price as before.
TEST_F(MidLockTest, D3PriceAxisRoutesReleaseTheLock) {
    for (int route = 0; route < 3; ++route) {
        lock.setEnabled(true);
        const double span0 = span();
        if (route == 0) c.zoom().zoomPriceWheel(120, 0, 0, 100);
        if (route == 1) { c.zoom().zoomPriceDrag(200, 100); c.zoom().endZoomGesture(); }
        if (route == 2) c.zoom().zoomPriceClicks(1, 100);
        EXPECT_FALSE(lock.enabled()) << "route " << route;
        EXPECT_FALSE(view.autoPriceScale());
        EXPECT_NE(span(), span0) << "route " << route << ": the axis still zooms price";
        while (frame()) {}
    }
}

// D4: keyboard price pans and explicit price bounds release the lock (and the
// snapshot the API replies with says so).
TEST_F(MidLockTest, D4PricePansAndExplicitBoundsReleaseTheLock) {
    lock.setEnabled(true);
    const double lo = view.getMinPrice();
    c.commands().panUp();
    EXPECT_FALSE(lock.enabled());
    EXPECT_GT(view.getMinPrice(), lo) << "the pan applies after the release";
    lock.setEnabled(true);
    c.commands().panDown();
    EXPECT_FALSE(lock.enabled());
    lock.setEnabled(true);
    c.commands().setViewport(0, 100000, 60, 90);
    EXPECT_FALSE(lock.enabled());
    lock.setEnabled(true);
    chart::ViewportRequest request;
    request.priceMin = 70;
    request.priceMax = 80;
    EXPECT_TRUE(c.applyViewportRequest(request).isEmpty());
    EXPECT_FALSE(lock.enabled());
    EXPECT_FALSE(c.snapshot().lockMid);
    EXPECT_EQ(view.getMinPrice(), 70.0);
}

// D5: a price fit, the default view and the auto price scale release the lock; a
// time fit does not.
TEST_F(MidLockTest, D5FitsAndAutoScaleReleaseTheLock) {
    lock.setEnabled(true);
    chart::ViewportRequest time;
    time.fit = "time";
    c.applyViewportRequest(time);
    EXPECT_TRUE(lock.enabled()) << "a time fit leaves price to the lock";
    for (const char* fit : {"price", "default", "both"}) {
        lock.setEnabled(true);
        chart::ViewportRequest request;
        request.fit = fit;
        EXPECT_TRUE(c.applyViewportRequest(request).isEmpty()) << fit;
        EXPECT_FALSE(lock.enabled()) << fit;
        EXPECT_TRUE(view.autoPriceScale()) << fit;
    }
    lock.setEnabled(true);
    EXPECT_FALSE(view.autoPriceScale());
    chart::ViewportRequest autoScale;
    autoScale.autoScale = true;
    c.applyViewportRequest(autoScale);
    EXPECT_FALSE(lock.enabled());
    lock.setEnabled(true);
    c.commands().setAutoPriceScale(true); // the P toggle
    EXPECT_FALSE(lock.enabled());
}
} // namespace
