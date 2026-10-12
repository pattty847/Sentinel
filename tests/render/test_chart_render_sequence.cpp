#include "chart/layers/RenderSequence.hpp"
#include "render/heatmap/LabelRequestCoverage.hpp"
#include "render/heatmap/LabelRetryPolicy.hpp"
#include <gtest/gtest.h>
#include <atomic>
#include <memory>
#include <string>
#include <vector>
namespace {
using Calls = std::vector<std::string>;
TEST(HeatmapLayerSequence, GatePrepareAndSubmit) {
    Calls calls;
    struct Gate {
        Calls& calls; double value = 1;
        double opacity() const { return value; }
        void setOpacity(double v) { value = v; calls.push_back(v ? "open" : "close"); }
    } gate{calls};
    struct Tile { Calls& calls; void setFrame(int&&) { calls.push_back("submit"); } } tile{calls};
    int frame = 0;
    bool succeeds = true;
    const auto prepare = [&](int&) { calls.push_back("prepare"); return succeeds; };
    EXPECT_FALSE(chart::prepareHeatmap(false, true, prepare, gate, tile, frame));
    EXPECT_EQ(calls, (Calls{"close"})); calls.clear();
    EXPECT_FALSE(chart::prepareHeatmap(true, false, prepare, gate, tile, frame));
    EXPECT_TRUE(calls.empty());
    EXPECT_TRUE(chart::prepareHeatmap(true, true, prepare, gate, tile, frame));
    EXPECT_EQ(calls, (Calls{"prepare", "open", "submit"})); calls.clear();
    succeeds = false;
    EXPECT_FALSE(chart::prepareHeatmap(true, true, prepare, gate, tile, frame));
    EXPECT_EQ(calls, (Calls{"prepare", "close"}));
}
TEST(ChartTextLayerSequence, BeginAxisLabelsEnd) {
    Calls calls;
    struct Text { Calls& calls; void endFrame() { calls.push_back("end"); } } text{calls};
    chart::submitChartText(text, [&] { calls.push_back("begin"); }, [&] { calls.push_back("axis"); },
        [&] { calls.push_back("labels"); }, [&](int stage) { calls.push_back("mark" + std::to_string(stage)); });
    EXPECT_EQ(calls, (Calls{"begin", "axis", "mark0", "labels", "mark1", "end", "mark2"}));
}

TEST(HeatmapLabelSequence, SamePictureAndMarginSuppressRepost) {
    struct Request {
        int64_t tfMs, tickUnits; double priceScale;
        uint64_t spanVersion, liveVersion;
        int64_t firstBucket, firstBin; uint32_t columns, rows;
    } p{};
    p.tfMs = 60000; p.tickUnits = 100; p.priceScale = 100;
    p.spanVersion = 4; p.liveVersion = 5;
    p.firstBucket = 10; p.columns = 20; p.firstBin = 100; p.rows = 40;
    const heatmap::gpu::LabelPicture same{60000, 100, 100, 4, 5};
    const std::array<double, 4> window{11, 29, 101, 139};
    EXPECT_TRUE(heatmap::gpu::labelRequestCovers(p, same, window));
    auto changed = same; changed.tf *= 2;
    EXPECT_FALSE(heatmap::gpu::labelRequestCovers(p, changed, window));
    changed = same; changed.tick *= 2;
    EXPECT_FALSE(heatmap::gpu::labelRequestCovers(p, changed, window));
    changed = same; ++changed.spanVersion;
    EXPECT_FALSE(heatmap::gpu::labelRequestCovers(p, changed, window));
    changed = same; ++changed.liveVersion;
    EXPECT_FALSE(heatmap::gpu::labelRequestCovers(p, changed, window));
    EXPECT_FALSE(heatmap::gpu::labelRequestCovers(p, same, {9, 29, 101, 139}));
    EXPECT_FALSE(heatmap::gpu::labelRequestCovers(p, same, {11, 31, 101, 139}));
    EXPECT_FALSE(heatmap::gpu::labelRequestCovers(p, same, {11, 29, 99, 139}));
    EXPECT_FALSE(heatmap::gpu::labelRequestCovers(p, same, {11, 29, 101, 141}));
}
TEST(HeatmapLabelSequence, RetryBackoffCapsAndResets) {
    heatmap::gpu::LabelRetryPolicy retry;
    std::vector<int> delays{retry.delayMs()};
    for (int i = 0; i < 6; ++i) { retry.failed(); delays.push_back(retry.delayMs()); }
    EXPECT_EQ(delays, (std::vector<int>{2000, 4000, 8000, 16000, 30000, 30000, 30000}));
    retry.succeeded(); EXPECT_EQ(retry.delayMs(), 2000);
}
TEST(HeatmapLayerSequence, AfterRenderingConsumesFlagsInOrderWithoutPollingAgain) {
    struct Stats { std::atomic<bool> wantsFrame{true}; };
    auto stats = std::make_shared<Stats>();
    std::atomic<bool> incomplete{true};
    Calls calls;
    const auto update = [&] { calls.push_back("queue frame"); };
    const auto labels = [&] { calls.push_back("queue labels"); };
    chart::finishHeatmapFrame(stats, incomplete, update, labels);
    EXPECT_EQ(calls, (Calls{"queue frame", "queue labels"}));
    calls.clear();
    chart::finishHeatmapFrame(stats, incomplete, update, labels);
    EXPECT_TRUE(calls.empty());
    stats.reset(); incomplete.store(true);
    chart::finishHeatmapFrame(stats, incomplete, update, labels);
    EXPECT_EQ(calls, (Calls{"queue labels"}));
}
} // namespace
