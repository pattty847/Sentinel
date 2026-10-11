#include "chart/core/ChartViewController.hpp"
#include "chart/core/PublishedFrame.hpp"
#include <gtest/gtest.h>
#include <limits>

namespace {
struct Environment final : chart::IChartEnvironment, chart::ITickOracle, chart::IHeatmapViewSink {
    chart::SurfaceFacts size{800, 400, 1};
    bool present = true;
    double tick = 1;
    bool attached() const override { return present; }
    chart::SurfaceFacts surface() const override { return size; }
    double drawnTick() const override { return tick; }
    double priceScale() const override { return 1; }
    bool manualTick() const override { return true; }
    bool hasCurrentResolution() const override { return true; }
    bool priceScaleCurrent() const override { return true; }
    chart::SpanLimits limits() const override { return {}; }
    int64_t liveAnchorMs() const override { return 100000; }
    int64_t liveOpenEndMs() const override { return 100000; }
    int64_t oldestAvailableMs() const override { return 0; }
    double recentMidPrice() const override { return 100; }
    std::optional<heatmap::gpu::ViewWindow> restView() const override { return std::nullopt; }
    int64_t predictTickUnits(const heatmap::gpu::ViewWindow&) const override { return 1; }
    bool buildsTick(int64_t, const heatmap::gpu::ViewWindow&) const override { return true; }
    int64_t autoTickState() const override { return 1; }
    heatmap::AutoTickParams autoTickParams() const override { return {1, .1}; }
    bool setAutoPriceTick(std::optional<int64_t>) override { return false; }
    void setView(const heatmap::gpu::ViewWindow&, bool) override {}
};
struct Timer {
    bool active = false;
    int starts = 0, stops = 0;
    qint64 now = 0, deadline = 0;
    std::function<void()> fire;
    void advance(int ms) { now += ms; if (active && now >= deadline) { active = false; fire(); } }
};
struct TimerHandle final : chart::IZoomTimer {
    Timer& t;
    explicit TimerHandle(Timer& timer) : t(timer) {}
    bool isActive() const override { return t.active; }
    void start(int ms) override { t.active = true; t.deadline = t.now + ms; ++t.starts; }
    void stop() override { t.active = false; ++t.stops; }
};
struct ZoomTest : testing::Test {
    Environment env;
    chart::PublishedFrame published;
    chart::ChartViewController c{env, env, env, published};
    GridViewState& v = *c.viewPtr();
    chart::ZoomController& z = c.zoom();
    Timer timer;
    std::vector<std::string> events;
    void SetUp() override {
        c.session().timeframeMs = 1000;
        v.setViewportSize(800, 400);
        v.setAutoPriceScale(false);
        v.setViewport(0, 100000, 50, 150);
        v.enableAutoScroll(false);
        v.setRasterAnchor(.5, .5);
        c.zoomGlide().setClock([this] { return timer.now; });
        z.setTimerFactoryForTest([this](std::function<void()> fire) {
            timer.fire = std::move(fire);
            return std::make_unique<TimerHandle>(timer);
        });
        QObject::connect(&v, &GridViewState::viewportChanged, &c, [this] { events.push_back("view"); });
        QObject::connect(&v, &GridViewState::priceInteracted, &c, [this] { events.push_back("price"); });
        QObject::connect(&v, &GridViewState::autoScrollEnabledChanged, &c, [this] { events.push_back("follow"); });
        QObject::connect(&z, &chart::ZoomController::updateRequested, &c, [this] { events.push_back("update"); });
        QObject::connect(&c.zoomGlide(), &chart::ZoomGlide::rasterChanged, &c, [this] { events.push_back("raster"); });
        QObject::connect(&c.zoomGlide(), &chart::ZoomGlide::viewportChanged, &c, [this] { events.push_back("glide-view"); });
        QObject::connect(&c.zoomGlide(), &chart::ZoomGlide::polishRequested, &c, [this] { events.push_back("polish"); });
        QObject::connect(&c.zoomGlide(), &chart::ZoomGlide::updateRequested, &c, [this] { events.push_back("update"); });
    }
    void bounds(qint64 start, qint64 end, double lo, double hi) {
        EXPECT_EQ(v.getVisibleTimeStart(), start);
        EXPECT_EQ(v.getVisibleTimeEnd(), end);
        EXPECT_DOUBLE_EQ(v.getMinPrice(), lo);
        EXPECT_DOUBLE_EQ(v.getMaxPrice(), hi);
    }
    void wheel(int angle, bool null = true, int phase = 0, bool precise = false,
               bool time = true, bool price = false, chart::ZoomController::WheelRoute route = chart::ZoomController::WheelRoute::Chart) {
        z.wheelZoom(angle, null, Qt::ScrollPhase(phase), precise, 400, 200, time, price, route);
    }
};
TEST(WheelInterpreterTest, EveryClassificationCombination) {
    using K = chart::WheelInterpreter::Kind;
    // Explicit truth table: columns are null/non-null pixels crossed with plain/precise.
    for (int angle : {-240, -120, -60, 0, 60, 120, 240}) {
        const bool multiple = angle == -240 || angle == -120 || angle == 120 || angle == 240;
        for (int phase : {0, 1, 2, 3, 4}) {
            for (bool null : {false, true}) for (bool precise : {false, true}) {
                K expected = angle == 0 ? K::Ignore : K::Continuous;
                if (phase == 3) expected = K::End;
                if (phase == 0 && !precise && (null || multiple)) expected = K::Notch;
                EXPECT_EQ(chart::WheelInterpreter::classify(angle, null, phase, precise), expected)
                    << angle << "," << null << "," << phase << "," << precise;
            }
        }
    }
}
TEST_F(ZoomTest, SetRungChangesOnlySelectedAxes) {
    const auto drawn = c.camera().rasterCameraNow(false);
    ASSERT_TRUE(z.setZoomRung(drawn, .25, .75, true, 16, false, {}));
    bounds(12500, 62500, 50, 150);
    EXPECT_DOUBLE_EQ(v.rasterAnchor().fracX, .25);
    EXPECT_DOUBLE_EQ(v.rasterAnchor().fracY, .5);
    ASSERT_TRUE(z.setZoomRung(drawn, .25, .75, false, 0, true, {1, 8}));
    bounds(12500, 62500, 62.5, 112.5);
    ASSERT_TRUE(z.setZoomRung(drawn, .25, .75, false, 0, true, {-80, 0}));
    bounds(12500, 62500, 55, 135);
    EXPECT_FALSE(z.setZoomRung({}, .5, .5, true, 16, true, {1,8}));
    bounds(12500, 62500, 55, 135);
}
TEST_F(ZoomTest, StoredViewClampsAndPreservesSignalOrder) {
    v.setMinSpans(60000, 60);
    v.setMaxSpans(120000, 120);
    v.enableAutoScroll(true);
    events.clear();
    z.zoomStoredView(2, 200, 300, true, true);
    bounds(10000, 70000, 60, 120);
    EXPECT_EQ(events, (std::vector<std::string>{"view", "price", "follow", "update"}));
    events.clear();
    z.zoomStoredView(2, 200, 300, true, true);
    EXPECT_TRUE(events.empty());
    z.zoomStoredView(.1, -10, 900, true, true);
    bounds(10000, 130000, 60, 180);
    EXPECT_DOUBLE_EQ(v.rasterAnchor().fracX, 0);
    EXPECT_DOUBLE_EQ(v.rasterAnchor().fracY, 1);
}
TEST_F(ZoomTest, StoredViewZeroSurfaceUsesCenter) {
    env.size = {0,0,1};
    z.zoomStoredView(2, 200, 300, true, true);
    bounds(25000, 75000, 75, 125);
}
TEST_F(ZoomTest, ClicksRejectZeroDraggingAndUnselectedAxes) {
    z.zoomClicks(0,400,200);
    z.zoomClicks(1,400,200,false,false);
    v.handlePanStart({0,0});
    events.clear();
    z.zoomClicks(1,400,200);
    bounds(0,100000,50,150);
    EXPECT_TRUE(events.empty());
}
TEST_F(ZoomTest, ClicksUseStoredFallbackWithoutCamera) {
    env.size.widthLogical = 0;
    z.zoomClicks(1,400,200);
    bounds(10000,90000,60,140);
    EXPECT_FALSE(c.zoomGlide().gliding());
    EXPECT_EQ(events, (std::vector<std::string>{"view","price","update"}));
}
TEST_F(ZoomTest, ClicksAccumulateFromTargetAndKeepAutoPrice) {
    v.setAutoPriceScale(true);
    events.clear();
    z.zoomClicks(1,400,200);
    bounds(10000,90000,50,150);
    EXPECT_EQ(c.glide().colPx, 10);
    EXPECT_TRUE(c.glide().time.active);
    EXPECT_FALSE(c.glide().price.active);
    z.zoomClicks(1,400,200);
    bounds(19231,80769,50,150);
    EXPECT_EQ(c.glide().colPx, 13);
    EXPECT_EQ(events, (std::vector<std::string>{"view","raster","glide-view","polish","update",
                                             "view","raster","glide-view","polish","update"}));
}
TEST_F(ZoomTest, ClicksPriceRungAndDirectionalFallback) {
    z.zoomClicks(1,200,300,false,true);
    bounds(0,100000,55,135);
    EXPECT_EQ(c.glide().row.rowPx,5);
    c.zoomGlide().endZoomGlide();
    env.tick = 0;
    // A valid time camera with no rows falls back to the directional price ratio.
    v.setMinSpans(0,100);
    z.zoomClicks(-1,400,200,false,true);
    bounds(0,100000,45,145);
}
TEST_F(ZoomTest, ClicksAtLimitsLeaveViewAndSignalsAlone) {
    v.setMinSpans(100000,100);
    v.setMaxSpans(100000,100);
    events.clear();
    z.zoomClicks(1,400,200);
    bounds(0,100000,50,150);
    EXPECT_TRUE(events.empty());
    z.zoomClicks(-1,400,200);
    EXPECT_TRUE(events.empty());
}
TEST_F(ZoomTest, ContinuousTimerRestartsAndSettles) {
    z.zoomContinuous(2,400,200,true,false);
    bounds(25000,75000,50,150);
    EXPECT_TRUE(c.glide().gestureTime);
    EXPECT_FALSE(c.glide().gesturePrice);
    ASSERT_TRUE(z.gestureSettlePending());
    EXPECT_EQ(timer.deadline,160);
    timer.advance(159);
    EXPECT_TRUE(c.glide().gesture);
    z.zoomContinuousFor(2,400,200,true,false,1000);
    EXPECT_EQ(timer.deadline,1159);
    bounds(37500,62500,50,150);
    timer.advance(999);
    EXPECT_TRUE(c.glide().gesture);
    timer.advance(1);
    EXPECT_FALSE(c.glide().gesture);
    EXPECT_FALSE(z.gestureSettlePending());
    EXPECT_EQ(timer.starts,2);
    EXPECT_EQ(timer.stops,1);
}
TEST_F(ZoomTest, ContinuousAxesJoinAndClamp) {
    v.setMinSpans(60000,60);
    v.setMaxSpans(120000,120);
    v.enableAutoScroll(true);
    events.clear();
    z.zoomContinuous(2,200,300,true,false);
    bounds(10000,70000,50,150);
    z.zoomContinuous(2,200,300,false,true);
    bounds(10000,70000,60,120);
    EXPECT_TRUE(c.glide().gestureTime && c.glide().gesturePrice);
    EXPECT_EQ(events, (std::vector<std::string>{"view","follow","update","view","price","update"}));
    z.zoomContinuous(.1,200,300,true,true);
    bounds(-5000,115000,45,165);
}
TEST_F(ZoomTest, ContinuousRejectsInvalidAndFallsBackBeforeGesture) {
    for (double f : {0.,-1.,1.,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()})
        z.zoomContinuous(f,400,200);
    z.zoomContinuous(2,400,200,false,false);
    EXPECT_TRUE(events.empty());
    env.size.widthLogical=0;
    z.zoomContinuous(2,400,200,true,true);
    bounds(25000,75000,75,125);
    EXPECT_FALSE(c.glide().gesture);
    c.glide().gesture=true;
    events.clear();
    z.zoomContinuous(2,400,200);
    EXPECT_TRUE(events.empty());
}
TEST_F(ZoomTest, EndGestureSettlesAndClickEndsGestureFirst) {
    z.zoomContinuous(1.1,200,300,true,true);
    events.clear();
    z.endZoomGesture();
    EXPECT_FALSE(c.glide().gesture);
    EXPECT_FALSE(z.gestureSettlePending());
    EXPECT_EQ(c.glide().colPx,9);
    EXPECT_EQ(c.glide().row.rowPx,4);
    EXPECT_EQ(events, (std::vector<std::string>{"view","raster","glide-view","polish","update"}));
    z.zoomContinuous(1.1,200,300,true,false);
    z.zoomClicks(1,200,300,true,false);
    EXPECT_FALSE(c.glide().gesture);
    EXPECT_FALSE(z.gestureSettlePending());
}
TEST_F(ZoomTest, EndGestureNoOpAndInvalidCamera) {
    z.endZoomGesture();
    EXPECT_TRUE(events.empty());
    z.zoomContinuous(2,400,200);
    env.size.widthLogical=0;
    events.clear();
    z.endZoomGesture();
    EXPECT_EQ(events,(std::vector<std::string>{"update"}));
    EXPECT_FALSE(c.glide().gesture);
}
TEST_F(ZoomTest, WheelCarriesPartialOnlyAndDropsRouteAxesReversal) {
    wheel(60);
    bounds(0,100000,50,150);
    wheel(60);
    EXPECT_EQ(c.glide().colPx,10);
    wheel(60);
    wheel(-60);
    EXPECT_EQ(c.glide().colPx,10);
    wheel(-60);
    EXPECT_EQ(c.glide().colPx,8);
    wheel(60);
    wheel(60,true,0,false,true,false,chart::ZoomController::WheelRoute::TimeAxis);
    EXPECT_EQ(c.glide().colPx,8);
    wheel(60,true,0,false,false,true,chart::ZoomController::WheelRoute::TimeAxis);
    EXPECT_EQ(c.glide().colPx,8);
    wheel(1200);
    EXPECT_EQ(c.glide().colPx,20);
    wheel(-120);
    EXPECT_EQ(c.glide().colPx,16);
}
TEST_F(ZoomTest, WheelPixelNotchesPrecisePhasesAndEnd) {
    wheel(120,false);
    EXPECT_EQ(c.glide().colPx,10);
    wheel(30,false,2,true);
    EXPECT_TRUE(c.glide().gesture);
    EXPECT_EQ(timer.deadline,160);
    wheel(0,false,3,true);
    EXPECT_FALSE(c.glide().gesture);
    events.clear();
    wheel(0,false,0,true);
    EXPECT_TRUE(events.empty());
}
TEST_F(ZoomTest, AxisWrappersPreserveOwnershipAndSafetyTimeout) {
    v.setAutoPriceScale(true);
    z.zoomPriceWheel(0,0,0,200);
    EXPECT_TRUE(v.autoPriceScale());
    z.zoomPriceWheel(120,1,0,200);
    EXPECT_FALSE(v.autoPriceScale());
    EXPECT_EQ(c.glide().row.rowPx,5);
    z.zoomTimeWheel(120,1,0,400);
    EXPECT_EQ(c.glide().colPx,10);
    z.zoomTimeDrag(100,400);
    EXPECT_EQ(timer.deadline,1000);
    z.endZoomGesture();
    v.setAutoPriceScale(true);
    z.zoomPriceDrag(100,200);
    EXPECT_FALSE(v.autoPriceScale());
    EXPECT_EQ(timer.deadline,1000);
    timer.advance(1000);
    EXPECT_FALSE(c.glide().gesture);
    v.setAutoPriceScale(true);
    z.zoomPriceClicks(1,200);
    EXPECT_FALSE(v.autoPriceScale());
    const int col=c.glide().colPx;
    z.zoomTimeClicks(1,400);
    EXPECT_GT(c.glide().colPx,col);
}
} // namespace
