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
