#include "chart/core/ChartViewController.hpp"

#include <gtest/gtest.h>

namespace {

struct FakeEnvironment final : chart::IChartEnvironment {
    bool isAttached = true;
    mutable int limitsReads = 0;
    mutable int recentMidReads = 0;
    double mid = 103.0;

    bool attached() const override { return isAttached; }
    chart::SurfaceFacts surface() const override { return {}; }
    double drawnTick() const override { return 0.0; }
    double priceScale() const override { return 0.0; }
    bool manualTick() const override { return false; }
    bool hasCurrentResolution() const override { return false; }
    bool priceScaleCurrent() const override { return false; }
    chart::SpanLimits limits() const override {
        ++limitsReads;
        return {5.0, 1000.0, 0.25, 100.0};
    }
    int64_t liveAnchorMs() const override { return 0; }
    int64_t liveOpenEndMs() const override { return 0; }
    int64_t oldestAvailableMs() const override { return 0; }
    double recentMidPrice() const override {
        ++recentMidReads;
        return mid;
    }
    std::optional<heatmap::gpu::ViewWindow> restView() const override { return std::nullopt; }
};

struct FakeSink final : chart::IHeatmapViewSink {
    int setViewCalls = 0;
    void setView(const heatmap::gpu::ViewWindow&, bool) override { ++setViewCalls; }
};

TEST(ChartViewController, DetachedEnvironmentDoesNotApplyLimitsOrPublishView) {
    FakeEnvironment environment;
    FakeSink sink;
    chart::ChartViewController controller(environment, sink);
    controller.viewPtr()->setViewport(100, 200, 10.0, 20.0);
    environment.isAttached = false;

    controller.applyGpuLimits();
    controller.syncGpuView();

    EXPECT_EQ(environment.limitsReads, 0);
    EXPECT_EQ(controller.viewPtr()->minTimeSpanMs(), 0.0);
    EXPECT_EQ(controller.viewPtr()->maxTimeSpanMs(), 0.0);
    EXPECT_EQ(sink.setViewCalls, 0);
}

TEST(ChartViewController, LivePriceUsesBookThenTradeThenRecentMidThenDetachedZero) {
    FakeEnvironment environment;
    FakeSink sink;
    chart::ChartViewController controller(environment, sink);
    controller.session().bookMid = 101.0;
    controller.session().lastTrade = 102.0;

    EXPECT_DOUBLE_EQ(controller.gpuLivePrice(), 101.0);
    controller.session().bookMid = 0.0;
    EXPECT_DOUBLE_EQ(controller.gpuLivePrice(), 102.0);
    EXPECT_EQ(environment.recentMidReads, 0);

    controller.session().lastTrade = 0.0;
    EXPECT_DOUBLE_EQ(controller.gpuLivePrice(), 103.0);
    EXPECT_EQ(environment.recentMidReads, 1);
    environment.isAttached = false;
    EXPECT_DOUBLE_EQ(controller.gpuLivePrice(), 0.0);
    EXPECT_EQ(environment.recentMidReads, 1);
}

} // namespace
