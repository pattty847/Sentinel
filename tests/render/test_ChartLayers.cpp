#include "chart/layers/ChartLayers.hpp"
#include "render/heatmap/HeatmapGpuLayer.hpp"
#include "chart/feeds/TradeOverlaySequence.hpp"
#include <gtest/gtest.h>
#include "render/FrameContextBuilder.hpp"
#include <QElapsedTimer>
#include <memory>
#include <string>
#include <vector>

#include "chart/core/ChartFrame.hpp"
#include "render/ChartTextRenderer.hpp"

namespace {
using Calls = std::vector<std::string>;
struct Layer : chart::IChartLayer {
    std::string id;
    Calls& calls;
    Layer(std::string id, Calls& calls) : id(std::move(id)), calls(calls) {}
    std::string_view name() const override { return id; }
    void onRootRebuilt() override { calls.push_back("root:" + id); }
    void sync(const chart::ChartFrame&) override { calls.push_back("sync:" + id); }
    void updateNodes(const chart::ChartFrame&, chart::LayerNodeContext&) override { calls.push_back(id); }
    void afterRendering() override { calls.push_back("after:" + id); }
};
void update(chart::LayerRegistry& registry) {
    chart::ChartFrame frame;
    ChartTextRenderer text;
    ChartTextAtlas atlas;
    registry.updateNodes(frame, {nullptr, nullptr, text, atlas, false});
}
TEST(LayerRegistry, DispatchPreservesRegistrationOrder) {
    Calls calls;
    Layer a("a", calls), b("b", calls);
    chart::LayerRegistry registry;
    registry.add(b); registry.add(a);
    chart::ChartFrame frame;
    registry.sync(frame);
    update(registry);
    registry.afterRendering();
    EXPECT_EQ(calls, (Calls{"sync:b", "sync:a", "b", "a", "after:b", "after:a"}));
}
TEST(ChartLayerSet, RegistersHeatmapFootprintVpTpoLadderText) {
    chart::ChartLayers layers;
    ASSERT_TRUE(layers.registry.precedes("heatmap", "text"));
    EXPECT_TRUE(layers.registry.precedes("TPO", "ladder"));
    EXPECT_TRUE(layers.registry.precedes("ladder", "text")) << "chart text draws over the ladder";
    EXPECT_FALSE(layers.registry.precedes("ladder", "TPO"));
    layers.heatmap.initialize();
    QSGNode root;
    layers.registry.onRootRebuilt(&root);
    EXPECT_EQ(root.childCount(), 6);
    EXPECT_EQ(root.childAtIndex(4)->type(), QSGNode::OpacityNodeType) << "the ladder's gate";
    EXPECT_EQ(root.firstChild()->type(), QSGNode::OpacityNodeType);
    EXPECT_EQ(root.firstChild()->firstChild()->type(), QSGNode::RenderNodeType);
}
struct Processor {
    Calls& calls;
    ~Processor() { calls.push_back("destroy processor"); }
    void stopProcessing() { calls.push_back("stop direct"); }
};
struct Thread {
    Calls& calls;
    bool running = true, waitSucceeds = true;
    ~Thread() { calls.push_back("destroy thread"); }
    bool isRunning() { return running; }
    void quit() { calls.push_back("quit"); }
    bool wait(int ms) { calls.push_back("wait " + std::to_string(ms)); return waitSucceeds; }
    void terminate() { calls.push_back("terminate"); }
};
Calls teardown(bool running, bool waitSucceeds) {
    Calls calls;
    auto processor = std::make_unique<Processor>(calls);
    auto thread = std::make_unique<Thread>(calls, running, waitSucceeds);
    chart::teardownTradeOverlayFeeds(processor, thread,
        [&](Processor*) { calls.push_back("stop blocking"); },
        [&](Processor*) { calls.push_back("disconnect"); });
    return calls;
}
TEST(TradeOverlayFeeds, TeardownStopsBeforeDisconnectAndJoin) {
    EXPECT_EQ(teardown(true, true), (Calls{"stop blocking", "disconnect", "quit", "wait 5000",
                                          "destroy processor", "destroy thread"}));
    EXPECT_EQ(teardown(true, false), (Calls{"stop blocking", "disconnect", "quit", "wait 5000", "terminate",
                                           "wait 1000", "destroy processor", "destroy thread"}));
    EXPECT_EQ(teardown(false, true), (Calls{"stop direct", "disconnect", "destroy processor", "destroy thread"}));
}
struct Mailbox {
    Calls& calls;
    std::string name;
    void clearPending() { calls.push_back("clear " + name); }
    void requestNeutralReset() { calls.push_back("neutral " + name); }
};
TEST(TradeOverlayFeeds, ClearQueuesProcessorBeforeMailboxesAndNeutralReset) {
    Calls calls;
    auto processor = std::make_unique<Processor>(calls);
    Mailbox footprint{calls, "footprint"}, vp{calls, "VP"};
    chart::clearTradeOverlayFeeds(processor, footprint, vp,
        [&](Processor*) { calls.push_back("queue clear"); });
    EXPECT_EQ(calls, (Calls{"queue clear", "clear footprint", "clear VP", "neutral footprint"}));
    processor.reset(); calls.clear();
    chart::clearTradeOverlayFeeds(processor, footprint, vp, [&](Processor*) { ADD_FAILURE(); });
    EXPECT_EQ(calls, (Calls{"clear footprint", "clear VP", "neutral footprint"}));
}
} // namespace

TEST(FrameContextBuilder, CapturesEveryOverlayFlag) {
    QElapsedTimer clock;
    TimeAuthority time;
    for (int bits = 0; bits < 16; ++bits) {
        const FrameContext::OverlayActivationSet flags{bool(bits & 1), bool(bits & 2), bool(bits & 4), bool(bits & 8)};
        const auto frame = FrameContextBuilder::build({}, 1, clock, time, nullptr, flags);
        EXPECT_EQ(frame.overlays.heatmap, flags.heatmap);
        EXPECT_EQ(frame.overlays.footprint, flags.footprint);
        EXPECT_EQ(frame.overlays.tpo, flags.tpo);
        EXPECT_EQ(frame.overlays.volumeProfile, flags.volumeProfile);
    }
}
TEST(HeatmapLabelSequence, DrawSignature) {
    heatmap::LabelCells labels;
    labels.key.serial = 10;
    const std::vector<uint8_t> columns{1, 0, 1};
    using Layer = heatmap::gpu::HeatmapGpuLayer;
    const auto signature = Layer::labelSignature(&labels, columns, 2);
    EXPECT_EQ(signature, Layer::labelSignature(&labels, columns, 2));
    EXPECT_NE(signature, Layer::labelSignature(&labels, {1, 1, 1}, 3));
    EXPECT_EQ(Layer::labelSignature(nullptr, columns, 2), 0u);

}

#include "chart/feeds/TradeBubbleFeed.hpp"
TEST(TradeBubbleFeed, TapeSettingsAndPublicationStayTogether) {
    chart::TradeBubbleFeed feed;
    Trade trade{};
    trade.trade_id = "one"; trade.price = 10; trade.size = 2;
    trade.timestamp = std::chrono::system_clock::time_point(std::chrono::milliseconds(100));
    EXPECT_FALSE(feed.append(trade)); // hidden tape still ingests
    const auto frame = feed.tradeBubbleRenderFrame();
    ASSERT_EQ(frame->tape->samples().size(), 1u);
    EXPECT_FALSE(feed.append(trade));
    EXPECT_EQ(frame->tape->samples().size(), 1u);
    heatmap::HeatmapChartSettings settings;
    settings.showTrades = true; settings.tradeMinNotional = 30;
    settings.tradesAboveCandles = false;
    int notified = 0;
    feed.setSettings(settings, [&] { ++notified; EXPECT_FALSE(feed.tradesAboveCandles()); });
    feed.setSettings(settings, [&] { ++notified; });
    EXPECT_EQ(notified, 1);
    TimeAxisMapping mapping; mapping.viewStartMs = 42;
    feed.publish(mapping);
    EXPECT_EQ(feed.tradeBubbleRenderFrame(), frame);
    EXPECT_TRUE(frame->enabled); EXPECT_EQ(frame->minNotional, 30);
    EXPECT_EQ(frame->mapping.viewStartMs, 42);
    feed.disableFrame(); EXPECT_FALSE(frame->enabled);
    feed.clear(); EXPECT_TRUE(frame->tape->samples().empty());
}

TEST(HeatmapLayer, AfterRenderingWithoutHostCallbacksConsumesFrameRequest) {
    chart::HeatmapLayer layer;
    EXPECT_NO_THROW(layer.afterRendering());
    layer.initialize();
    auto stats = layer.gpu->tileStatsPtr();
    stats->wantsFrame.store(true);
    EXPECT_NO_THROW(layer.afterRendering());
    EXPECT_FALSE(stats->wantsFrame.load());
    EXPECT_NO_THROW(layer.afterRendering());
}
