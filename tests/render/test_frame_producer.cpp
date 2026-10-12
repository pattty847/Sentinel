#include "chart/core/ChartViewController.hpp"
#include "chart/core/FrameProducer.hpp"
#include "chart/core/FrameAck.hpp"
#include "chart/core/PublishedFrame.hpp"
#include <QElapsedTimer>
#include <gtest/gtest.h>
#include <cstdlib>
#include <new>

namespace {
thread_local bool countAllocations = false;
thread_local size_t allocations = 0;
}
void* operator new(std::size_t size) {
    if (countAllocations) ++allocations;
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }

struct TickFake : chart::ITickAuthority {
    double tick = 2;
    double tickAfterChoice = 0;
    int chooses = 0, notes = 0;
    mutable int reads = 0;
    int row = 0, col = 0;
    double continuousRow = 0;
    heatmap::gpu::ViewWindow chosen{};
    void chooseTickForView(const heatmap::gpu::ViewWindow& view) override {
        ++chooses;
        chosen = view;
        if (tickAfterChoice > 0) tick = tickAfterChoice;
    }
    double tickPrice() const override { ++reads; return tick; }
    void noteRaster(int r, int c, double continuous) override { ++notes; row = r; col = c; continuousRow = continuous; }
};

namespace {
struct Environment final : chart::IChartEnvironment, chart::ITickOracle, chart::IHeatmapViewSink {
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


struct FrameProducerTest : testing::Test {
    Environment environment;
    chart::PublishedFrame published;
    chart::ChartViewController controller{environment, environment, environment, published};
    QElapsedTimer clock;
    TimeAuthority time;
    TickFake ticks;
    chart::FrameInputs inputs;
    chart::FrameProducer producer;
    void SetUp() override {
        controller.session().timeframeMs = 100;
        controller.viewPtr()->setAutoPriceScale(false);
        controller.viewPtr()->setViewportSize(100, 80);
        controller.viewPtr()->setViewport(1000, 2000, 100, 140);
        time.setActiveTimeframeMs(100);
        time.observeEventTime(1875, 0);
        inputs = capture();
    }
    chart::FrameInputs capture() {
        return controller.frameInputs({0, 0, 100, 80}, 2, clock, time, {true, true, true});
    }
    chart::ChartFrame produce() { return producer.produce(inputs, ticks, published); }
};

TEST_F(FrameProducerTest, CapturedInputsSurviveGuiMutation) {
    controller.viewPtr()->setViewport(3000, 5000, 200, 280);
    controller.session().timeframeMs = 500;
    controller.glide().gesture = controller.glide().gestureTime = true;
    const auto frame = produce();
    EXPECT_EQ(frame.context.viewport.timeStart, 1000);
    EXPECT_DOUBLE_EQ(frame.context.viewport.minPrice, 100);
    EXPECT_DOUBLE_EQ(frame.context.raster.drawnStartMs, 1000);
    EXPECT_DOUBLE_EQ(frame.context.raster.drawnEndMs, 2000);
    EXPECT_DOUBLE_EQ(frame.context.raster.drawnMinPrice, 100);
    EXPECT_DOUBLE_EQ(frame.context.raster.drawnMaxPrice, 140);
    EXPECT_DOUBLE_EQ(frame.context.raster.tfMs, 100);
    EXPECT_FALSE(frame.context.raster.free);
    EXPECT_DOUBLE_EQ(frame.coverageGamma, 0);
    EXPECT_FALSE(frame.binView);
}

TEST_F(FrameProducerTest, FixedMappingPublicationAndSurfaceAnnouncements) {
    ticks.tick = 1;
    ticks.tickAfterChoice = 2;
    auto frame = produce();
    ticks.tickAfterChoice = 0;
    ASSERT_TRUE(frame.ready);
    ASSERT_TRUE(frame.drawHeatmap);
    EXPECT_TRUE(frame.rasterChanged);
    EXPECT_EQ(published.step(), (chart_raster::RasterStep{8, 20}));
    EXPECT_EQ(ticks.chooses, 1);
    EXPECT_EQ(ticks.reads, 1);
    EXPECT_EQ(ticks.notes, 1);
    EXPECT_DOUBLE_EQ(ticks.chosen.timeLoMs, 1000);
    EXPECT_DOUBLE_EQ(ticks.chosen.timeHiMs, 2000);
    EXPECT_DOUBLE_EQ(ticks.chosen.priceLo, 100);
    EXPECT_DOUBLE_EQ(ticks.chosen.priceHi, 140);
    EXPECT_EQ(ticks.row, 8);
    EXPECT_EQ(ticks.col, 20);
    EXPECT_DOUBLE_EQ(ticks.continuousRow, 8);
    const auto context = published.current();
    EXPECT_EQ(context.surfaceBounds, QRectF(0, 0, 100, 80));
    EXPECT_DOUBLE_EQ(context.surfaceDpr, 2);
    EXPECT_EQ(context.activeTimeframeMs, 100);
    EXPECT_EQ(context.nowEventTimeMs, 1875);
    EXPECT_EQ(context.presentationTimeMs, frame.context.presentationTimeMs);
    EXPECT_EQ(context.currentBoundaryStartMs, 1800);
    EXPECT_EQ(context.nextBoundaryStartMs, 1900);
    EXPECT_EQ(context.boundarySequence, frame.context.time.boundarySequence);
    EXPECT_TRUE(context.hasEventTime);
    EXPECT_TRUE(context.viewportValid);
    EXPECT_EQ(context.viewportTimeStart, 1000);
    EXPECT_EQ(context.viewportTimeEnd, 2000);
    EXPECT_DOUBLE_EQ(context.viewportMinPrice, 100);
    EXPECT_DOUBLE_EQ(context.viewportMaxPrice, 140);
    EXPECT_DOUBLE_EQ(context.mapping.viewStartMs, 1000);
    EXPECT_DOUBLE_EQ(context.mapping.viewEndMs, 2000);
    EXPECT_DOUBLE_EQ(context.mapping.viewMinPrice, 100);
    EXPECT_DOUBLE_EQ(context.mapping.viewMaxPrice, 140);
    EXPECT_DOUBLE_EQ(context.raster.drawnStartMs, 1000);
    EXPECT_FALSE(produce().rasterChanged);
    ticks.tick = 4;
    EXPECT_TRUE(produce().rasterChanged);
    EXPECT_EQ(published.step(), (chart_raster::RasterStep{16, 20}));
    EXPECT_FALSE(produce().rasterChanged);
    inputs.camera.raster.dpr = 1;
    EXPECT_TRUE(produce().rasterChanged);
    EXPECT_FALSE(produce().rasterChanged);
    inputs.camera.raster.itemWidthLogical = 101;
    EXPECT_TRUE(produce().rasterChanged);
}

TEST_F(FrameProducerTest, ContinuousDragChoosesTickBeforeRasterAndHiddenSkipsChoice) {
    controller.viewPtr()->handlePanStart({0, 0});
    controller.viewPtr()->handlePanMove({2.75, -1.25});
    inputs = capture();
    auto frame = produce();
    EXPECT_DOUBLE_EQ(ticks.chosen.timeLoMs, 972.5);
    EXPECT_DOUBLE_EQ(ticks.chosen.priceLo, 99.375);
    EXPECT_DOUBLE_EQ(frame.context.raster.drawnStartMs, 970);
    EXPECT_DOUBLE_EQ(frame.context.raster.drawnMinPrice, 99.25);
    EXPECT_EQ(published.current().viewportPanVisualOffset, QPointF(2.75, -1.25));
    EXPECT_TRUE(published.current().viewportDragging);
    inputs.context.overlays.heatmap = false;
    EXPECT_FALSE(produce().drawHeatmap);
    EXPECT_EQ(ticks.chooses, 1);
    EXPECT_EQ(ticks.reads, 2);
    EXPECT_EQ(ticks.notes, 2);
}

TEST_F(FrameProducerTest, InvalidCameraDoesNotChangeStepOrAnnounce) {
    produce();
    inputs.camera.raster = {};
    inputs.context.viewport.valid = false;
    const auto frame = produce();
    EXPECT_FALSE(frame.context.raster.valid);
    EXPECT_FALSE(frame.drawHeatmap);
    EXPECT_FALSE(frame.rasterChanged);
    EXPECT_EQ(published.step(), (chart_raster::RasterStep{8, 20}));
    EXPECT_EQ(ticks.notes, 1);
    EXPECT_FALSE(published.current().viewportValid);
}

TEST_F(FrameProducerTest, ProduceAllocatesNothing) {
    allocations = 0;
    countAllocations = true;
    produce();
    produce();
    ticks.tick = 4;
    produce();
    countAllocations = false;
    EXPECT_EQ(allocations, 0u);
    std::printf("produce allocation count: %zu (first, unchanged, tick change)\n", allocations);
}

TEST(FrameAckTest, ControlsSubmissionAndRenderedIdsStaySeparate) {
    chart::FrameAck ack;
    FrameContext frame;
    ack.setAgentControlRevision(7, 8, 9);
    ack.stamp(frame);
    EXPECT_EQ(frame.controlRevision, 7);
    EXPECT_EQ(frame.selectionEpoch, 8);
    EXPECT_EQ(frame.viewportVersion, 9);
    EXPECT_EQ(frame.frameId, 1);
    ack.submitted(frame);
    ack.setAgentControlRevision(17, 18, 19);
    EXPECT_EQ(ack.renderedControlRevision(), 0);
    EXPECT_EQ(ack.renderedFrameId(), 0);
    ack.rendered();
    EXPECT_EQ(ack.renderedControlRevision(), 7);
    EXPECT_EQ(ack.renderedFrameId(), 1);
    ack.stamp(frame);
    EXPECT_EQ(frame.controlRevision, 17);
    EXPECT_EQ(frame.selectionEpoch, 18);
    EXPECT_EQ(frame.viewportVersion, 19);
    EXPECT_EQ(frame.frameId, 2);
    ack.rendered();
    EXPECT_EQ(ack.renderedFrameId(), 1);
    ack.submitted(frame);
    ack.rendered();
    EXPECT_EQ(ack.renderedControlRevision(), 17);
    EXPECT_EQ(ack.renderedFrameId(), 2);
}
} // namespace
