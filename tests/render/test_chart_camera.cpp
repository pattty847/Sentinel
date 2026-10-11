#include "chart/core/ChartViewController.hpp"
#include "chart/core/PublishedFrame.hpp"
#include "render/FrameContextBuilder.hpp"

#include <gtest/gtest.h>

namespace {

// No application, event loop, Qt Test or GUI dependency: only the policy and value ports.
struct CameraPort final : chart::IChartEnvironment, chart::ITickOracle, chart::IHeatmapViewSink {
    bool isAttached = true;
    mutable heatmap::gpu::ViewWindow predictedView{};
    bool attached() const override { return isAttached; }
    chart::SurfaceFacts surface() const override { return {100, 80, 2}; }
    double drawnTick() const override { return 2; }
    double priceScale() const override { return 100; }
    bool manualTick() const override { return false; }
    bool hasCurrentResolution() const override { return true; }
    bool priceScaleCurrent() const override { return true; }
    chart::SpanLimits limits() const override { return {}; }
    int64_t liveAnchorMs() const override { return 0; }
    int64_t liveOpenEndMs() const override { return 0; }
    int64_t oldestAvailableMs() const override { return 0; }
    double recentMidPrice() const override { return 0; }
    std::optional<heatmap::gpu::ViewWindow> restView() const override { return std::nullopt; }
    int64_t predictTickUnits(const heatmap::gpu::ViewWindow& view) const override {
        predictedView = view;
        return 400;
    }
    bool buildsTick(int64_t, const heatmap::gpu::ViewWindow&) const override { return true; }
    int64_t autoTickState() const override { return 200; }
    heatmap::AutoTickParams autoTickParams() const override { return {}; }
    bool setAutoPriceTick(std::optional<int64_t>) override { return false; }
    void setView(const heatmap::gpu::ViewWindow&, bool) override {}
};

struct ChartCameraTest : testing::Test {
    CameraPort port;
    chart::PublishedFrame published;
    chart::ChartViewController controller{port, port, port, published};

    void SetUp() override {
        controller.session().timeframeMs = 100;
        controller.viewPtr()->setAutoPriceScale(false);
        controller.viewPtr()->setViewportSize(100, 80);
        controller.viewPtr()->setViewport(1000, 2000, 100, 140);
        published.setStep({8, 20});
    }
};

TEST_F(ChartCameraTest, FixedRestDrawnAndPanOutputs) {
    auto& camera = controller.camera();
    const auto rest = camera.restCameraNow(false);
    ASSERT_TRUE(rest.valid);
    EXPECT_FALSE(rest.free);
    EXPECT_EQ(rest.widthDev, 200);
    EXPECT_EQ(rest.heightDev, 160);
    EXPECT_DOUBLE_EQ(rest.dpr, 2);
    EXPECT_EQ(rest.colPx, 20);
    EXPECT_EQ(rest.rowPx, 8);
    EXPECT_EQ(rest.leftColIndex, 200);
    EXPECT_EQ(rest.topRowIndex, 560);
    EXPECT_DOUBLE_EQ(rest.drawnStartMs, 1000);
    EXPECT_DOUBLE_EQ(rest.drawnEndMs, 2000);
    EXPECT_DOUBLE_EQ(rest.drawnMinPrice, 100);
    EXPECT_DOUBLE_EQ(rest.drawnMaxPrice, 140);

    controller.viewPtr()->handlePanStart({0, 0});
    controller.viewPtr()->handlePanMove({2.75, -1.25});
    const auto drawn = camera.rasterCameraNow();
    ASSERT_TRUE(drawn.valid);
    EXPECT_EQ(drawn.leftColIndex, 194);
    EXPECT_EQ(drawn.topRowIndex, 557);
    EXPECT_DOUBLE_EQ(drawn.drawnStartMs, 970);
    EXPECT_DOUBLE_EQ(drawn.drawnEndMs, 1970);
    EXPECT_DOUBLE_EQ(drawn.drawnMinPrice, 99.25);
    EXPECT_DOUBLE_EQ(drawn.drawnMaxPrice, 139.25);
    EXPECT_DOUBLE_EQ(camera.rasterCameraNow(false).drawnStartMs, 1000);
    EXPECT_DOUBLE_EQ(camera.restCameraNow().drawnMaxPrice, 139.25);

    qint64 time = 0;
    double price = 0;
    ASSERT_TRUE(camera.panShift({2.75, -1.25}, time, price));
    EXPECT_EQ(time, -30);
    EXPECT_DOUBLE_EQ(price, -0.75);
    const auto window = camera.drawnTimeWindow(1000, 2000);
    EXPECT_DOUBLE_EQ(window.first, 1000);
    EXPECT_DOUBLE_EQ(window.second, 2000);
}

TEST_F(ChartCameraTest, PredictedEndAndIndependentHeldAxis) {
    auto& camera = controller.camera();
    const auto end = camera.intendedEndNow();
    ASSERT_TRUE(end.valid);
    EXPECT_DOUBLE_EQ(end.tick, 4);
    EXPECT_EQ(end.rowPx, 16);
    EXPECT_DOUBLE_EQ(end.drawnMinPrice, 100);
    EXPECT_DOUBLE_EQ(end.drawnMaxPrice, 140);
    EXPECT_DOUBLE_EQ(camera.tickPredictor(900, 2100, 120, 0.25)(40), 4);
    EXPECT_DOUBLE_EQ(port.predictedView.timeLoMs, 900);
    EXPECT_DOUBLE_EQ(port.predictedView.timeHiMs, 2100);
    EXPECT_DOUBLE_EQ(port.predictedView.priceLo, 90);
    EXPECT_DOUBLE_EQ(port.predictedView.priceHi, 130);

    auto& glide = controller.glide();
    glide.time.active = true;
    glide.time.holding = true;
    glide.time.from = camera.restCameraNow(false);
    glide.time.end = chart_raster::shiftedRaster(glide.time.from, 20, 0);
    const auto drawn = camera.rasterCameraNow(false);
    EXPECT_TRUE(drawn.free);
    EXPECT_DOUBLE_EQ(drawn.drawnStartMs, 900);
    EXPECT_DOUBLE_EQ(drawn.drawnEndMs, 1900);
    EXPECT_DOUBLE_EQ(drawn.drawnMinPrice, 100);
    EXPECT_DOUBLE_EQ(camera.restCameraNow(false).drawnStartMs, 1000);

    const chart::CameraInputs captured{
        camera.rasterInputs(FrameContextBuilder::viewportSnapshot(controller.viewPtr()), 2, port.surface()),
        published.step(), glide};
    controller.viewPtr()->setViewport(3000, 5000, 200, 280);
    glide.time.active = false;
    const auto composed = chart::camerasFor(captured);
    EXPECT_DOUBLE_EQ(composed.drawn.drawnStartMs, 900);
    ASSERT_TRUE(composed.binView);
    EXPECT_DOUBLE_EQ(composed.binView->timeLo, 900);
    EXPECT_DOUBLE_EQ(composed.binView->timeHi, 2000);
    EXPECT_DOUBLE_EQ(composed.binView->priceLo, 100);
    EXPECT_DOUBLE_EQ(composed.binView->priceHi, 140);
}

TEST_F(ChartCameraTest, GestureUsesContinuousCameraOnlyOnSelectedAxis) {
    controller.viewPtr()->setViewport(1000, 1975, 100, 139);
    auto& glide = controller.glide();
    glide.gesture = glide.gestureTime = true;
    auto drawn = controller.camera().rasterCameraNow(false);
    EXPECT_TRUE(drawn.free);
    EXPECT_DOUBLE_EQ(drawn.colPxF, 20000.0 / 975.0);
    EXPECT_DOUBLE_EQ(drawn.rowPxF, 8);
    glide.gestureTime = false;
    glide.gesturePrice = true;
    drawn = controller.camera().rasterCameraNow(false);
    EXPECT_DOUBLE_EQ(drawn.colPxF, 20);
    EXPECT_DOUBLE_EQ(drawn.rowPxF, 320.0 / 39.0);
}

TEST_F(ChartCameraTest, NonHoldingGlideDrawsIntermediateProgress) {
    auto& g = controller.glide().time;
    g.active = true;
    g.from = controller.camera().restCameraNow(false);
    g.end = chart_raster::shiftedRaster(g.from, 20, 0);
    g.anchorTime = 1500;
    g.anchorPrice = 120;
    g.progress = 0.25;
    auto drawn = controller.camera().rasterCameraNow(false);
    EXPECT_DOUBLE_EQ(drawn.drawnStartMs, 975);
    EXPECT_DOUBLE_EQ(drawn.drawnEndMs, 1975);
    g.progress = 0.75;
    drawn = controller.camera().rasterCameraNow(false);
    EXPECT_DOUBLE_EQ(drawn.drawnStartMs, 925);
    EXPECT_DOUBLE_EQ(drawn.drawnMinPrice, 100);
}

TEST_F(ChartCameraTest, PanCommitsAtActiveGlideTickAndRungs) {
    controller.viewPtr()->setViewport(1000, 1975, 100, 139);
    auto& glide = controller.glide();
    glide.time.active = glide.price.active = true;
    glide.time.end = glide.price.end = controller.camera().intendedEndNow();
    glide.time.end.colPx = 21;
    glide.price.end.rowPx = 17;
    qint64 time = 0;
    double price = 0;
    ASSERT_TRUE(controller.camera().panShift({10, 4}, time, price));
    EXPECT_EQ(time, -95);
    EXPECT_DOUBLE_EQ(price, 32.0 / 17.0);
}

TEST_F(ChartCameraTest, DrawnTimeWindowUsesPriceFallback) {
    chart::ChartViewController empty{port, port, port, published};
    const auto window = empty.camera().drawnTimeWindow(1000, 1975);
    EXPECT_DOUBLE_EQ(window.first, 990);
    EXPECT_DOUBLE_EQ(window.second, 1990);
}

TEST_F(ChartCameraTest, DrawnTimeWindowUsesExplicitAnchor) {
    const auto window = controller.camera().drawnTimeWindow(1000, 1975, 0.0);
    EXPECT_DOUBLE_EQ(window.first, 1000);
    EXPECT_DOUBLE_EQ(window.second, 2000);
}

TEST_F(ChartCameraTest, ZoomGlideStartAdvanceAndEndPreserveNotificationOrder) {
    auto& policy = controller.zoomGlide();
    qint64 now = 17;
    policy.setClock([&] { return now; });
    std::string events;
    QObject::connect(&policy, &chart::ZoomGlide::rasterChanged, &policy, [&] { events += 'r'; }, Qt::DirectConnection);
    QObject::connect(&policy, &chart::ZoomGlide::viewportChanged, &policy, [&] { events += 'v'; }, Qt::DirectConnection);
    QObject::connect(&policy, &chart::ZoomGlide::polishRequested, &policy, [&] { events += 'p'; }, Qt::DirectConnection);
    QObject::connect(&policy, &chart::ZoomGlide::updateRequested, &policy, [&] { events += 'u'; }, Qt::DirectConnection);
    const auto before = controller.camera().restCameraNow(false);
    controller.viewPtr()->setViewport(1200, 2000, 104, 136);
    policy.startAxisGlides(before, before, 1800, 120, false, false);
    EXPECT_EQ(events, "rvpu");
    EXPECT_TRUE(controller.glide().time.active);
    EXPECT_TRUE(controller.glide().price.active);
    EXPECT_EQ(controller.glide().time.beganMs, now);
    events.clear();
    controller.advanceFrame(now + chart_raster::kZoomGlideMs / 2);
    EXPECT_EQ(events, "rvu");
    EXPECT_DOUBLE_EQ(controller.glide().time.progress, chart_raster::easeZoom(0.5));
    EXPECT_DOUBLE_EQ(controller.glide().price.progress, chart_raster::easeZoom(0.5));
    events.clear();
    policy.endZoomGlide(true, false);
    EXPECT_EQ(events, "u");
    EXPECT_FALSE(controller.glide().time.active);
    EXPECT_TRUE(controller.glide().price.active);
    policy.endZoomGlide(true, false);
    EXPECT_EQ(events, "u");
    policy.endZoomGlide();
    EXPECT_EQ(events, "uu");
    events.clear();
    controller.advanceFrame(now + 1);
    EXPECT_TRUE(events.empty());
}

TEST_F(ChartCameraTest, ZoomGlideHoldsPredictedTickThenRebasesAndCaps) {
    auto& policy = controller.zoomGlide();
    const auto before = controller.camera().restCameraNow(false);
    // Price uses a different tick in the predicted end, before a frame commits it.
    policy.startAxisGlides(before, before, 1500, 120, true, true);
    controller.advanceFrame(chart_raster::kZoomGlideMs);
    EXPECT_FALSE(controller.glide().time.active);
    EXPECT_TRUE(controller.glide().price.active);
    EXPECT_TRUE(controller.glide().price.holding);
    const auto held = controller.glide().price.end;
    controller.viewPtr()->setViewport(1000, 2000, 100, 132);
    controller.advanceFrame(chart_raster::kZoomGlideMs + 1);
    EXPECT_EQ(controller.glide().rebases, 1);
    EXPECT_FALSE(controller.glide().price.holding);
    EXPECT_DOUBLE_EQ(controller.glide().price.from.drawnMaxPrice, held.drawnMaxPrice);
    EXPECT_DOUBLE_EQ(controller.glide().price.progress, 0);
    controller.advanceFrame(2 * chart_raster::kZoomGlideMs);
    EXPECT_FALSE(policy.gliding());
    EXPECT_DOUBLE_EQ(controller.glide().price.progress, 1);
}

TEST_F(ChartCameraTest, ZoomGlideTranslationsMoveBothEndsWithoutRebase) {
    auto& policy = controller.zoomGlide();
    const auto before = controller.camera().restCameraNow(false);
    policy.startAxisGlides(before, before, 1500, 120, true, true);
    const auto original = controller.glide();
    policy.shiftTime(100);
    EXPECT_DOUBLE_EQ(controller.glide().time.from.drawnStartMs, original.time.from.drawnStartMs + 100);
    EXPECT_DOUBLE_EQ(controller.glide().time.end.drawnStartMs, original.time.end.drawnStartMs + 100);
    EXPECT_DOUBLE_EQ(controller.glide().time.anchorTime, 1600);
    EXPECT_DOUBLE_EQ(controller.glide().price.anchorTime, 1500);
    policy.shiftBoth(20, -8);
    EXPECT_DOUBLE_EQ(controller.glide().time.from.drawnStartMs, original.time.from.drawnStartMs);
    EXPECT_DOUBLE_EQ(controller.glide().time.end.drawnStartMs, original.time.end.drawnStartMs);
    EXPECT_DOUBLE_EQ(controller.glide().price.from.drawnMinPrice, original.price.from.drawnMinPrice - 2);
    EXPECT_DOUBLE_EQ(controller.glide().price.end.drawnMinPrice, original.price.end.drawnMinPrice - 2);
    EXPECT_EQ(controller.glide().rebases, 0);
    policy.endZoomGlide();
    const auto ended = controller.glide();
    policy.shiftTime(100);
    policy.shiftBoth(20, 8);
    EXPECT_DOUBLE_EQ(controller.glide().time.from.drawnStartMs, ended.time.from.drawnStartMs);
    EXPECT_DOUBLE_EQ(controller.glide().price.end.drawnMinPrice, ended.price.end.drawnMinPrice);
}

TEST_F(ChartCameraTest, DetachedCameraDoesNotProduceViews) {
    port.isAttached = false;
    auto& camera = controller.camera();
    EXPECT_FALSE(camera.rasterCameraNow().valid);
    EXPECT_FALSE(camera.restCameraNow().valid);
    EXPECT_FALSE(camera.intendedEndNow().valid);
    qint64 time = 77;
    double price = 88;
    EXPECT_FALSE(camera.panShift({1, 2}, time, price));
    EXPECT_EQ(time, 77);
    EXPECT_DOUBLE_EQ(price, 88);
    EXPECT_DOUBLE_EQ(camera.tickPredictor(1000, 2000, 120, 0.5)(40), 0);
    EXPECT_EQ(camera.drawnTimeWindow(1000, 2000), std::make_pair(1000.0, 2000.0));
}

} // namespace
