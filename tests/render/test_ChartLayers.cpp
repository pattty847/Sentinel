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
    EXPECT_EQ(root.childCount(), 7);
    EXPECT_EQ(root.childAtIndex(5)->type(), QSGNode::OpacityNodeType) << "the ladder's gate";
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
    EXPECT_TRUE(frame->enabled); EXPECT_EQ(frame->options.minimum, 30);
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

// Flow-backed footprint cells: independent of the ladder cases above.
#include "chart/feeds/FlowFeed.hpp"
#include "chart/layers/FootprintCellsLayer.hpp"
#include "chart/shell/LayerVisibility.hpp"
#include "mainwindow/ChartAgentPort.hpp"
#include "render/heatmap/HeatmapSettingsStore.hpp"
#include <QSGOpacityNode>
#include <QSGGeometryNode>
#include <QTemporaryDir>
#include <atomic>
#include <cstdlib>
#include <new>

namespace {
std::atomic<bool> footprintCounting{false};
std::atomic<size_t> footprintAllocations{0};
void* footprintAllocate(std::size_t size) {
    if (footprintCounting.load(std::memory_order_relaxed)) ++footprintAllocations;
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
}
void* operator new(std::size_t n) { return footprintAllocate(n); }
void* operator new[](std::size_t n) { return footprintAllocate(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {
MappingFrameContext footprintFrame() {
    MappingFrameContext f;
    f.activeTimeframeMs = 1000;
    f.surfaceBounds = {0, 0, 112, 120};
    f.surfaceDpr = 1;
    f.viewportValid = true;
    auto& r = f.raster;
    r.valid = true; r.dpr = 1; r.tick = 1; r.tfMs = 1000;
    r.rowPx = 12; r.rowPxF = 12; r.colPx = 56; r.colPxF = 56;
    r.drawnStartMs = 0; r.drawnEndMs = 2000; r.drawnMinPrice = 95; r.drawnMaxPrice = 105;
    r.topF = 1260; r.topRowIndex = 1260; r.heightDev = 120; r.widthDev = 112;
    return f;
}
FlowSeriesBuffer::Series footprintSeries(double nativeTick = 0.5) {
    FlowSeriesBuffer::Series series;
    auto& b = series.bars[0].bar;
    b.startMs = 0; b.timeframeMs = 1000; b.nativeTick = nativeTick;
    b.baseScale = 100; b.lowRow = int64_t(100 / nativeTick);
    b.rows = {{0, 110, 210}, {1, 130, 230}, {2, 170, 270}};
    b.sellVol = 410; b.buyVol = 710; b.delta = 300; b.maxRowVol = 440;
    return series;
}
chart::ChartFrame footprintChartFrame() {
    chart::ChartFrame frame;
    const auto source = footprintFrame();
    frame.context.raster = source.raster;
    frame.context.surfaceBounds = source.surfaceBounds;
    frame.context.surfaceDpr = source.surfaceDpr;
    frame.context.time.activeTimeframeMs = source.activeTimeframeMs;
    frame.context.itemBounds = frame.context.surfaceBounds;
    frame.context.overlays.footprint = true;
    return frame;
}
TEST(FootprintCells, BinsToDrawnTickAndRetainsExactTotals) {
    auto out = std::make_unique<chart::FlowSnapshot>();
    ASSERT_TRUE(chart::FlowFeed::bin(*out, footprintSeries(), footprintFrame()));
    ASSERT_EQ(out->barCount, 1); ASSERT_EQ(out->cellCount, 2);
    EXPECT_EQ(out->cells[0].row, 100); EXPECT_EQ(out->cells[0].span, 1);
    EXPECT_EQ(out->cells[0].bidVol, 240); EXPECT_EQ(out->cells[0].askVol, 440);
    EXPECT_EQ(out->cells[1].bidVol, 170); EXPECT_EQ(out->cells[1].askVol, 270);
    EXPECT_EQ(out->bars[0].bidVol, 410); EXPECT_EQ(out->bars[0].askVol, 710);
    EXPECT_EQ(out->bars[0].delta, 300); EXPECT_EQ(out->bars[0].maxRowVol, 680);
}
TEST(FootprintCells, CoarseNativeBinSpansRowsWithoutInventingVolume) {
    auto out = std::make_unique<chart::FlowSnapshot>();
    ASSERT_TRUE(chart::FlowFeed::bin(*out, footprintSeries(2), footprintFrame()));
    ASSERT_EQ(out->cellCount, 3);
    for (int i = 0; i < out->cellCount; ++i) {
        EXPECT_EQ(out->cells[size_t(i)].row, 100 + i * 2);
        EXPECT_EQ(out->cells[size_t(i)].span, 2);
    }
    EXPECT_EQ(out->cells[0].bidVol, 110); EXPECT_EQ(out->cells[0].askVol, 210);
    EXPECT_EQ(out->bars[0].bidVol, 410);
}
TEST(FootprintCells, OffscreenRowsRemainInBarTotalsAndBarMaximum) {
    auto out = std::make_unique<chart::FlowSnapshot>();
    auto frame = footprintFrame(); frame.raster.drawnMinPrice = 100; frame.raster.drawnMaxPrice = 100.25;
    ASSERT_TRUE(chart::FlowFeed::bin(*out, footprintSeries(), frame));
    EXPECT_EQ(out->cellCount, 1);
    EXPECT_EQ(out->bars[0].bidVol, 410); EXPECT_EQ(out->bars[0].askVol, 710);
    EXPECT_EQ(out->bars[0].maxRowVol, 680);
}
TEST(FootprintCells, WindowNormalisationAccountsForEachBarsScale) {
    auto out = std::make_unique<chart::FlowSnapshot>();
    auto series = footprintSeries();
    auto second = series.bars[0];
    second.bar.startMs = 1000; second.bar.baseScale = 50;
    series.bars[1000] = second;
    ASSERT_TRUE(chart::FlowFeed::bin(*out, series, footprintFrame()));
    EXPECT_DOUBLE_EQ(chart::FootprintCellsLayer::intensity(out->bars[0], out->cells[0], *out, false), 1);
    EXPECT_DOUBLE_EQ(chart::FootprintCellsLayer::intensity(out->bars[0], out->cells[0], *out, true), 0.5);
    EXPECT_DOUBLE_EQ(chart::FootprintCellsLayer::intensity(out->bars[1], out->cells[2], *out, true), 1);
    chart::FootprintCellsLayer layer;
    layer.presentation().enabled = true; layer.presentation().cellsSource = true;
    layer.mailbox().back() = *out; layer.mailbox().post();
    std::unique_ptr<QSGNode> root(layer.createContainer());
    ChartTextRenderer text; ChartTextAtlas atlas;
    chart::LayerNodeContext context{nullptr, root.get(), text, atlas, false};
    auto frame = footprintChartFrame();
    layer.sync(frame); layer.updateNodes(frame, context);
    auto* geometry = static_cast<QSGGeometryNode*>(root->firstChild())->geometry();
    const auto before = geometry->vertexDataAsColoredPoint2D()[0].a;
    layer.presentation().windowIntensity = true;
    layer.sync(frame); layer.updateNodes(frame, context);
    EXPECT_EQ(before, 230);
    EXPECT_EQ(geometry->vertexDataAsColoredPoint2D()[0].a, 115);
}
TEST(FootprintCells, InvalidBarDoesNotWithdrawCompatibleBars) {
    auto out = std::make_unique<chart::FlowSnapshot>();
    auto series = footprintSeries(0.3);
    auto good = footprintSeries().bars[0]; good.bar.startMs = 1000;
    series.bars[1000] = good;
    ASSERT_TRUE(chart::FlowFeed::bin(*out, series, footprintFrame()));
    EXPECT_TRUE(out->valid); EXPECT_EQ(out->skippedBars, 1);
    ASSERT_EQ(out->barCount, 1); EXPECT_EQ(out->bars[0].startMs, 1000);
    EXPECT_EQ(out->bars[0].bidVol, 410); EXPECT_EQ(out->cellCount, 2);
}
TEST(FootprintCells, CapacityRetainsNearestBarsAndNeverPartialBarCells) {
    auto out = std::make_unique<chart::FlowSnapshot>();
    auto series = footprintSeries();
    auto frame = footprintFrame(); frame.raster.drawnEndMs = 600000;
    for (int i = -600; i < 1200; ++i) {
        series.bars[i * 1000] = series.bars[0]; series.bars[i * 1000].bar.startMs = i * 1000;
    }
    ASSERT_TRUE(chart::FlowFeed::bin(*out, series, frame));
    EXPECT_TRUE(out->valid); EXPECT_EQ(out->barCount, chart::FlowSnapshot::kMaxBars);
    EXPECT_GT(out->truncatedBars, 0);
    EXPECT_EQ(out->bars[0].startMs, 44000);
    EXPECT_EQ(out->bars[size_t(out->barCount - 1)].startMs, 555000);
    // The nearest oversized bar cannot corrupt the following small bar's cells.
    series = footprintSeries();
    auto huge = series.bars[0]; huge.bar.rows.resize(chart::FlowSnapshot::kMaxCells + 1);
    series.bars[0] = huge;
    auto small = footprintSeries().bars[0]; small.bar.startMs = 1000; series.bars[1000] = small;
    ASSERT_TRUE(chart::FlowFeed::bin(*out, series, footprintFrame()));
    EXPECT_EQ(out->truncatedBars, 1); ASSERT_EQ(out->barCount, 1);
    EXPECT_EQ(out->bars[0].firstCell, 0); EXPECT_EQ(out->cellCount, 2);
    EXPECT_EQ(out->cells[0].bidVol, 240);
}
TEST(FootprintCells, RegistrySlotAndNoData) {
    chart::ChartLayers layers;
    layers.heatmap.initialize();
    QSGNode root;
    layers.registry.onRootRebuilt(&root);
    EXPECT_EQ(root.childAtIndex(2)->type(), QSGNode::OpacityNodeType);
    EXPECT_EQ(root.childAtIndex(2)->firstChild()->type(), QSGNode::GeometryNodeType);
    auto frame = footprintChartFrame();
    layers.footprintCells.presentation().enabled = true;
    layers.footprintCells.sync(frame);
    EXPECT_FALSE(layers.footprintCells.drawing());
    EXPECT_EQ(layers.footprintCells.vertexCount(), 0);
}
TEST(FootprintCells, TwoFramesAfterDataAndChangedSnapshotAllocateNothing) {
    chart::FootprintCellsLayer layer;
    layer.presentation().enabled = true; layer.presentation().cellsSource = true;
    auto frame = footprintChartFrame();
    ASSERT_TRUE(chart::FlowFeed::bin(layer.mailbox().back(), footprintSeries(), footprintFrame()));
    layer.mailbox().post();
    std::unique_ptr<QSGNode> root(layer.createContainer());
    ChartTextRenderer text; ChartTextAtlas atlas;
    chart::LayerNodeContext context{nullptr, root.get(), text, atlas, false};
    layer.sync(frame); layer.updateNodes(frame, context);
    ASSERT_TRUE(layer.drawing()); ASSERT_EQ(layer.vertexCount(), 24);
    EXPECT_EQ(layer.stage(), chart::FootprintStage::Numbers);
    EXPECT_FALSE(layer.presentation().hollow(frame.context.raster)) << "only half of the window has flow history";
    // Fresh data and geometry every frame, not just a cache hit.
    for (int i = 0; i < 2; ++i) {
        ASSERT_TRUE(chart::FlowFeed::bin(layer.mailbox().back(), footprintSeries(), footprintFrame()));
        layer.mailbox().post();
        footprintAllocations = 0; footprintCounting = true;
        layer.sync(frame); layer.updateNodes(frame, context); layer.submitLabels(frame, context);
        footprintCounting = false;
        EXPECT_EQ(footprintAllocations.load(), 0u);
    }
    auto* geometry = static_cast<QSGGeometryNode*>(root->firstChild())->geometry();
    auto* vertices = geometry->vertexDataAsColoredPoint2D();
    EXPECT_FLOAT_EQ(vertices[0].x, 0); EXPECT_FLOAT_EQ(vertices[2].x, 28);
    layer.presentation().numbers = false; layer.sync(frame);
    EXPECT_EQ(layer.stage(), chart::FootprintStage::Cells);
    frame.context.raster.tick = 2; layer.sync(frame);
    EXPECT_FALSE(layer.drawing());
}
TEST(FootprintCells, SettingsPersistAndRejectInvalidVariantsAtomically) {
    QTemporaryDir dir;
    QSettings settings(dir.filePath("chart.ini"), QSettings::IniFormat);
    heatmap::HeatmapSettingsStore store(settings);
    heatmap::HeatmapChartSettings value;
    EXPECT_EQ(value.footprintSource, "q16");
    EXPECT_TRUE(heatmap::applySettingsPatch(value, {{"footprintSource", "cells"}, {"footprintIntensity", "window"},
        {"footprintHollowCandles", false}, {"footprintNumbers", false}}).isEmpty());
    store.save("test", value);
    auto loaded = store.load("test", ClientHeatmapConfig{});
    EXPECT_EQ(loaded.footprintSource, "cells");
    EXPECT_EQ(loaded.footprintIntensity, "window");
    EXPECT_FALSE(loaded.footprintHollowCandles); EXPECT_FALSE(loaded.footprintNumbers);
    EXPECT_FALSE(heatmap::applySettingsPatch(loaded, {{"footprintIntensity", "bad"}, {"footprintNumbers", true}}).isEmpty());
    EXPECT_EQ(loaded.footprintSource, "cells");
    EXPECT_EQ(loaded.footprintIntensity, "window"); EXPECT_FALSE(loaded.footprintNumbers);
    chart::LayerVisibility visibility;
    EXPECT_EQ(visibility.footprintSource(), "q16");
    EXPECT_TRUE(visibility.setProperty("footprintSource", "cells"));
    EXPECT_EQ(visibility.footprintSource(), "cells");
    EXPECT_FALSE(heatmap::applySettingsPatch(loaded, {{"footprintSource", "unknown"}}).isEmpty());
    EXPECT_EQ(loaded.footprintSource, "cells");
    EXPECT_TRUE(visibility.setProperty("footprintIntensity", "window"));
    EXPECT_TRUE(visibility.setProperty("footprintNumbers", false));
    EXPECT_TRUE(visibility.setProperty("footprintHollowCandles", false));
    EXPECT_EQ(visibility.footprintIntensity(), "window");
    EXPECT_FALSE(visibility.footprintNumbers()); EXPECT_FALSE(visibility.footprintHollowCandles());
}
} // namespace

#include "config/AgentHostMode.hpp"
TEST(FootprintCells, NumbersLayoutIsBoundedAndAllocatesNothingAfterWarmup) {
    QTemporaryDir cache;
    QString error;
    ASSERT_TRUE(AgentHostMode::activate(cache.path(), {}, &error)) << error.toStdString();
    struct Reset { ~Reset() { AgentHostMode::resetForTests(); } } reset;
    ChartTextAtlas atlas;
    ChartTextAtlas::BuildParams params;
    params.fontPath = QStringLiteral(SENTINEL_SOURCE_DIR "/resources/fonts/RobotoMono/RobotoMono-Regular.ttf");
    params.fontPx = 64; params.charset = "0123456789.kMe+-";
    ASSERT_TRUE(atlas.build(params));
    chart::FootprintCellsLayer layer;
    layer.presentation().enabled = true; layer.presentation().cellsSource = true;
    auto frame = footprintChartFrame();
    ASSERT_TRUE(chart::FlowFeed::bin(layer.mailbox().back(), footprintSeries(), footprintFrame()));
    layer.mailbox().post();
    std::unique_ptr<QSGNode> root(layer.createContainer());
    ChartTextRenderer text;
    chart::LayerNodeContext context{nullptr, root.get(), text, atlas, true};
    layer.sync(frame); layer.updateNodes(frame, context); layer.submitLabels(frame, context);
    ASSERT_GT(layer.glyphCount(), 0u);
    for (int i = 0; i < 2; ++i) {
        footprintAllocations = 0; footprintCounting = true;
        layer.sync(frame); layer.updateNodes(frame, context); layer.submitLabels(frame, context);
        footprintCounting = false;
        EXPECT_EQ(footprintAllocations.load(), 0u);
    }
    layer.presentation().numbers = false;
    layer.sync(frame); layer.submitLabels(frame, context);
    EXPECT_EQ(layer.glyphCount(), 0u);
    EXPECT_TRUE(layer.drawing());
}

TEST(FootprintSource, RegistryDefaultsToQ16AndSwitchesExclusively) {
    chart::ChartLayers layers;
    EXPECT_TRUE(layers.registry.precedes("footprint", "footprintCells"));
    EXPECT_TRUE(layers.registry.precedes("footprintCells", "VP"));
    auto frame = footprintChartFrame();
    auto& look = layers.footprintCells.presentation();
    look.enabled = true;
    ASSERT_TRUE(chart::FlowFeed::bin(layers.footprintCells.mailbox().back(), footprintSeries(), footprintFrame()));
    layers.footprintCells.mailbox().post();
    layers.footprintCells.sync(frame);
    EXPECT_TRUE(layers.footprint.selected(frame));
    EXPECT_FALSE(layers.footprintCells.drawing()) << "q16 is the default even when cells have data";
    EXPECT_FALSE(look.hollow(frame.context.raster));
    look.cellsSource = true;
    layers.footprintCells.sync(frame);
    EXPECT_FALSE(layers.footprint.selected(frame));
    EXPECT_TRUE(layers.footprintCells.drawing());
    EXPECT_FALSE(look.hollow(frame.context.raster)) << "partial history keeps normal candles";
    frame.context.overlays.footprint = false;
    layers.footprintCells.sync(frame);
    EXPECT_FALSE(layers.footprint.selected(frame));
    EXPECT_FALSE(layers.footprintCells.drawing());
}

TEST(FootprintMotion, OverscanRetainsVisibleRowsAcrossPanAndFollowLiveWithoutAllocation) {
    chart::FootprintCellsLayer layer;
    layer.presentation().windowIntensity = true;
    layer.presentation().enabled = true; layer.presentation().cellsSource = true;
    auto series = footprintSeries();
    for (int i : {-2, -1, 1, 2, 3}) {
        series.bars[i * 1000] = series.bars[0]; series.bars[i * 1000].bar.startMs = i * 1000;
    }
    ASSERT_TRUE(chart::FlowFeed::bin(layer.mailbox().back(), series, footprintFrame()));
    EXPECT_EQ(layer.mailbox().back().barCount, 6);
    EXPECT_EQ(layer.mailbox().back().fromMs, -2000); EXPECT_EQ(layer.mailbox().back().toMs, 4000);
    EXPECT_EQ(layer.mailbox().back().low, 85); EXPECT_EQ(layer.mailbox().back().high, 115);
    layer.mailbox().post();
    std::unique_ptr<QSGNode> root(layer.createContainer());
    ChartTextRenderer text; ChartTextAtlas atlas;
    chart::LayerNodeContext context{nullptr, root.get(), text, atlas, false};
    auto frame = footprintChartFrame();
    layer.sync(frame); layer.updateNodes(frame, context);
    ASSERT_TRUE(layer.presentation().hollow(frame.context.raster));
    // 1 px, quarter viewport, follow-live column, and beyond overscan containment.
    for (double dx : {1., 28., 56., 168.}) {
        frame = footprintChartFrame();
        auto& r = frame.context.raster;
        r.leftF += dx; r.drawnStartMs += dx / 56 * 1000; r.drawnEndMs += dx / 56 * 1000;
        r.topF += 1; r.drawnMinPrice += 1. / 12; r.drawnMaxPrice += 1. / 12;
        footprintAllocations = 0; footprintCounting = true;
        layer.sync(frame); layer.updateNodes(frame, context);
        footprintCounting = false;
        EXPECT_EQ(footprintAllocations.load(), 0u);
        EXPECT_TRUE(layer.drawing()); EXPECT_GT(layer.vertexCount(), 0);
        EXPECT_EQ(layer.presentation().hollow(r), dx < 168);
    }
}
TEST(FootprintCells, FlatBinningReusesStorageAndWindowMaximumExcludesOverscan) {
    auto out = std::make_unique<chart::FlowSnapshot>();
    auto series = footprintSeries();
    auto far = series.bars[0]; far.bar.startMs = 2000;
    for (auto& row : far.bar.rows) { row.bidVol *= 100; row.askVol *= 100; }
    series.bars[2000] = far;
    const auto frame = footprintFrame();
    chart::FlowFeed::bin(*out, series, frame);
    footprintAllocations = 0; footprintCounting = true;
    chart::FlowFeed::bin(*out, series, frame);
    footprintCounting = false;
    EXPECT_EQ(footprintAllocations.load(), 0u);
    EXPECT_DOUBLE_EQ(out->maxRowVolume, 6.8);
    EXPECT_EQ(out->barCount, 2);
}

TEST(CvdLayer, LazyContainerKeepsDefaultTreeAndInsertsBeforeText) {
    chart::ChartLayers layers;
    layers.heatmap.initialize();
    QSGNode root;
    layers.registry.onRootRebuilt(&root);
    ASSERT_EQ(root.childCount(), 7);
    auto* textRoot = root.lastChild();
    ChartTextRenderer text; ChartTextAtlas atlas;
    chart::LayerNodeContext context{nullptr, &root, text, atlas, false};
    auto frame = footprintChartFrame();
    layers.registry.sync(frame); layers.registry.updateNodes(frame, context);
    EXPECT_EQ(root.childCount(), 7); EXPECT_EQ(root.lastChild(), textRoot);
    layers.cvd.presentation().stripPx = 50;
    chart::CvdFeed::build(layers.cvd.mailbox().back(), footprintSeries(), footprintFrame(), chart::CvdReset::UtcDay, 4);
    layers.cvd.mailbox().post();
    layers.registry.sync(frame); layers.registry.updateNodes(frame, context);
    ASSERT_EQ(root.childCount(), 8); EXPECT_EQ(root.lastChild(), textRoot);
    EXPECT_EQ(root.childAtIndex(6)->type(), QSGNode::OpacityNodeType);
    EXPECT_TRUE(layers.cvd.drawing());
    layers.cvd.presentation().stripPx = 0;
    layers.registry.sync(frame); layers.registry.updateNodes(frame, context);
    EXPECT_FALSE(layers.cvd.drawing());
    EXPECT_EQ(static_cast<QSGOpacityNode*>(root.childAtIndex(6))->opacity(), 0);
}
TEST(CvdLayer, LineAreaBoundsAndFreshFramesAllocateNothing) {
    chart::CvdLayer layer;
    layer.presentation().stripPx = 50;
    auto frame = footprintChartFrame();
    frame.context.surfaceBounds = {0, 0, 112, 100}; // excludes the time axis and price gutter
    frame.context.itemBounds = {0, 0, 160, 120}; frame.context.rightGutterPx = 48;
    frame.context.overlays.footprint = false;
    auto series = footprintSeries();
    series.bars[1000] = series.bars[0]; series.bars[1000].bar.startMs = 1000;
    series.bars[1000].bar.delta = -600;
    chart::CvdFeed::build(layer.mailbox().back(), series, footprintFrame(), chart::CvdReset::UtcDay, 4);
    layer.mailbox().post(); layer.sync(frame);
    std::unique_ptr<QSGNode> root(layer.createContainer());
    ChartTextRenderer text; ChartTextAtlas atlas;
    chart::LayerNodeContext context{nullptr, root.get(), text, atlas, false};
    layer.updateNodes(frame, context);
    ASSERT_TRUE(layer.drawing()); ASSERT_GT(layer.vertexCount(), 12);
    EXPECT_EQ(layer.strip(), QRectF(0, 50, 112, 50));
    auto* geometry = static_cast<QSGGeometryNode*>(root->firstChild())->geometry();
    auto* vertices = geometry->vertexDataAsColoredPoint2D();
    const auto lineVertex = vertices[13];
    layer.presentation().area = true;
    for (int i = 0; i < 2; ++i) {
        chart::CvdFeed::build(layer.mailbox().back(), series, footprintFrame(), chart::CvdReset::UtcDay, 4);
        layer.mailbox().post();
        footprintAllocations = 0; footprintCounting = true;
        layer.sync(frame); layer.updateNodes(frame, context); layer.submitLabels(frame, context);
        footprintCounting = false;
        EXPECT_EQ(footprintAllocations.load(), 0u);
    }
    EXPECT_NE(vertices[13].y, lineVertex.y); EXPECT_EQ(vertices[13].a, 110);
    bool green = false, red = false;
    for (int i = 0; i < layer.vertexCount(); ++i) {
        EXPECT_GE(vertices[i].x, 0); EXPECT_LE(vertices[i].x, 112);
        EXPECT_GE(vertices[i].y, 50); EXPECT_LE(vertices[i].y, 100);
        green |= vertices[i].g > vertices[i].r;
        red |= vertices[i].r > vertices[i].g;
    }
    EXPECT_TRUE(green); EXPECT_TRUE(red);
    layer.presentation().stripPx = 1; layer.presentation().area = false;
    layer.sync(frame); layer.updateNodes(frame, context);
    for (int i = 0; i < layer.vertexCount(); ++i) {
        EXPECT_GE(vertices[i].y, 99); EXPECT_LE(vertices[i].y, 100);
    }
}
TEST(CvdFeed, SumsAllLoadedBarsAndRespectsTpoOpenAndBaseScale) {
    auto source = footprintSeries();
    auto frame = footprintFrame();
    frame.raster.drawnStartMs = 1000;
    source.bars[1000] = source.bars[0]; source.bars[1000].bar.startMs = 1000;
    source.bars[1000].bar.baseScale = 50;
    chart::CvdSnapshot snapshot;
    chart::CvdFeed::build(snapshot, source, frame, chart::CvdReset::None, 4);
    ASSERT_EQ(snapshot.series.count, 1); EXPECT_EQ(snapshot.series.points[0].value, 9);
    const int64_t open = 13 * 3600000;
    source.bars.clear();
    for (auto time : {open - 1000, open, open + 1000}) {
        auto& bar = source.bars[time].bar;
        bar.startMs = time; bar.timeframeMs = 1000; bar.baseScale = 1; bar.delta = 5;
    }
    frame.raster.drawnStartMs = open; frame.raster.drawnEndMs = open + 2000;
    chart::CvdFeed::build(snapshot, source, frame, chart::CvdReset::TpoSession, 0);
    ASSERT_EQ(snapshot.series.count, 2); EXPECT_EQ(snapshot.series.points[0].value, 5);
    EXPECT_EQ(snapshot.series.points[1].value, 10);
}

TEST(CvdLayer, LabelsAreBoundedAndFreshDataFramesAllocateNothing) {
    QTemporaryDir cache;
    QString error;
    ASSERT_TRUE(AgentHostMode::activate(cache.path(), {}, &error)) << error.toStdString();
    struct Reset { ~Reset() { AgentHostMode::resetForTests(); } } reset;
    ChartTextAtlas atlas;
    ChartTextAtlas::BuildParams params;
    params.fontPath = QStringLiteral(SENTINEL_SOURCE_DIR "/resources/fonts/RobotoMono/RobotoMono-Regular.ttf");
    params.fontPx = 64; params.charset = " 0123456789.e+-CVDsinceTPOUdaylotr";
    ASSERT_TRUE(atlas.build(params));
    chart::CvdLayer layer;
    layer.presentation().stripPx = 80;
    auto frame = footprintChartFrame(); frame.context.surfaceBounds.setWidth(400);
    chart::CvdFeed::build(layer.mailbox().back(), footprintSeries(), footprintFrame(), chart::CvdReset::UtcDay, 4);
    layer.mailbox().post(); layer.sync(frame);
    std::unique_ptr<QSGNode> root(layer.createContainer());
    ChartTextRenderer text;
    chart::LayerNodeContext context{nullptr, root.get(), text, atlas, true};
    layer.updateNodes(frame, context); layer.submitLabels(frame, context);
    ASSERT_GT(text.submittedGlyphs(), 0);
    for (int i = 0; i < 2; ++i) {
        auto series = footprintSeries(); series.bars[0].bar.delta += i + 1;
        chart::CvdFeed::build(layer.mailbox().back(), series, footprintFrame(), chart::CvdReset::UtcDay, 4);
        layer.mailbox().post();
        footprintAllocations = 0; footprintCounting = true;
        layer.sync(frame); layer.updateNodes(frame, context); layer.submitLabels(frame, context);
        footprintCounting = false;
        EXPECT_EQ(footprintAllocations.load(), 0u);
    }
}

TEST(CvdFeed, SubsecondChartUsesOneSecondFlow) {
    auto frame = footprintFrame(); frame.activeTimeframeMs = 100;
    chart::CvdSnapshot snapshot;
    chart::CvdFeed::build(snapshot, footprintSeries(), frame, chart::CvdReset::UtcDay, 4);
    EXPECT_EQ(snapshot.timeframeMs, 1000); ASSERT_EQ(snapshot.series.count, 1);
    EXPECT_EQ(snapshot.series.points[0].value, 3);
}
