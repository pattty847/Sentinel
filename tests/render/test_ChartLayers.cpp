#include "chart/layers/ChartLayers.hpp"
#include "themes/ThemePalette.hpp"
#include "render/heatmap/HeatmapGpuLayer.hpp"
#include "chart/feeds/TradeOverlaySequence.hpp"
#include <gtest/gtest.h>
#include "render/FrameContextBuilder.hpp"
#include <QElapsedTimer>
#include <memory>
#include <string>
#include <vector>
#include <set>
#include "heatmap/HeatmapResolution.hpp"
#include <chrono>
#include <cstdio>
#include "render/ChartRaster.hpp"

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
struct RegistryScene {
    std::unique_ptr<QSGNode> fields, profiles, foreground;
    explicit RegistryScene(chart::LayerRegistry& registry)
        : fields(registry.createRoot(nullptr)),
          profiles(registry.createOutputRoot(chart::LayerPlane::Profiles)),
          foreground(registry.createOutputRoot(chart::LayerPlane::Foreground)) {}
};
TEST(ChartLayerSet, RegistersHeatmapFootprintCellsVpTpoLadderText) {
    chart::ChartLayers layers;
    ASSERT_TRUE(layers.registry.precedes("heatmap", "text"));
    EXPECT_TRUE(layers.registry.precedes("TPO", "ladder"));
    EXPECT_TRUE(layers.registry.precedes("ladder", "text"));
    EXPECT_FALSE(layers.registry.precedes("ladder", "TPO"));
    layers.heatmap.initialize();
    RegistryScene scene(layers.registry);
    EXPECT_EQ(scene.fields->childCount(), 2);
    EXPECT_EQ(scene.profiles->firstChild()->childCount(), 2);
    EXPECT_EQ(scene.foreground->firstChild()->childCount(), 2);
    EXPECT_EQ(scene.foreground->firstChild()->firstChild()->type(), QSGNode::OpacityNodeType);
    EXPECT_EQ(scene.fields->firstChild()->type(), QSGNode::OpacityNodeType);
    EXPECT_EQ(scene.fields->firstChild()->firstChild()->type(), QSGNode::RenderNodeType);
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
};
TEST(TradeOverlayFeeds, ClearQueuesProcessorBeforeMailbox) {
    Calls calls;
    auto processor = std::make_unique<Processor>(calls);
    Mailbox vp{calls, "VP"};
    chart::clearTradeOverlayFeeds(processor, vp, [&](Processor*) { calls.push_back("queue clear"); });
    EXPECT_EQ(calls, (Calls{"queue clear", "clear VP"}));
    processor.reset(); calls.clear();
    chart::clearTradeOverlayFeeds(processor, vp, [&](Processor*) { ADD_FAILURE(); });
    EXPECT_EQ(calls, (Calls{"clear VP"}));
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
#include "chart/core/FlowInk.hpp"
#include "chart/shell/LayerVisibility.hpp"
#include "chart/shell/ChartLookSettings.hpp"
#include "mainwindow/ChartAgentPort.hpp"
#include "render/heatmap/HeatmapSettingsStore.hpp"
#include "config/AgentHostMode.hpp"
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

TEST(LayerRegistry, OutputRootsCanSyncAndDieInEitherOrderWithoutSteadyAllocations) {
    for (bool surfaceFirst : {false, true}) {
        for (bool ownerDiesFirst : {false, true}) {
            SCOPED_TRACE(surfaceFirst);
            SCOPED_TRACE(ownerDiesFirst);
            Calls calls;
            Layer fields("fields", calls), profiles("profiles", calls), text("text", calls);
            chart::LayerRegistry registry;
            registry.add(fields);
            registry.add(profiles, FrameProfiler::StageCount, chart::LayerPlane::Profiles);
            registry.add(text, FrameProfiler::StageCount, chart::LayerPlane::Foreground);
            std::unique_ptr<QSGNode> owner, surface;
            if (surfaceFirst) surface.reset(registry.createOutputRoot(chart::LayerPlane::Profiles));
            owner.reset(registry.createRoot(nullptr));
            if (!surfaceFirst) surface.reset(registry.createOutputRoot(chart::LayerPlane::Profiles));
            ASSERT_EQ(surface->childCount(), 1);
            auto* content = surface->firstChild();
            ASSERT_EQ(content->childCount(), 1);
            EXPECT_NE(content->parent(), owner.get());
            auto* ownerPtr = owner.get(); auto* surfacePtr = surface.get();
            footprintAllocations = 0; footprintCounting = true;
            for (int frame = 0; frame < 10; ++frame) {
                registry.createRoot(ownerPtr);
                registry.createOutputRoot(chart::LayerPlane::Profiles, surfacePtr);
            }
            footprintCounting = false;
            EXPECT_EQ(footprintAllocations.load(), 0u);
            EXPECT_EQ(surface->firstChild(), content);
            if (ownerDiesFirst) {
                owner.reset();
                EXPECT_EQ(surface->childCount(), 0);
                owner.reset(registry.createRoot(nullptr));
                ASSERT_EQ(surface->childCount(), 1);
                EXPECT_EQ(surface->firstChild()->childCount(), 1);
            } else {
                surface.reset();
                EXPECT_EQ(content->parent(), owner.get());
                surface.reset(registry.createOutputRoot(chart::LayerPlane::Profiles));
                EXPECT_EQ(surface->firstChild(), content);
            }
        }
    }
}

namespace {
MappingFrameContext footprintFrame() {
    MappingFrameContext f;
    f.activeTimeframeMs = 1000;
    f.surfaceBounds = {0, 0, 200, 180};
    f.surfaceDpr = 1;
    f.viewportValid = true;
    auto& r = f.raster;
    r.valid = true; r.dpr = 1; r.tick = 1; r.tfMs = 1000;
    r.rowPx = 18; r.rowPxF = 18; r.colPx = 100; r.colPxF = 100;
    r.drawnStartMs = 0; r.drawnEndMs = 2000; r.drawnMinPrice = 95; r.drawnMaxPrice = 105;
    r.topF = 1890; r.topRowIndex = 1890; r.heightDev = 180; r.widthDev = 200;
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
    EXPECT_EQ(out->bars[0].maxSideVol, 440);
    chart::FootprintCellsLayer layer;
    layer.presentation().enabled = true;
    layer.presentation().dustNotional = 0;
    layer.mailbox().back() = *out; layer.mailbox().post();
    std::unique_ptr<QSGNode> root(layer.createContainer());
    ChartTextRenderer text; ChartTextAtlas atlas;
    chart::LayerNodeContext context{nullptr, root.get(), text, atlas, false};
    auto frame = footprintChartFrame();
    layer.sync(frame); layer.updateNodes(frame, context);
    auto* geometry = static_cast<QSGGeometryNode*>(root->firstChild())->geometry();
    auto* v = geometry->vertexDataAsColoredPoint2D();
    EXPECT_EQ(v[0].a, 255) << "opaque card covers heatmap";
    EXPECT_FLOAT_EQ(v[0].x, 0); EXPECT_FLOAT_EQ(v[2].x, 100);
    const auto share = [](const QSGGeometry::ColoredPoint2D& vertex) {
        return (vertex.a / 255. - chart::kFlowMinFillAlpha) / (1 - chart::kFlowMinFillAlpha);
    };
    EXPECT_FLOAT_EQ(v[14].x - v[12].x, v[20].x - v[18].x) << "a side fills its whole half whatever its volume";
    EXPECT_NEAR(share(v[12]) / share(v[18]), 240.0 / 440, .01) << "opacity tracks volume";
    EXPECT_FLOAT_EQ(v[14].x, 54.5); EXPECT_FLOAT_EQ(v[18].x, 55.5);
    EXPECT_DOUBLE_EQ(layer.cards().size(), 2);
    const double bidShare = share(v[12]), askShare = share(v[18]);
    layer.presentation().windowIntensity = true;
    layer.sync(frame); layer.updateNodes(frame, context);
    v = geometry->vertexDataAsColoredPoint2D();
    EXPECT_NEAR(share(v[12]), bidShare / 2, .01);
    EXPECT_NEAR(share(v[18]), askShare / 2, .01);
    EXPECT_EQ(v[0].a, 255);
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
    auto frame = footprintFrame(); frame.raster.drawnEndMs = 9000000; frame.raster.widthDev = 900000;
    for (int i = -1000; i < 10000; ++i) {
        series.bars[i * 1000] = series.bars[0]; series.bars[i * 1000].bar.startMs = i * 1000;
    }
    ASSERT_TRUE(chart::FlowFeed::bin(*out, series, frame));
    EXPECT_TRUE(out->valid); EXPECT_EQ(out->barCount, chart::FlowSnapshot::kMaxBars);
    EXPECT_GT(out->truncatedBars, 0);
    EXPECT_EQ(out->bars[0].startMs, 9000000 - chart::FlowSnapshot::kMaxBars * 1000) << "the far past is trimmed";
    EXPECT_EQ(out->bars[size_t(out->barCount - 1)].startMs, 8999000) << "never the newest edge";
    // A view that fits the cap shrinks its overscan instead of truncating.
    frame.raster.drawnEndMs = 7000000; frame.raster.widthDev = 700000;
    ASSERT_TRUE(chart::FlowFeed::bin(*out, series, frame));
    EXPECT_EQ(out->truncatedBars, 0);
    EXPECT_EQ(out->fromMs, -596000); EXPECT_EQ(out->toMs, 7596000);
    EXPECT_EQ(out->barCount, chart::FlowSnapshot::kMaxBars - 2);
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
FlowSeriesBuffer::Series ladderSeries(int rows, int64_t nativeTick = 1) {
    FlowSeriesBuffer::Series series;
    auto& b = series.bars[0].bar;
    b.startMs = 0; b.timeframeMs = 1000; b.nativeTick = double(nativeTick); b.baseScale = 1;
    b.lowRow = 100 / nativeTick;
    for (int i = 0; i < rows; ++i) b.rows.push_back({int16_t(i), 10 + i, 100 + i});
    return series;
}
MappingFrameContext stripFrame(double rowPx) {
    auto frame = footprintFrame();
    frame.raster.colPxF = 10; frame.raster.rowPxF = rowPx;
    return frame;
}
TEST(FootprintBands, MergedBandsSumBidAndAskAcrossRowBoundaries) {
    auto out = std::make_unique<chart::FlowSnapshot>();
    ASSERT_EQ(chart::footprintLod(10, 2), chart::FootprintStage::Strip);
    ASSERT_TRUE(chart::FlowFeed::bin(*out, ladderSeries(6), stripFrame(2))); // k = 3
    ASSERT_EQ(out->cellCount, 3);
    const int64_t starts[] = {99, 102, 105}, bids[] = {10 + 11, 12 + 13 + 14, 15}, asks[] = {100 + 101, 102 + 103 + 104, 105};
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(out->cells[size_t(i)].row, starts[i]); EXPECT_EQ(out->cells[size_t(i)].span, 3);
        EXPECT_EQ(out->cells[size_t(i)].bidVol, bids[i]); EXPECT_EQ(out->cells[size_t(i)].askVol, asks[i]);
    }
    EXPECT_EQ(out->bars[0].bidVol, 0) << "bar totals come from the bar, not the bands";
    EXPECT_EQ(out->bars[0].maxRowVol, 12 + 13 + 14 + 102 + 103 + 104);
    EXPECT_EQ(out->bars[0].lowRow, 99); EXPECT_EQ(out->bars[0].highRow, 108);
    // Coarse native bins stay whole: span 2 with k = 3 bands by 4.
    ASSERT_TRUE(chart::FlowFeed::bin(*out, ladderSeries(3, 2), stripFrame(2)));
    ASSERT_EQ(out->cellCount, 2);
    EXPECT_EQ(out->cells[0].row, 100); EXPECT_EQ(out->cells[0].span, 4);
    EXPECT_EQ(out->cells[0].bidVol, 21); EXPECT_EQ(out->cells[1].row, 104); EXPECT_EQ(out->cells[1].bidVol, 12);
    // Rows that already reach the band height are not merged.
    ASSERT_TRUE(chart::FlowFeed::bin(*out, ladderSeries(6), stripFrame(6)));
    EXPECT_EQ(out->cellCount, 6); EXPECT_EQ(out->cells[0].span, 1);
}
TEST(FootprintBands, PanKeepsTheSameBandsForTheSamePrices) {
    auto first = std::make_unique<chart::FlowSnapshot>(), panned = std::make_unique<chart::FlowSnapshot>();
    const auto series = ladderSeries(9);
    ASSERT_TRUE(chart::FlowFeed::bin(*first, series, stripFrame(2)));
    for (double shift : {1., 2., 2.5, 7.}) {
        auto frame = stripFrame(2);
        auto& r = frame.raster;
        r.topF += shift * 2; r.drawnMinPrice += shift; r.drawnMaxPrice += shift;
        r.leftF += shift; r.drawnStartMs += shift * 100; r.drawnEndMs += shift * 100;
        ASSERT_TRUE(chart::FlowFeed::bin(*panned, series, frame));
        ASSERT_EQ(panned->cellCount, first->cellCount) << shift;
        for (int i = 0; i < first->cellCount; ++i) {
            EXPECT_EQ(panned->cells[size_t(i)].row, first->cells[size_t(i)].row) << shift;
            EXPECT_EQ(panned->cells[size_t(i)].bidVol, first->cells[size_t(i)].bidVol) << shift;
        }
    }
}
TEST(FootprintBands, WidestStripViewFitsEveryVisibleBarAndTheCellBudget) {
    // 2 px columns across a 4096 px surface with sub-pixel rows: every visible bar keeps its bands.
    auto out = std::make_unique<chart::FlowSnapshot>();
    auto series = ladderSeries(200);
    for (int i = 0; i < 2048; ++i) { series.bars[i * 1000] = series.bars[0]; series.bars[i * 1000].bar.startMs = i * 1000; }
    auto frame = stripFrame(.25);
    auto& r = frame.raster;
    r.colPxF = 2; r.drawnStartMs = 0; r.drawnEndMs = 2048000; r.drawnMinPrice = 0; r.drawnMaxPrice = 400;
    r.widthDev = 4096; r.heightDev = 100;
    ASSERT_TRUE(chart::FlowFeed::bin(*out, series, frame));
    EXPECT_EQ(out->barCount, 2048); EXPECT_EQ(out->truncatedBars, 0);
    EXPECT_LE(out->cellCount, chart::FlowSnapshot::kMaxCells);
    EXPECT_EQ(out->bars[0].cellCount, 9) << "200 rows in 24-row bands from row 96";
}
// 4096 px of 2 px columns, 800 native rows per bar under 0.25 px rows, prices 0..1000.
FlowSeriesBuffer::Series denseSeries(int first, int last) {
    auto series = ladderSeries(800);
    for (int i = first; i < last; ++i) { series.bars[i * 1000] = series.bars[0]; series.bars[i * 1000].bar.startMs = i * 1000; }
    return series;
}
MappingFrameContext denseFrame() {
    auto frame = stripFrame(.25);
    auto& r = frame.raster;
    r.colPxF = 2; r.drawnStartMs = 0; r.drawnEndMs = 2048000; r.drawnMinPrice = 0; r.drawnMaxPrice = 1000;
    r.widthDev = 4096; r.heightDev = 250;
    return frame;
}
int visibleBars(const chart::FlowSnapshot& out, const MappingFrameContext& frame) {
    int count = 0;
    for (int i = 0; i < out.barCount; ++i)
        count += double(out.bars[size_t(i)].startMs) + 1000 > frame.raster.drawnStartMs &&
                 double(out.bars[size_t(i)].startMs) < frame.raster.drawnEndMs;
    return count;
}
int visibleBars60(const chart::FlowSnapshot& out, const MappingFrameContext& frame) {
    int count = 0;
    for (int i = 0; i < out.barCount; ++i)
        count += double(out.bars[size_t(i)].startMs) + 60000 > frame.raster.drawnStartMs &&
                 double(out.bars[size_t(i)].startMs) < frame.raster.drawnEndMs;
    return count;
}
TEST(FootprintBands, DenseViewCoarsensBandsInsteadOfDroppingVisibleBars) {
    auto out = std::make_unique<chart::FlowSnapshot>();
    const auto frame = denseFrame();
    ASSERT_TRUE(chart::FlowFeed::bin(*out, denseSeries(0, 2048), frame));
    EXPECT_EQ(out->bandRows, 34) << "24 rows reach 6 px; 34 fit 2049 visible bars x 1001 rows";
    EXPECT_EQ(out->barCount, 2048); EXPECT_EQ(out->truncatedBars, 0);
    EXPECT_EQ(out->bars[2047].startMs, 2047000);
    EXPECT_LE(out->cellCount, chart::FlowSnapshot::kMaxCells);
}
TEST(FootprintBands, PanNeverDropsAVisibleBarOrFlipsTheBandFactor) {
    auto out = std::make_unique<chart::FlowSnapshot>();
    const auto series = denseSeries(-200, 2300);
    for (double shift : {0., 300., 500., 1700., 2500.5, -999.}) {
        auto frame = denseFrame();
        auto& r = frame.raster;
        r.drawnStartMs += shift; r.drawnEndMs += shift; r.leftF += shift / 500;
        r.drawnMinPrice += shift / 1000; r.drawnMaxPrice += shift / 1000;
        ASSERT_TRUE(chart::FlowFeed::bin(*out, series, frame, chart::FootprintStage::Strip, 34));
        EXPECT_EQ(out->bandRows, 34) << shift;
        const int expected = int(std::ceil(r.drawnEndMs / 1000) - std::floor(r.drawnStartMs / 1000));
        EXPECT_EQ(visibleBars(*out, frame), expected) << shift;
        EXPECT_LE(out->cellCount, chart::FlowSnapshot::kMaxCells);
    }
}
TEST(FootprintBands, BudgetBandFactorIgnoresPanRoundingOnADecimalTick) {
    // Real free-camera arithmetic (shiftedRaster): drawn bounds carry ~1e-10 rounding on a .01 tick.
    auto frame = footprintFrame();
    auto base = frame.raster;
    base.free = true; base.widthDev = 4096; base.heightDev = 299; base.colPx = 2; base.rowPx = 1;
    base.colPxF = 2; base.rowPxF = 1; base.dpr = 1; base.tick = .01; base.tfMs = 1000;
    base.topF = 6553897; base.leftF = 2000000;
    const FlowSeriesBuffer::Series empty;
    auto out = std::make_unique<chart::FlowSnapshot>();
    std::set<int64_t> factors;
    for (int step = 1; step <= 64; ++step) {
        for (const auto& raster : {chart_raster::shiftedRaster(base, 0, step * .25),   // price pan across 6553898/9
                                   chart_raster::shiftedRaster(base, step * .37, 0)}) { // fractional time pan
            frame.raster = raster;
            ASSERT_TRUE(chart::FlowFeed::bin(*out, empty, frame, chart::FootprintStage::Strip, 0)) << step;
            factors.insert(out->bandRows);
        }
    }
    EXPECT_EQ(factors, std::set<int64_t>{10}) << "2049 bars x 300 rows: k = 10 on every pan";
}
// 1m history one bar wider than the widest supported view, 200 rows per bar spread
// over 2880 rows, one unit of delta per bar.
const FlowSeriesBuffer::Series& widestSeries() {
    static const FlowSeriesBuffer::Series series = [] {
        FlowSeriesBuffer::Series out;
        sentinel::flow::FlowBar bar;
        bar.timeframeMs = 60000; bar.nativeTick = 1; bar.baseScale = 1; bar.lowRow = 100; bar.delta = 1;
        for (int i = 0; i < 200; ++i) bar.rows.push_back({int16_t(i * 14), 10 + i, 100 + i});
        for (int i = -100; i < heatmap::kMaxChartColumns + 100; ++i) {
            auto& slot = out.bars[int64_t(i) * 60000].bar; slot = bar; slot.startMs = int64_t(i) * 60000;
        }
        return out;
    }();
    return series;
}
// Full zoom-out (or a fraction of it) through the real camera inputs: the shared limit
// sets the span; prices 0..2880 over 2880 logical px.
chart_raster::RasterInputs zoomedOut(double shiftBars, double widthLogical, double dpr, double spanScale = 1) {
    chart_raster::RasterInputs in;
    const double span = heatmap::maxTimeSpanMs(widthLogical, 60000) * spanScale;
    in.timeStart = std::llround(shiftBars * 60000); in.timeEnd = in.timeStart + int64_t(span);
    in.minPrice = shiftBars * .37; in.maxPrice = in.minPrice + 2880; in.tick = 1; in.tfMs = 60000;
    in.itemWidthLogical = widthLogical; in.itemHeightLogical = 2880; in.dpr = dpr;
    return in;
}
constexpr double kWidestLogical = heatmap::kMaxChartColumns * heatmap::kMinChartCellLogicalPx;
MappingFrameContext frameFor(const chart_raster::RasterCamera& camera) {
    auto frame = footprintFrame();
    frame.activeTimeframeMs = 60000;
    frame.raster = camera;
    return frame;
}
// The free camera a pinch or glide draws: a fractional left edge.
MappingFrameContext widestFrame(double shiftBars) {
    return frameFor(chart_raster::continuousRaster(zoomedOut(shiftBars, kWidestLogical, 1)));
}
// The same bars through the real retention path (history page, then eviction).
FlowSeriesBuffer retainedWidest() {
    FlowSeriesBuffer buffer;
    FlowSeriesBuffer::Batch batch;
    batch.symbol = "BTC-USD"; batch.timeframeMs = 60000; batch.history = true;
    for (const auto& [start, record] : widestSeries().bars) {
        auto& next = batch.bars.emplace_back(record); next.complete = true;
        next.source = SentinelStreamClient::FlowSource::Store;
    }
    buffer.apply(batch, 0);
    return buffer;
}
// Bars of the full history (before retention) that the drawn window touches.
int historyBarsIn(const chart_raster::RasterCamera& r) {
    int count = 0;
    for (const auto& [start, record] : widestSeries().bars)
        count += double(start) + 60000 > r.drawnStartMs && double(start) < r.drawnEndMs;
    return count;
}
double drawnColumns(const chart_raster::RasterCamera& camera) {
    return double(camera.widthDev) / (camera.free ? camera.colPxF : double(camera.colPx));
}
TEST(ChartZoomLimit, FullZoomOutDrawsAtMostHalfTheLogicalWidthAtRestAndMidGlide) {
    EXPECT_EQ(heatmap::kMinChartCellLogicalPx, 2);
    for (int widthDev : {1920, 2560, 5120, 10000})
        for (double dpr : {1., 2.}) {
            const double logical = widthDev / dpr, limit = std::floor(logical / 2) + 2;
            const auto in = zoomedOut(100.5, logical, dpr);
            const auto rest = chart_raster::computeRaster(in, {});
            const auto free = chart_raster::continuousRaster(in);
            const auto near = chart_raster::computeRaster(zoomedOut(100.5, logical, dpr, .02), {});
            const auto glide = chart_raster::glideRaster(near, free, double(in.timeStart) + (in.timeEnd - in.timeStart) / 2.0, 1440, .5);
            for (const auto& camera : {rest, free, glide}) {
                ASSERT_TRUE(camera.valid) << widthDev << ' ' << dpr;
                EXPECT_LE(drawnColumns(camera), limit) << widthDev << " dpr " << dpr << (camera.free ? " free" : " rest");
            }
        }
    // A resize or dpr glide: the from camera moves onto the to surface first.
    struct Surface { double logical, dpr; };
    for (auto [from, to] : {std::pair{Surface{2560, 1}, Surface{1920, 1}}, std::pair{Surface{1920, 1}, Surface{2560, 1}},
                           std::pair{Surface{2560, 1}, Surface{2560, 2}}, std::pair{Surface{5000, 2}, Surface{1280, 1}}}) {
        const auto a = chart_raster::continuousRaster(zoomedOut(100.5, from.logical, from.dpr));
        const auto b = chart_raster::continuousRaster(zoomedOut(100.5, to.logical, to.dpr));
        for (double e : {.25, .5, .75}) {
            const auto mid = chart_raster::glideRaster(a, b, double(zoomedOut(100.5, to.logical, to.dpr).timeStart), 1440, e);
            ASSERT_TRUE(mid.valid);
            EXPECT_LE(drawnColumns(mid), std::floor(to.logical / 2) + 2) << from.logical << " -> " << to.logical << " e " << e;
        }
    }
}
TEST(ChartZoomLimit, ManualTickRowsStopAtTwoLogicalPixels) {
    EXPECT_DOUBLE_EQ(heatmap::maxManualPriceSpan(880, 1), 440);
    EXPECT_DOUBLE_EQ(heatmap::maxManualPriceSpan(880.5, 25), 440 * 25);
    EXPECT_DOUBLE_EQ(heatmap::maxTimeSpanMs(1500.5, 60000), 750.0 * 60000);
}
TEST(ChartZoomLimit, EveryVisibleBarIsRetainedAndBinnedAtFullZoomOut) {
    const auto buffer = retainedWidest();
    const auto* series = buffer.series(60000);
    ASSERT_EQ(series->bars.size(), FlowSeriesBuffer::kMaxBuckets);
    const int64_t newest = series->bars.rbegin()->first / 60000;
    auto out = std::make_unique<chart::FlowSnapshot>();
    struct Surface { double logical, dpr; };
    for (auto surface : {Surface{1920, 1}, Surface{1280, 2}, Surface{2560, 1}, Surface{2560, 2}, Surface{5000, 2},
                        Surface{10000, 1}, Surface{kWidestLogical, 1}}) {
        const double columns = heatmap::maxTimeSpanMs(surface.logical, 60000) / 60000;
        const auto in = zoomedOut(double(newest + 1) - columns - .5, surface.logical, surface.dpr);
        for (const auto& camera : {chart_raster::computeRaster(in, {}), chart_raster::continuousRaster(in)}) {
            ASSERT_TRUE(camera.valid);
            const auto frame = frameFor(camera);
            ASSERT_TRUE(chart::FlowFeed::bin(*out, *series, frame, chart::FootprintStage::Strip, 0));
            const int expected = historyBarsIn(camera);
            EXPECT_LE(expected, heatmap::kMaxChartSurfaceBars) << surface.logical;
            EXPECT_EQ(visibleBars60(*out, frame), expected) << surface.logical << (camera.free ? " free" : " rest");
            EXPECT_EQ(out->truncatedBars, 0) << surface.logical;
            EXPECT_LE(out->cellCount, chart::FlowSnapshot::kMaxCells) << surface.logical;
        }
    }
}
TEST(FootprintBands, WidestSupportedViewKeepsEveryVisibleBarWithinTheCellBudget) {
    auto out = std::make_unique<chart::FlowSnapshot>();
    const auto frame = widestFrame(0);
    ASSERT_DOUBLE_EQ(frame.raster.colPxF, 2);
    double best = 1e9;
    for (int run = 0; run < 3; ++run) {
        const auto start = std::chrono::steady_clock::now();
        ASSERT_TRUE(chart::FlowFeed::bin(*out, widestSeries(), frame, chart::FootprintStage::Strip, 0));
        best = std::min(best, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    }
    std::printf("footprint bin at the widest supported 1m view: %d bars, %d cells, k=%lld, %.2f ms\n",
                out->barCount, out->cellCount, (long long)out->bandRows, best);
    RecordProperty("bin_ms", std::to_string(best));
    EXPECT_EQ(visibleBars60(*out, frame), heatmap::kMaxChartColumns);
    EXPECT_EQ(out->truncatedBars, 0);
    EXPECT_EQ(out->bandRows, 481) << "8193 bars x 2881 rows fit 65536 cells at 481 rows per band";
    EXPECT_LE(out->cellCount, chart::FlowSnapshot::kMaxCells);
}
TEST(FootprintBands, PanAtTheNarrowestColumnsKeepsEveryVisibleBarAndTheBandFactor) {
    auto out = std::make_unique<chart::FlowSnapshot>();
    for (double shift : {0., .3, .5, 1.7, 2.5, -.999, 37.25}) {
        const auto frame = widestFrame(shift);
        const auto& r = frame.raster;
        ASSERT_TRUE(chart::FlowFeed::bin(*out, widestSeries(), frame, chart::FootprintStage::Strip, 481));
        EXPECT_EQ(out->bandRows, 481) << shift;
        EXPECT_EQ(visibleBars60(*out, frame), historyBarsIn(r)) << shift;
        EXPECT_LE(out->cellCount, chart::FlowSnapshot::kMaxCells);
    }
}
TEST(CvdFeed, DayAndSessionKeepTheNewestPointAtFullZoomOutFromRetainedHistory) {
    const auto buffer = retainedWidest();
    const auto* series = buffer.series(60000);
    const int64_t newest = series->bars.rbegin()->first;
    const auto frame = widestFrame(double(newest / 60000 + 1 - heatmap::kMaxChartColumns) - .5);
    for (auto reset : {chart::CvdReset::UtcDay, chart::CvdReset::TpoSession}) {
        auto snapshot = std::make_unique<chart::CvdSnapshot>();
        chart::CvdFeed::build(*snapshot, *series, frame, reset, 4);
        const auto& points = snapshot->series;
        ASSERT_EQ(points.count, heatmap::kMaxChartColumns + 1) << "8193 visible bars";
        EXPECT_EQ(points.overflow, 0);
        EXPECT_EQ(points.points[size_t(points.count - 1)].timeMs, newest);
        EXPECT_TRUE(snapshot->totals[size_t(points.count - 1)].valid);
        EXPECT_EQ(snapshot->totals[size_t(points.count - 1)].delta, 1);
        if (reset == chart::CvdReset::UtcDay)
            EXPECT_EQ(points.points[size_t(points.count - 1)].value, double(newest % 86400000 / 60000 + 1))
                << "one per bar since the UTC day began";
    }
}
TEST(FootprintBands, BarCapIsTheWidestSupportedView) {
    for (int64_t tf : {int64_t(1000), int64_t(60000), int64_t(300000), int64_t(3600000), int64_t(86400000)})
        EXPECT_EQ(int64_t(heatmap::maxTimeSpanMs(kWidestLogical, tf)) / tf + 2, chart::FlowSnapshot::kMaxBars) << tf;
    EXPECT_EQ(int(FlowSeriesBuffer::kMaxBuckets), chart::FlowSnapshot::kMaxBars);
    EXPECT_EQ(chart::CvdSeries::kCapacity, chart::FlowSnapshot::kMaxBars);
}
TEST(FootprintBands, ThinColumnsTileWithoutOverlap) {
    chart::FootprintCellsLayer layer;
    layer.presentation().enabled = true;
    layer.presentation().dustNotional = 0;
    auto series = ladderSeries(1);
    series.bars[1000] = series.bars[0]; series.bars[1000].bar.startMs = 1000;
    auto binFrame = stripFrame(6); binFrame.raster.colPxF = 1;
    ASSERT_TRUE(chart::FlowFeed::bin(layer.mailbox().back(), series, binFrame));
    layer.mailbox().post();
    std::unique_ptr<QSGNode> root(layer.createContainer());
    ChartTextRenderer text; ChartTextAtlas atlas;
    chart::LayerNodeContext context{nullptr, root.get(), text, atlas, false};
    auto frame = footprintChartFrame();
    frame.context.raster.colPxF = 1; frame.context.raster.rowPxF = 6; frame.context.raster.topF = 110 * 6;
    layer.sync(frame); layer.updateNodes(frame, context);
    ASSERT_EQ(layer.stage(), chart::FootprintStage::Strip);
    ASSERT_EQ(layer.vertexCount(), 12);
    auto* v = static_cast<QSGGeometryNode*>(root->firstChild())->geometry()->vertexDataAsColoredPoint2D();
    EXPECT_FLOAT_EQ(v[0].x, 0); EXPECT_FLOAT_EQ(v[2].x, 1) << "a strip fills exactly its column, like a candle body";
    EXPECT_FLOAT_EQ(v[6].x, 1); EXPECT_FLOAT_EQ(v[8].x, 2);
}
TEST(FootprintBands, StripDrawsOneTintedCellPerBandWithoutCardOrSpine) {
    chart::FootprintCellsLayer layer;
    layer.presentation().enabled = true;
    layer.presentation().dustNotional = 0;
    auto series = ladderSeries(6);
    series.bars[0].bar.rows[0] = {0, 500, 1}; // bid-dominant lowest band
    ASSERT_TRUE(chart::FlowFeed::bin(layer.mailbox().back(), series, stripFrame(2)));
    layer.mailbox().post();
    std::unique_ptr<QSGNode> root(layer.createContainer());
    ChartTextRenderer text; ChartTextAtlas atlas;
    chart::LayerNodeContext context{nullptr, root.get(), text, atlas, false};
    auto frame = footprintChartFrame();
    frame.context.raster.colPxF = 10; frame.context.raster.rowPxF = 2; frame.context.raster.topF = 110 * 2;
    layer.sync(frame); layer.updateNodes(frame, context);
    EXPECT_EQ(layer.stage(), chart::FootprintStage::Strip);
    ASSERT_TRUE(layer.drawing());
    EXPECT_EQ(layer.vertexCount(), 3 * 6) << "one quad per band, no card or mid line";
    EXPECT_TRUE(layer.cards().empty());
    EXPECT_FALSE(layer.presentation().spine(frame.context.raster));
    auto* v = static_cast<QSGGeometryNode*>(root->firstChild())->geometry()->vertexDataAsColoredPoint2D();
    EXPECT_FLOAT_EQ(v[0].x, 0); EXPECT_FLOAT_EQ(v[2].x, 10) << "the band spans the whole column";
    const auto sell = chart::ChartLookSettings::footprintSellAggr(), buy = chart::ChartLookSettings::footprintBuyAggr();
    EXPECT_EQ(v[0].a, 255 * sell.alpha() / 255) << "the sell-dominant band is the bar's busiest";
    EXPECT_EQ(v[0].r, sell.red() * v[0].a / 255) << "sell aggression is the down colour";
    EXPECT_EQ(v[0].g, sell.green() * v[0].a / 255);
    EXPECT_EQ(v[6].g, buy.green() * v[6].a / 255) << "a buy-dominant band is the up colour";
    EXPECT_LT(v[12].a, v[6].a) << "intensity follows band volume";
    layer.submitLabels(frame, context);
    EXPECT_EQ(layer.glyphCount(), 0u);
}
namespace {
struct CellsRig {
    chart::FootprintCellsLayer layer;
    std::unique_ptr<QSGNode> root;
    ChartTextRenderer text; ChartTextAtlas atlas;
    chart::ChartFrame frame = footprintChartFrame();
    using Rows = decltype(FlowSeriesBuffer::Record{}.bar.rows);
    explicit CellsRig(Rows rows, double dust = 2500) {
        layer.presentation().enabled = true;
        layer.presentation().dustNotional = dust;
        auto series = footprintSeries();
        series.bars[0].bar.rows = std::move(rows);
        EXPECT_TRUE(chart::FlowFeed::bin(layer.mailbox().back(), series, footprintFrame()));
        layer.mailbox().post();
        root.reset(layer.createContainer());
        chart::LayerNodeContext context{nullptr, root.get(), text, atlas, false};
        layer.sync(frame); layer.updateNodes(frame, context);
    }
    const QSGGeometry::ColoredPoint2D* vertices() const {
        return static_cast<QSGGeometryNode*>(root->firstChild())->geometry()->vertexDataAsColoredPoint2D();
    }
};
}
TEST(FootprintAggressorColours, BuyIsGreenOnTheRightAndSellIsRedOnTheLeft) {
    const auto buy = chart::ChartLookSettings::footprintBuyAggr(), sell = chart::ChartLookSettings::footprintSellAggr();
    ASSERT_EQ(buy, theme::positive()); ASSERT_EQ(sell, theme::negative());
    CellsRig onlyBuy({{0, 0, 300}}, 0);   // row = {index, bidVol, askVol}: askVol is buy aggression
    ASSERT_EQ(onlyBuy.layer.vertexCount(), 12 + 6);
    const auto* v = onlyBuy.vertices() + 12;
    EXPECT_GT(v[0].x, 55) << "buy fill is the right half";
    EXPECT_EQ(v[0].g, buy.green() * v[0].a / 255); EXPECT_EQ(v[0].r, buy.red() * v[0].a / 255);
    EXPECT_GT(v[0].g, v[0].r) << "green, not red";
    CellsRig onlySell({{0, 300, 0}}, 0);
    ASSERT_EQ(onlySell.layer.vertexCount(), 12 + 6);
    v = onlySell.vertices() + 12;
    EXPECT_LT(v[2].x, 55) << "sell fill is the left half";
    EXPECT_EQ(v[0].r, sell.red() * v[0].a / 255); EXPECT_EQ(v[0].g, sell.green() * v[0].a / 255);
    EXPECT_GT(v[0].r, v[0].g) << "red, not green";
}
TEST(FootprintAggressorColours, FillIsFullHalfWidthAndOpacityTracksVolume) {
    CellsRig rig({{0, 100, 200}}, 0);
    ASSERT_EQ(rig.layer.vertexCount(), 12 + 12);
    const auto* sell = rig.vertices() + 12; const auto* buy = rig.vertices() + 18;
    const float sellWidth = sell[2].x - sell[0].x, buyWidth = buy[2].x - buy[0].x;
    EXPECT_FLOAT_EQ(sellWidth, buyWidth) << "both halves are filled across, whatever the volume";
    EXPECT_GT(sellWidth, 40);
    EXPECT_GT(buy[0].a, sell[0].a) << "twice the volume is more opaque";
    EXPECT_GE(sell[0].a, 45) << "the weakest fill stays visible (alpha .18)";
    EXPECT_NEAR(sell[0].a / 255., chart::flowFillAlpha(1, 2), 1. / 255);
}
TEST(FootprintAggressorColours, RenderedHalvesFollowAggressorColourFullWidthAndVolumeOpacity) {
    CellsRig rig({{0, 100, 200}}, 0);  // sell 1.0 on the left, buy 2.0 on the right, normaliser 2.0
    ASSERT_EQ(rig.layer.vertexCount(), 12 + 12);
    const auto* sell = rig.vertices() + 12; const auto* buy = rig.vertices() + 18;
    const auto up = theme::positive(), down = theme::negative();
    EXPECT_LT(sell[2].x, 55) << "sell aggression is the left half";
    EXPECT_GT(buy[0].x, 55) << "buy aggression is the right half";
    EXPECT_NEAR(sell[2].x - sell[0].x, buy[2].x - buy[0].x, 1e-4) << "both fills span their whole half";
    EXPECT_GT(sell[2].x - sell[0].x, 40);
    EXPECT_NEAR(sell[0].a, 150, 1) << "alpha .18 + .82 * (1 / 2)";
    EXPECT_NEAR(buy[0].a, 255, 1) << "full volume is opaque";
    EXPECT_NEAR(sell[0].r, down.red() * sell[0].a / 255., 1.5) << "sell is the down colour";
    EXPECT_NEAR(sell[0].g, down.green() * sell[0].a / 255., 1.5);
    EXPECT_NEAR(buy[0].r, up.red() * buy[0].a / 255., 1.5) << "buy is the up colour";
    EXPECT_NEAR(buy[0].g, up.green() * buy[0].a / 255., 1.5);
}
TEST(FootprintAggressorColours, DustDrawsItsFillAndDimsItsNumber) {
    CellsRig rig({{0, 1, 0}});  // 0.01 contracts at ~100 is far under the 2500 notional
    ASSERT_EQ(rig.layer.vertexCount(), 12 + 6) << "a dust cell still draws its fill";
    EXPECT_GT(rig.vertices()[12].a, 0);
    EXPECT_EQ(chart::flowTier(.01, 100, 1, 2500), chart::FlowTier::Dust);
    EXPECT_DOUBLE_EQ(chart::flowTextAlpha(chart::FlowTier::Dust), .45);
}
TEST(FootprintBands, StripBuyDominantBandIsGreen) {
    chart::FootprintCellsLayer layer;
    layer.presentation().enabled = true;
    layer.presentation().dustNotional = 0;
    auto series = ladderSeries(6);
    series.bars[0].bar.rows[0] = {0, 1, 500};
    ASSERT_TRUE(chart::FlowFeed::bin(layer.mailbox().back(), series, stripFrame(2)));
    layer.mailbox().post();
    std::unique_ptr<QSGNode> root(layer.createContainer());
    ChartTextRenderer text; ChartTextAtlas atlas;
    chart::LayerNodeContext context{nullptr, root.get(), text, atlas, false};
    auto frame = footprintChartFrame();
    frame.context.raster.colPxF = 10; frame.context.raster.rowPxF = 2; frame.context.raster.topF = 110 * 2;
    layer.sync(frame); layer.updateNodes(frame, context);
    auto* v = static_cast<QSGGeometryNode*>(root->firstChild())->geometry()->vertexDataAsColoredPoint2D();
    const auto buy = chart::ChartLookSettings::footprintBuyAggr();
    EXPECT_EQ(v[0].g, buy.green() * v[0].a / 255); EXPECT_GT(v[0].g, v[0].r);
}
TEST(FootprintCells, RegistrySlotAndNoData) {
    chart::ChartLayers layers;
    layers.heatmap.initialize();
    RegistryScene scene(layers.registry);
    auto& root = *scene.fields;
    EXPECT_EQ(root.childAtIndex(1)->type(), QSGNode::OpacityNodeType);
    EXPECT_EQ(root.childAtIndex(1)->firstChild()->type(), QSGNode::GeometryNodeType);
    auto frame = footprintChartFrame();
    layers.footprintCells.presentation().enabled = true;
    layers.footprintCells.sync(frame);
    EXPECT_FALSE(layers.footprintCells.drawing());
    EXPECT_EQ(layers.footprintCells.vertexCount(), 0);
}
TEST(FootprintEmptyState, CoverageDistinguishesLoadingRefusedAndProvenEmpty) {
    FlowSeriesBuffer::Series series;
    using State = chart::FlowSnapshot::WindowState;
    using Status = SentinelStreamClient::FlowStatus;
    auto frame = footprintFrame();
    frame.viewportTimeStart = 0; frame.viewportTimeEnd = 2000;
    frame.raster.drawnEndMs = 2000.25; // the raster extends into an unrequested bucket
    EXPECT_EQ(chart::FlowFeed::windowState(series, frame), State::Loading);
    series.coverage[0] = {0, 1000, Status::Complete};
    EXPECT_EQ(chart::FlowFeed::windowState(series, frame), State::Loading);
    series.coverage[1000] = {1000, 2000, Status::Building};
    EXPECT_EQ(chart::FlowFeed::windowState(series, frame), State::Loading);
    series.coverage[1000].status = Status::Refused;
    EXPECT_EQ(chart::FlowFeed::windowState(series, frame), State::Unavailable);
    series.coverage[1000].status = Status::Complete;
    EXPECT_EQ(chart::FlowFeed::windowState(series, frame), State::Ready);
    series.coverage.clear();
    series.coverage[0] = {0, 3600000, Status::Complete};
    series.coverage[1000] = {1000, 2000, Status::Partial};
    EXPECT_EQ(chart::FlowFeed::windowState(series, frame), State::Loading);
    series.coverage[1000].status = Status::Building;
    EXPECT_EQ(chart::FlowFeed::windowState(series, frame), State::Loading);
    series.coverage.erase(1000);
    EXPECT_EQ(chart::FlowFeed::windowState(series, frame), State::Ready);
}
TEST(FootprintEmptyState, MessageRequiresProvenEmptyVisibleWindow) {
    chart::FootprintCellsLayer layer;
    layer.presentation().enabled = true;
    auto frame = footprintChartFrame();
    auto publish = [&](const FlowSeriesBuffer::Series& series, chart::FlowSnapshot::WindowState state) {
        ASSERT_TRUE(chart::FlowFeed::bin(layer.mailbox().back(), series, footprintFrame()));
        layer.mailbox().back().windowState = state;
        layer.mailbox().post();
        layer.sync(frame);
    };
    FlowSeriesBuffer::Series empty;
    publish(empty, chart::FlowSnapshot::WindowState::Loading);
    EXPECT_TRUE(layer.emptyStateText(frame).isEmpty());
    publish(empty, chart::FlowSnapshot::WindowState::Unavailable);
    EXPECT_EQ(layer.emptyStateText(frame), "Footprint: trade data unavailable for this window");
    publish(empty, chart::FlowSnapshot::WindowState::Ready);
    EXPECT_EQ(layer.emptyStateText(frame), "Footprint: no trade data recorded for this window");
    frame.context.raster.drawnStartMs += 1;
    EXPECT_TRUE(layer.emptyStateText(frame).isEmpty()) << "the feed has not classified the new window yet";
    frame.context.raster.drawnStartMs -= 1;
    publish(footprintSeries(), chart::FlowSnapshot::WindowState::Ready);
    EXPECT_TRUE(layer.emptyStateText(frame).isEmpty());
    auto priceFrame = footprintFrame();
    priceFrame.raster.drawnMinPrice = 200; priceFrame.raster.drawnMaxPrice = 210;
    auto chartPriceFrame = frame; chartPriceFrame.context.raster = priceFrame.raster;
    ASSERT_TRUE(chart::FlowFeed::bin(layer.mailbox().back(), footprintSeries(), priceFrame));
    layer.mailbox().back().windowState = chart::FlowSnapshot::WindowState::Ready;
    EXPECT_EQ(layer.mailbox().back().visibleBarCount, 1);
    EXPECT_EQ(layer.mailbox().back().maxRowVolume, 0);
    layer.mailbox().post(); layer.sync(chartPriceFrame);
    EXPECT_TRUE(layer.emptyStateText(chartPriceFrame).isEmpty());
    ASSERT_TRUE(chart::FlowFeed::bin(layer.mailbox().back(), footprintSeries(0.6), footprintFrame()));
    EXPECT_EQ(layer.mailbox().back().barCount, 0);
    EXPECT_EQ(layer.mailbox().back().skippedBars, 1);
    EXPECT_EQ(layer.mailbox().back().visibleBarCount, 1) << "invalid geometry still means recorded trade data";
    layer.mailbox().back().windowState = chart::FlowSnapshot::WindowState::Ready;
    layer.mailbox().post(); layer.sync(frame);
    EXPECT_TRUE(layer.emptyStateText(frame).isEmpty());
    auto timeFrame = footprintFrame();
    timeFrame.viewportTimeStart = 1000; timeFrame.viewportTimeEnd = 2000;
    ASSERT_TRUE(chart::FlowFeed::bin(layer.mailbox().back(), footprintSeries(), timeFrame));
    EXPECT_EQ(layer.mailbox().back().visibleBarCount, 0) << "drawn overscan is outside the visible window";
    frame.context.overlays.footprint = false;
    EXPECT_TRUE(layer.emptyStateText(frame).isEmpty());
}
TEST(VolumeProfileEmptyState, LoadingEmptyAndVisibleDataHaveDistinctResults) {
    chart::VolumeProfileLayer layer;
    QSGNode root;
    ChartTextRenderer text;
    ChartTextAtlas atlas;
    chart::LayerNodeContext context{nullptr, &root, text, atlas, false};
    auto frame = footprintChartFrame();
    frame.context.overlays.volumeProfile = true;
    frame.context.mapping.viewMinPrice = 95;
    frame.context.mapping.viewMaxPrice = 105;
    layer.updateNodes(frame, context);
    EXPECT_TRUE(layer.emptyStateText(frame).isEmpty());
    VolumeProfileState::Snapshot snap;
    snap.sessionStartMs = 500; snap.sessionEndMs = 1500;
    snap.minPrice = 95; snap.tickSize = 1; snap.gridHeight = 10;
    layer.renderer.enqueue({}, snap);
    layer.updateNodes(frame, context);
    EXPECT_EQ(layer.emptyStateText(frame), "Volume profile: no trade data for this window");
    layer.renderer.enqueue(std::vector<float>(10, 0), snap);
    layer.updateNodes(frame, context);
    EXPECT_EQ(layer.emptyStateText(frame), "Volume profile: no trade data for this window");
    layer.renderer.enqueue(std::vector<float>(10, 1), snap);
    layer.updateNodes(frame, context);
    EXPECT_TRUE(layer.emptyStateText(frame).isEmpty());
    ASSERT_EQ(root.childCount(), 3);
    auto* bars = static_cast<QSGGeometryNode*>(root.firstChild()->nextSibling())->geometry();
    EXPECT_NE(bars->indexDataAsUShort()[0], bars->indexDataAsUShort()[1]);
    frame.context.raster.drawnStartMs = 3000; frame.context.raster.drawnEndMs = 5000;
    layer.updateNodes(frame, context);
    EXPECT_TRUE(layer.emptyStateText(frame).isEmpty()) << "the current-session bars still draw on a past window";
    EXPECT_NE(bars->indexDataAsUShort()[0], bars->indexDataAsUShort()[1]);
    frame.context.mapping.viewMinPrice = 200; frame.context.mapping.viewMaxPrice = 210;
    layer.updateNodes(frame, context);
    EXPECT_EQ(layer.emptyStateText(frame), "Volume profile: current session only");
    layer.renderer.enqueue(std::vector<float>(10, 0), snap);
    layer.updateNodes(frame, context);
    EXPECT_EQ(layer.emptyStateText(frame), "Volume profile: current session only");
    layer.renderer.enqueue(std::vector<float>(10, 1), snap);
    frame.context.raster.drawnStartMs = 0; frame.context.raster.drawnEndMs = 2000;
    layer.updateNodes(frame, context);
    EXPECT_TRUE(layer.emptyStateText(frame).isEmpty()) << "profile volume exists outside this price range";
    frame.context.overlays.volumeProfile = false;
    EXPECT_TRUE(layer.emptyStateText(frame).isEmpty());
}
TEST(ChartEmptyStateText, EmptyLabelsUseMsdfAndAvoidVisibleCandles) {
    QTemporaryDir cache;
    QString error;
    ASSERT_TRUE(AgentHostMode::activate(cache.path(), {}, &error)) << error.toStdString();
    struct Reset { ~Reset() { AgentHostMode::resetForTests(); } } reset;
    chart::ChartLayers layers;
    auto& atlas = layers.text.atlas;
    ChartTextAtlas::BuildParams params;
    params.fontPath = QStringLiteral(SENTINEL_SOURCE_DIR "/resources/fonts/RobotoMono/RobotoMono-Regular.ttf");
    params.fontPx = 64;
    params.charset = " 0123456789.+-,:/<>$%kMBABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    ASSERT_TRUE(atlas.build(params));
    auto frame = footprintChartFrame();
    frame.context.surfaceBounds = {0, 0, 800, 400};
    frame.context.itemBounds = frame.context.surfaceBounds;
    layers.footprintCells.presentation().enabled = true;
    FlowSeriesBuffer::Series empty;
    ASSERT_TRUE(chart::FlowFeed::bin(layers.footprintCells.mailbox().back(), empty, footprintFrame()));
    layers.footprintCells.mailbox().back().windowState = chart::FlowSnapshot::WindowState::Ready;
    layers.footprintCells.mailbox().post();
    QSGNode root;
    chart::LayerNodeContext context{nullptr, &root, layers.text.renderer, atlas, true};
    for (double dpr : {1.0, 2.0}) {
        frame.context.surfaceDpr = dpr;
        frame.context.raster.dpr = dpr;
        layers.footprintCells.sync(frame);
        layers.text.updateNodes(frame, context);
        const auto& glyphs = layers.text.emptyGlyphs();
        ASSERT_GT(glyphs.size(), 30u);
        EXPECT_EQ(glyphs.size(), size_t(layers.text.renderer.submittedGlyphs()));
        double left = 1e9, right = -1e9, top = 1e9, bottom = -1e9;
        for (const auto& glyph : glyphs) {
            left = std::min(left, glyph.rect.left()); right = std::max(right, glyph.rect.right());
            top = std::min(top, glyph.rect.top()); bottom = std::max(bottom, glyph.rect.bottom());
            EXPECT_EQ(glyph.color, QColor(143, 163, 184));
        }
        EXPECT_NEAR((left + right) * 0.5, frame.context.surfaceBounds.center().x(), 2.0);
        EXPECT_NEAR((top + bottom) * 0.5, frame.context.surfaceBounds.center().y(), 2.0);
    }
    chart::CandleBar candle;
    candle.timeStartMs = 0; candle.timeEndMs = 2000;
    candle.high = 95; candle.low = 92;
    frame.context.surfaceDpr = 1; frame.context.raster.dpr = 1;
    frame.context.raster.colPxF = 400;
    layers.text.setCandleEnvelope({candle}, 2000);
    layers.text.sync(frame);
    layers.text.updateNodes(frame, context);
    ASSERT_FALSE(layers.text.emptyGlyphs().empty());
    double labelBottom = -1e9;
    for (const auto& glyph : layers.text.emptyGlyphs()) labelBottom = std::max(labelBottom, glyph.rect.bottom());
    EXPECT_LT(labelBottom, 177.0) << "the centre text box crosses this candle's wick";
    layers.footprintCells.mailbox().back().windowState = chart::FlowSnapshot::WindowState::Loading;
    layers.footprintCells.mailbox().post();
    layers.footprintCells.sync(frame);
    layers.text.updateNodes(frame, context);
    EXPECT_TRUE(layers.text.emptyGlyphs().empty());

    frame.context.overlays.footprint = false;
    frame.context.overlays.volumeProfile = true;
    frame.context.mapping.viewMinPrice = 95;
    frame.context.mapping.viewMaxPrice = 105;
    VolumeProfileState::Snapshot snap;
    snap.sessionStartMs = 500; snap.sessionEndMs = 1500;
    snap.minPrice = 95; snap.tickSize = 1; snap.gridHeight = 10;
    std::unique_ptr<QSGNode> vpRoot(layers.vp.createContainer());
    chart::LayerNodeContext vpContext{nullptr, vpRoot.get(), layers.text.renderer, atlas, true};
    layers.vp.renderer.enqueue(std::vector<float>(10, 0), snap);
    for (double dpr : {1.0, 2.0}) {
        frame.context.surfaceDpr = dpr;
        frame.context.raster.dpr = dpr;
        layers.vp.updateNodes(frame, vpContext);
        layers.text.updateNodes(frame, context);
        EXPECT_GT(layers.text.emptyGlyphs().size(), 30u);
        EXPECT_EQ(layers.text.emptyGlyphs().size(), size_t(layers.text.renderer.submittedGlyphs()));
    }
    layers.vp.renderer.enqueue(std::vector<float>(10, 1), snap);
    layers.vp.updateNodes(frame, vpContext);
    layers.text.updateNodes(frame, context);
    EXPECT_TRUE(layers.text.emptyGlyphs().empty());
}
TEST(FootprintCells, TwoFramesAfterDataAndChangedSnapshotAllocateNothing) {
    chart::FootprintCellsLayer layer;
    layer.presentation().enabled = true;
    auto frame = footprintChartFrame();
    ASSERT_TRUE(chart::FlowFeed::bin(layer.mailbox().back(), footprintSeries(), footprintFrame()));
    layer.mailbox().post();
    std::unique_ptr<QSGNode> root(layer.createContainer());
    ChartTextRenderer text; ChartTextAtlas atlas;
    chart::LayerNodeContext context{nullptr, root.get(), text, atlas, false};
    layer.sync(frame); layer.updateNodes(frame, context);
    ASSERT_TRUE(layer.drawing()); ASSERT_EQ(layer.vertexCount(), 12 + 2 * 12) << "card, spine and a fill per side for each visible row, dust included";
    EXPECT_EQ(layer.stage(), chart::FootprintStage::Numbers);
    EXPECT_TRUE(layer.presentation().spineBar(frame.context.raster, 0));
    EXPECT_FALSE(layer.presentation().spineBar(frame.context.raster, 1000));
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
    EXPECT_FLOAT_EQ(vertices[0].x, 0); EXPECT_FLOAT_EQ(vertices[2].x, 100);
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
    EXPECT_TRUE(heatmap::applySettingsPatch(value, {{"footprintIntensity", "window"},
        {"footprintCandle", "full"}, {"footprintNumbers", false}}).isEmpty());
    store.save("test", value);
    auto loaded = store.load("test", ClientHeatmapConfig{});
    EXPECT_EQ(loaded.footprintIntensity, "window");
    EXPECT_EQ(loaded.footprintCandle, "full"); EXPECT_FALSE(loaded.footprintNumbers);
    EXPECT_FALSE(heatmap::applySettingsPatch(loaded, {{"footprintIntensity", "bad"}, {"footprintNumbers", true}}).isEmpty());
    EXPECT_EQ(loaded.footprintIntensity, "window"); EXPECT_FALSE(loaded.footprintNumbers);
    chart::LayerVisibility visibility;
    EXPECT_TRUE(visibility.setProperty("footprintIntensity", "window"));
    EXPECT_TRUE(visibility.setProperty("footprintNumbers", false));
    EXPECT_TRUE(visibility.setProperty("footprintCandle", "full"));
    EXPECT_EQ(visibility.footprintIntensity(), "window");
    EXPECT_FALSE(visibility.footprintNumbers()); EXPECT_EQ(visibility.footprintCandle(), "full");
}
TEST(FootprintCells, FootprintHasNoSourceSettingAndStaleSavedKeyIsIgnored) {
    heatmap::HeatmapChartSettings value;
    const auto error = heatmap::applySettingsPatch(value, {{"footprintSource", "cells"}});
    EXPECT_EQ(error, "Unknown heatmap setting: footprintSource");
    EXPECT_FALSE(heatmap::settingsJson(value).contains("footprintSource"));
    chart::LayerVisibility visibility;
    EXPECT_LT(visibility.metaObject()->indexOfProperty("footprintSource"), 0);
    QTemporaryDir dir;
    QSettings settings(dir.filePath("chart.ini"), QSettings::IniFormat);
    settings.setValue("heatmap/test/footprintSource", "q16");
    settings.setValue("heatmap/test/footprintIntensity", "window");
    heatmap::HeatmapSettingsStore store(settings);
    const auto loaded = store.load("test", ClientHeatmapConfig{});
    EXPECT_EQ(loaded.footprintIntensity, "window");
    EXPECT_EQ(loaded.footprintCandle, heatmap::HeatmapChartSettings{}.footprintCandle);
}
} // namespace

TEST(FootprintCells, NumbersLayoutIsBoundedAndAllocatesNothingAfterWarmup) {
    QTemporaryDir cache;
    QString error;
    ASSERT_TRUE(AgentHostMode::activate(cache.path(), {}, &error)) << error.toStdString();
    struct Reset { ~Reset() { AgentHostMode::resetForTests(); } } reset;
    ChartTextAtlas atlas;
    ChartTextAtlas::BuildParams params;
    params.fontPath = QStringLiteral(SENTINEL_SOURCE_DIR "/resources/fonts/RobotoMono/RobotoMono-Regular.ttf");
    params.fontPx = 64; params.charset = " 0123456789.+-,:/<>$%kMBABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    ASSERT_TRUE(atlas.build(params));
    const double widest = chart::kFlowMagnitudeGlyphs * atlas.glyph(QChar('8')).advance * chart::kFootprintTextPx / atlas.fontPx();
    EXPECT_LE(widest, (chart::kFootprintNumbersFitColumn - chart::kFootprintCandleSpace) / 2 - 3);
    chart::FootprintCellsLayer layer;
    layer.presentation().enabled = true;
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
    // Zero side is absent, while the other side keeps its full formatted run.
    auto& next = layer.mailbox().back();
    next = layer.current();
    next.bars[0].cellCount = 1; next.cellCount = 1;
    next.cells[0].bidVol = 0; next.cells[0].askVol = 245;
    layer.mailbox().post();
    layer.sync(frame); layer.updateNodes(frame, context); layer.submitLabels(frame, context);
    ASSERT_EQ(layer.glyphCount(), 4u); // 2.45, no zero-side glyph
    frame.context.raster.rowPxF = chart::kFootprintNumbersRow;
    frame.context.raster.topF = 105 * chart::kFootprintNumbersRow;
    layer.sync(frame); layer.updateNodes(frame, context); layer.submitLabels(frame, context);
    EXPECT_EQ(layer.glyphCount(), 4u) << "13 px numbers fit at the row entry threshold with the full chart atlas";
    for (const auto& glyph : layer.glyphs()) EXPECT_NEAR(glyph.color.alphaF(), .45, .005);
    layer.presentation().dustNotional = 0;
    layer.updateNodes(frame, context); layer.submitLabels(frame, context);
    for (const auto& glyph : layer.glyphs()) EXPECT_DOUBLE_EQ(glyph.color.alphaF(), 1);
    // Rebuild warmed geometry and label runs, not merely a cache hit.
    for (int i = 0; i < 3; ++i) {
        auto& changed = layer.mailbox().back(); changed = layer.current();
        changed.cells[0].askVol = 250 + i;
        layer.mailbox().post();
        footprintAllocations = 0; footprintCounting = true;
        layer.sync(frame); layer.updateNodes(frame, context); layer.submitLabels(frame, context);
        footprintCounting = false;
        EXPECT_EQ(footprintAllocations.load(), 0u);
    }
    auto& widestRow = layer.mailbox().back(); widestRow = layer.current();
    widestRow.cells[0].bidVol = 123400; widestRow.cells[0].askVol = 123400;
    layer.mailbox().post();
    frame.context.raster.colPxF = chart::kFootprintNumbersColumn;
    frame.context.raster.rowPxF = chart::kFootprintNumbersRow;
    frame.context.raster.topF = 105 * frame.context.raster.rowPxF;
    layer.sync(frame); layer.updateNodes(frame, context); layer.submitLabels(frame, context);
    ASSERT_EQ(layer.glyphCount(), 10u); // 1.23k on both sides
    frame.context.raster.colPxF = chart::kFootprintNumbersFitColumn;
    frame.context.raster.rowPxF = chart::kFootprintNumbersFitRow;
    frame.context.raster.topF = 105 * frame.context.raster.rowPxF;
    layer.sync(frame); layer.updateNodes(frame, context); layer.submitLabels(frame, context);
    EXPECT_EQ(layer.stage(), chart::FootprintStage::Numbers);
    EXPECT_EQ(layer.glyphCount(), 10u) << "widest labels survive at the hysteresis exit edge";
    layer.presentation().numbers = false;
    layer.sync(frame); layer.submitLabels(frame, context);
    EXPECT_EQ(layer.glyphCount(), 0u);
    EXPECT_TRUE(layer.drawing());
}

TEST(FootprintSource, CellsAreTheOnlyFootprintLayerAndNeedNoSourceSetting) {
    chart::ChartLayers layers;
    EXPECT_FALSE(layers.registry.precedes("footprint", "footprintCells"));
    EXPECT_FALSE(layers.registry.precedes("footprint", "VP"));
    EXPECT_TRUE(layers.registry.precedes("heatmap", "footprintCells"));
    EXPECT_TRUE(layers.registry.precedes("footprintCells", "VP"));
    auto frame = footprintChartFrame();
    auto& look = layers.footprintCells.presentation();
    look.enabled = true;
    ASSERT_TRUE(chart::FlowFeed::bin(layers.footprintCells.mailbox().back(), footprintSeries(), footprintFrame()));
    layers.footprintCells.mailbox().post();
    layers.footprintCells.sync(frame);
    EXPECT_TRUE(layers.footprintCells.drawing()) << "enabled footprint draws cells with no source choice";
    EXPECT_TRUE(look.spineBar(frame.context.raster, 0));
    EXPECT_FALSE(look.spineBar(frame.context.raster, 1000));
    frame.context.overlays.footprint = false;
    layers.footprintCells.sync(frame);
    EXPECT_FALSE(layers.footprintCells.drawing());
}

TEST(FootprintMotion, OverscanRetainsVisibleRowsAcrossPanAndFollowLiveWithoutAllocation) {
    chart::FootprintCellsLayer layer;
    layer.presentation().windowIntensity = true;
    layer.presentation().enabled = true;
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
    ASSERT_TRUE(layer.presentation().spine(frame.context.raster));
    // 1 px, quarter viewport, follow-live column, and beyond overscan containment.
    for (double dx : {1., 28., 56., 168.}) {
        frame = footprintChartFrame();
        auto& r = frame.context.raster;
        r.leftF += dx; r.drawnStartMs += dx / 100 * 1000; r.drawnEndMs += dx / 100 * 1000;
        r.topF += 1; r.drawnMinPrice += 1. / 16; r.drawnMaxPrice += 1. / 16;
        footprintAllocations = 0; footprintCounting = true;
        layer.sync(frame); layer.updateNodes(frame, context);
        footprintCounting = false;
        EXPECT_EQ(footprintAllocations.load(), 0u);
        EXPECT_TRUE(layer.drawing()); EXPECT_GT(layer.vertexCount(), 0);
        EXPECT_TRUE(layer.presentation().spineBar(r, 1000));
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
    RegistryScene scene(layers.registry);
    auto& root = *scene.foreground->firstChild();
    ASSERT_EQ(root.childCount(), 2);
    auto* textRoot = root.lastChild();
    ChartTextRenderer text; ChartTextAtlas atlas;
    chart::LayerNodeContext context{nullptr, &root, text, atlas, false};
    auto frame = footprintChartFrame();
    layers.registry.sync(frame); layers.registry.updateNodes(frame, context);
    EXPECT_EQ(root.childCount(), 2); EXPECT_EQ(root.lastChild(), textRoot);
    layers.cvd.presentation().stripPx = 50;
    EXPECT_FALSE(layers.cvd.presentation().drawable(1000));
    chart::CvdFeed::build(layers.cvd.mailbox().back(), footprintSeries(), footprintFrame(), chart::CvdReset::Visible, 4);
    layers.cvd.mailbox().post();
    EXPECT_TRUE(layers.cvd.presentation().drawable(1000));
    EXPECT_FALSE(layers.cvd.presentation().drawable(60000));
    layers.registry.sync(frame); layers.registry.updateNodes(frame, context);
    ASSERT_EQ(root.childCount(), 3); EXPECT_EQ(root.lastChild(), textRoot);
    EXPECT_EQ(root.childAtIndex(1)->type(), QSGNode::OpacityNodeType);
    EXPECT_TRUE(layers.cvd.drawing());
    layers.cvd.presentation().stripPx = 0;
    layers.registry.sync(frame); layers.registry.updateNodes(frame, context);
    EXPECT_FALSE(layers.cvd.drawing());
    EXPECT_EQ(static_cast<QSGOpacityNode*>(root.childAtIndex(1))->opacity(), 0);
}
TEST(CvdLayer, LineAreaBoundsAndFreshFramesAllocateNothing) {
    chart::CvdLayer layer;
    layer.presentation().reset = chart::CvdReset::UtcDay;
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
    const auto lineVertex = vertices[19];
    layer.presentation().area = true;
    for (int i = 0; i < 2; ++i) {
        chart::CvdFeed::build(layer.mailbox().back(), series, footprintFrame(), chart::CvdReset::UtcDay, 4);
        layer.mailbox().post();
        footprintAllocations = 0; footprintCounting = true;
        layer.sync(frame); layer.updateNodes(frame, context); layer.submitLabels(frame, context);
        footprintCounting = false;
        EXPECT_EQ(footprintAllocations.load(), 0u);
    }
    EXPECT_NE(vertices[19].y, lineVertex.y); EXPECT_EQ(vertices[19].a, 110);
    bool green = false, red = false;
    for (int i = 0; i < layer.vertexCount(); ++i) {
        EXPECT_GE(vertices[i].x, 0); EXPECT_LE(vertices[i].x, i < 12 ? layer.gutter().right() : 112);
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
using CvdVertex = QSGGeometry::ColoredPoint2D;
std::vector<CvdVertex> cvdAreaVertices(const std::array<double, 3>& values, chart::CvdFillAnchor anchor,
                                       bool area = true, int stripPx = 60) {
    chart::CvdLayer layer;
    layer.presentation().stripPx = stripPx;
    layer.presentation().area = area;
    layer.presentation().fillAnchor = anchor;
    auto frame = footprintChartFrame();
    frame.context.surfaceBounds = {0, 0, 300, 100};
    frame.context.itemBounds = frame.context.surfaceBounds;
    frame.context.raster.widthDev = 300;
    frame.context.raster.drawnEndMs = 3000;
    auto& snapshot = layer.mailbox().back();
    snapshot.series.clear(); snapshot.timeframeMs = 1000; snapshot.reset = chart::CvdReset::Visible;
    for (int i = 0; i < 3; ++i) snapshot.series.points[size_t(i)] = {i * 1000, 0, values[size_t(i)], 0};
    snapshot.series.count = 3;
    layer.mailbox().post(); layer.sync(frame);
    std::unique_ptr<QSGNode> root(layer.createContainer());
    ChartTextRenderer text; ChartTextAtlas atlas;
    chart::LayerNodeContext context{nullptr, root.get(), text, atlas, false};
    layer.updateNodes(frame, context);
    const auto* vertices = static_cast<QSGGeometryNode*>(root->firstChild())->geometry()->vertexDataAsColoredPoint2D();
    return {vertices, vertices + layer.vertexCount()};
}
TEST(CvdLayer, AutoAnchorsNegativeAreaAtTopEdgeAndPositiveAtBottomEdge) {
    const auto negative = cvdAreaVertices({-1130, -1060, -1100}, chart::CvdFillAnchor::Auto);
    const auto positive = cvdAreaVertices({1130, 1060, 1100}, chart::CvdFillAnchor::Auto);
    const auto forcedEdge = cvdAreaVertices({-1130, -1060, -1100}, chart::CvdFillAnchor::Edge);
    // The panel and divider consume 12 vertices; an off-screen zero emits no zero line.
    ASSERT_GE(negative.size(), 30u); ASSERT_GE(positive.size(), 30u);
    const chart::CvdStripLayout layout{60, {}};
    const float top = float(40 + layout.lineY(1));
    const float bottom = float(40 + layout.lineY(0));
    EXPECT_FLOAT_EQ(negative[13].y, top);
    EXPECT_GT(negative[12].y, 40); EXPECT_LT(negative[12].y, 100);
    EXPECT_FLOAT_EQ(positive[13].y, bottom);
    EXPECT_FLOAT_EQ(forcedEdge[13].y, top);
    EXPECT_GT(positive[12].y, 40); EXPECT_LT(positive[12].y, 100);
    for (size_t i = 12; i < negative.size(); ++i) {
        EXPECT_GE(negative[i].y, top); EXPECT_LE(negative[i].y, bottom);
        EXPECT_GE(positive[i].y, top); EXPECT_LE(positive[i].y, bottom);
    }
}
TEST(CvdLayer, EdgeFillUsesSeriesSignWhenFirstPointOrFlatBarIsAtExtreme) {
    const chart::CvdStripLayout layout{60, {}};
    const float top = float(40 + layout.lineY(1));
    const float bottom = float(40 + layout.lineY(0));
    for (const auto& values : {std::array<double, 3>{-1060, -1100, -1130},
                               std::array<double, 3>{-1060, -1060, -1130}}) {
        const auto vertices = cvdAreaVertices(values, chart::CvdFillAnchor::Auto);
        ASSERT_GE(vertices.size(), 24u);
        for (const size_t first : {12u, 18u}) {
            EXPECT_GT(vertices[first].r, vertices[first].g);
            EXPECT_FLOAT_EQ(vertices[first + 1].y, top);
        }
    }
    for (const auto& values : {std::array<double, 3>{1060, 1100, 1130},
                               std::array<double, 3>{1060, 1060, 1130}}) {
        const auto vertices = cvdAreaVertices(values, chart::CvdFillAnchor::Auto);
        ASSERT_GE(vertices.size(), 24u);
        for (const size_t first : {12u, 18u}) {
            EXPECT_GT(vertices[first].g, vertices[first].r);
            EXPECT_FLOAT_EQ(vertices[first + 1].y, bottom);
        }
    }
}
TEST(CvdLayer, EdgeFillStaysBelowDividerAndTitleBand) {
    constexpr int stripPx = 80;
    const chart::CvdStripLayout layout{stripPx, {}};
    ASSERT_TRUE(layout.titleFits());
    const float plotTop = float(100 - stripPx + layout.lineY(1));
    const float plotBottom = float(100 - stripPx + layout.lineY(0));
    for (const auto& values : {std::array<double, 3>{-1060, -1100, -1130},
                               std::array<double, 3>{1060, 1100, 1130}}) {
        const auto vertices = cvdAreaVertices(values, chart::CvdFillAnchor::Auto, true, stripPx);
        ASSERT_GE(vertices.size(), 30u);
        for (size_t i = 12; i < vertices.size(); ++i) {
            EXPECT_GE(vertices[i].y, plotTop);
            EXPECT_LE(vertices[i].y, plotBottom);
        }
    }
}
TEST(CvdLayer, CrossingZeroKeepsZeroFillAndNoneUsesLineGeometry) {
    const std::array<double, 3> values{-10, 5, -3};
    const auto automatic = cvdAreaVertices(values, chart::CvdFillAnchor::Auto);
    const auto legacy = cvdAreaVertices(values, chart::CvdFillAnchor::Zero);
    const auto forcedEdge = cvdAreaVertices(values, chart::CvdFillAnchor::Edge);
    ASSERT_EQ(automatic.size(), legacy.size());
    ASSERT_EQ(forcedEdge.size(), legacy.size());
    for (size_t i = 0; i < automatic.size(); ++i) {
        EXPECT_FLOAT_EQ(automatic[i].x, legacy[i].x);
        EXPECT_FLOAT_EQ(automatic[i].y, legacy[i].y);
        EXPECT_EQ(automatic[i].a, legacy[i].a);
        EXPECT_FLOAT_EQ(forcedEdge[i].x, legacy[i].x);
        EXPECT_FLOAT_EQ(forcedEdge[i].y, legacy[i].y);
        EXPECT_EQ(forcedEdge[i].a, legacy[i].a);
    }
    const auto forcedZero = cvdAreaVertices({-1130, -1060, -1100}, chart::CvdFillAnchor::Zero);
    EXPECT_NE(forcedZero[13].y, 40);
    const auto none = cvdAreaVertices(values, chart::CvdFillAnchor::None);
    const auto line = cvdAreaVertices(values, chart::CvdFillAnchor::Auto, false);
    ASSERT_EQ(none.size(), line.size());
    for (size_t i = 0; i < none.size(); ++i) {
        EXPECT_FLOAT_EQ(none[i].x, line[i].x);
        EXPECT_FLOAT_EQ(none[i].y, line[i].y);
        EXPECT_EQ(none[i].a, line[i].a);
    }
}
TEST(CvdFeed, SumsAllLoadedBarsAndRespectsTpoOpenAndBaseScale) {
    auto source = footprintSeries();
    auto frame = footprintFrame();
    frame.raster.drawnStartMs = 1000;
    source.bars[1000] = source.bars[0]; source.bars[1000].bar.startMs = 1000;
    source.bars[1000].bar.baseScale = 50;
    chart::CvdSnapshot snapshot;
    chart::CvdFeed::build(snapshot, source, frame, chart::CvdReset::UtcDay, 4);
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
    params.fontPx = 64; params.charset = " 0123456789.e+-CVDsinceTPOUdaylotrvw";
    ASSERT_TRUE(atlas.build(params));
    chart::CvdLayer layer;
    layer.presentation().stripPx = 80;
    auto frame = footprintChartFrame(); frame.context.surfaceBounds.setWidth(400);
    frame.context.itemBounds = frame.context.surfaceBounds;
    auto labelledSeries = footprintSeries();
    labelledSeries.bars[1000] = labelledSeries.bars[0];
    labelledSeries.bars[1000].bar.startMs = 1000;
    chart::CvdFeed::build(layer.mailbox().back(), labelledSeries, footprintFrame(), chart::CvdReset::Visible, 4);
    layer.mailbox().post(); layer.sync(frame);
    std::unique_ptr<QSGNode> root(layer.createContainer());
    ChartTextRenderer text;
    chart::LayerNodeContext context{nullptr, root.get(), text, atlas, true};
    layer.updateNodes(frame, context); layer.submitLabels(frame, context);
    const std::string_view title(layer.current().label.data());
    const size_t titleCount = std::count_if(title.begin(), title.end(), [](char c) { return c != ' '; });
    ASSERT_EQ(text.submittedGlyphs(), int(titleCount) + 9)
        << "3.00 maximum, zero minimum and 3.00 last must all have glyphs";
    const auto glyphs = layer.labelGlyphs();
    QRectF maximum, minimum, last;
    for (size_t i = titleCount; i < glyphs.size(); ++i) {
        EXPECT_TRUE(layer.gutter().contains(glyphs[i].rect));
        EXPECT_FALSE(layer.strip().intersects(glyphs[i].rect));
        auto& box = i < titleCount + 4 ? maximum : i < titleCount + 5 ? minimum : last;
        box = box.united(glyphs[i].rect);
    }
    EXPECT_FALSE(maximum.intersects(last)); EXPECT_FALSE(minimum.intersects(last));
    for (int height : {20, 21, 39, 40, 58, 59, 77, 78, 80}) {
        SCOPED_TRACE(height);
        layer.presentation().stripPx = height;
        layer.sync(frame); layer.updateNodes(frame, context);
        text.beginFrame(root.get(), nullptr, atlas);
        layer.submitLabels(frame, context);
        const int values = height >= 59 ? 9 : height >= 40 ? 5 : height >= 21 ? 4 : 0;
        const int titleGlyphs = height >= 78 ? int(titleCount) : 0;
        EXPECT_EQ(text.submittedGlyphs(), values + titleGlyphs);
        for (const auto& glyph : layer.labelGlyphs())
            EXPECT_TRUE(layer.strip().contains(glyph.rect) || layer.gutter().contains(glyph.rect));
        text.endFrame();
    }
    for (int i = 0; i < 2; ++i) {
        auto series = footprintSeries(); series.bars[0].bar.delta += i + 1;
        chart::CvdFeed::build(layer.mailbox().back(), series, footprintFrame(), chart::CvdReset::Visible, 4);
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

TEST(CvdFeed, CoverageAndEachResetPeriodDetermineTheBaselineLabel) {
    auto series = footprintSeries();
    auto frame = footprintFrame();
    constexpr int64_t hour = 3600000;
    auto record = series.bars[0]; series.bars.clear();
    record.bar.startMs = 9 * hour; series.bars[9 * hour] = record;
    frame.raster.drawnStartMs = 9 * hour; frame.raster.drawnEndMs = 9 * hour + 1000;
    chart::CvdSnapshot snapshot;
    chart::CvdFeed::build(snapshot, series, frame, chart::CvdReset::UtcDay, 4);
    EXPECT_STREQ(snapshot.label.data(), "CVD since 09:00 loaded");
    series.coverage[0] = {0, 9 * hour, SentinelStreamClient::FlowStatus::Complete};
    chart::CvdFeed::build(snapshot, series, frame, chart::CvdReset::UtcDay, 4);
    EXPECT_STREQ(snapshot.label.data(), "CVD since UTC day");
    series.coverage[0].status = SentinelStreamClient::FlowStatus::Building;
    chart::CvdFeed::build(snapshot, series, frame, chart::CvdReset::UtcDay, 4);
    EXPECT_STREQ(snapshot.label.data(), "CVD since 09:00 loaded");
    record.bar.startMs = 86400000; series.bars[record.bar.startMs] = record;
    frame.raster.drawnEndMs = 86401000;
    chart::CvdFeed::build(snapshot, series, frame, chart::CvdReset::UtcDay, 4);
    EXPECT_STREQ(snapshot.label.data(), "CVD since 1970-01-01 09:00 loaded");
    chart::CvdFeed::build(snapshot, series, frame, chart::CvdReset::Visible, 4);
    EXPECT_STREQ(snapshot.label.data(), "CVD since view start");
    frame.raster.drawnStartMs = 86400000;
    chart::CvdFeed::build(snapshot, series, frame, chart::CvdReset::UtcDay, 4);
    EXPECT_STREQ(snapshot.label.data(), "CVD since UTC day");
}

TEST(CvdFeed, VisibleStartsInsideDrawnWindowAndReanchorsOnPan) {
    auto source = footprintSeries(); source.bars.clear();
    auto frame = footprintFrame();
    for (int i = -20; i < 8; ++i) {
        auto& bar = source.bars[i * 1000].bar;
        bar.startMs = i * 1000; bar.timeframeMs = 1000; bar.baseScale = 2; bar.delta = 2 * (i + 1);
    }
    frame.raster.drawnStartMs = 500; frame.raster.drawnEndMs = 6000;
    chart::CvdSnapshot first, panned;
    chart::CvdFeed::build(first, source, frame, chart::CvdReset::Visible, 4);
    ASSERT_EQ(first.series.count, 5);
    EXPECT_EQ(first.series.points[0].timeMs, 1000); EXPECT_EQ(first.series.points[0].value, 0);
    EXPECT_EQ(first.series.points[4].value, 18); EXPECT_STREQ(first.label.data(), "CVD since view start");
    frame.raster.drawnStartMs = 2500;
    chart::CvdFeed::build(panned, source, frame, chart::CvdReset::Visible, 4);
    ASSERT_EQ(panned.series.count, 3); EXPECT_EQ(panned.series.points[0].timeMs, 3000);
    EXPECT_EQ(panned.series.points[0].value, 0);
    for (int i = 0; i < 3; ++i) EXPECT_EQ(first.series.points[size_t(i + 2)].value - panned.series.points[size_t(i)].value, 7);
    source.bars[5000].bar.delta = -2;
    chart::CvdFeed::build(panned, source, frame, chart::CvdReset::Visible, 4);
    ASSERT_EQ(panned.series.count, 3); EXPECT_EQ(panned.series.points[2].value, 4);
}

TEST(FlowFooter, TotalsAndCumulativeShareEveryResetAndIgnoreMissingBars) {
    auto source = footprintSeries();
    const auto original = source.bars.at(0);
    source.bars.clear();
    constexpr int64_t open = 13 * 3600000;
    for (int i : {-1, 0, 2}) {
        auto record = original;
        record.bar.startMs = open + i * 1000;
        record.bar.baseScale = 100; record.bar.sellVol = 100;
        record.bar.buyVol = i == 2 ? 50 : 400;
        record.bar.delta = record.bar.buyVol - record.bar.sellVol;
        source.bars[record.bar.startMs] = record;
    }
    auto frame = footprintFrame();
    frame.raster.drawnStartMs = open; frame.raster.drawnEndMs = open + 3000;
    chart::CvdLayer cvd;
    cvd.presentation().flowFooter = true;
    for (const auto reset : {chart::CvdReset::Visible, chart::CvdReset::UtcDay, chart::CvdReset::TpoSession}) {
        cvd.presentation().reset = reset;
        chart::CvdFeed::build(cvd.mailbox().back(), source, frame, reset, 0);
        cvd.mailbox().post(); cvd.sync(footprintChartFrame());
        const auto& s = cvd.current();
        ASSERT_EQ(s.series.count, 2);
        EXPECT_EQ(s.series.points[0].timeMs, open); EXPECT_EQ(s.series.points[1].timeMs, open + 2000);
        EXPECT_TRUE(s.totals[0].valid); EXPECT_TRUE(s.totals[1].valid);
        EXPECT_EQ(s.totals[0].delta, 3); EXPECT_EQ(s.totals[0].volume, 5);
        EXPECT_EQ(s.totals[1].delta, -.5); EXPECT_EQ(s.totals[1].volume, 1.5);
        const double anchor = reset == chart::CvdReset::Visible ? 0 : reset == chart::CvdReset::UtcDay ? 6 : 3;
        EXPECT_EQ(s.series.points[0].value, anchor); EXPECT_EQ(s.series.points[1].value, anchor - .5);
    }
}

TEST(FlowFooter, DefaultNodesAreIdenticalAndEnabledNodesAreLazyBeforeText) {
    chart::ChartLayers layers;
    layers.heatmap.initialize();
    RegistryScene scene(layers.registry);
    auto& root = *scene.foreground->firstChild();
    auto* textRoot = root.lastChild();
    auto frame = footprintChartFrame(); frame.context.surfaceBounds.setHeight(200);
    ChartTextRenderer text; ChartTextAtlas atlas;
    chart::LayerNodeContext context{nullptr, &root, text, atlas, false};
    layers.registry.sync(frame); layers.registry.updateNodes(frame, context);
    EXPECT_EQ(root.childCount(), 2); EXPECT_EQ(layers.flowFooter.createContainer(), nullptr);
    layers.cvd.presentation().flowFooter = true;
    chart::CvdFeed::build(layers.cvd.mailbox().back(), footprintSeries(), footprintFrame(), chart::CvdReset::Visible, 4);
    layers.cvd.mailbox().post(); layers.registry.sync(frame); layers.registry.updateNodes(frame, context);
    EXPECT_EQ(root.childCount(), 3); EXPECT_EQ(root.lastChild(), textRoot);
    EXPECT_TRUE(layers.flowFooter.drawing()); EXPECT_FALSE(layers.cvd.drawing());
    layers.cvd.presentation().stripPx = 60;
    layers.registry.sync(frame); layers.registry.updateNodes(frame, context);
    EXPECT_EQ(root.childCount(), 4);
    EXPECT_EQ(layers.flowFooter.bounds().bottom(), layers.cvd.strip().top());
    EXPECT_EQ(layers.flowFooter.bounds().top(), 85);
    EXPECT_EQ(layers.cvd.presentation().bottomInset(1000, 200), 115);
    layers.cvd.presentation().flowFooter = false;
    layers.registry.sync(frame); layers.registry.updateNodes(frame, context);
    EXPECT_FALSE(layers.flowFooter.drawing());
    EXPECT_EQ(static_cast<QSGOpacityNode*>(root.childAtIndex(2))->opacity(), 0);
}

TEST(FlowFooter, FreshDataPanAndLabelsAllocateNothingAfterWarmup) {
    QTemporaryDir cache;
    QString error;
    ASSERT_TRUE(AgentHostMode::activate(cache.path(), {}, &error)) << error.toStdString();
    struct Reset { ~Reset() { AgentHostMode::resetForTests(); } } reset;
    ChartTextAtlas atlas;
    ChartTextAtlas::BuildParams params;
    params.fontPath = QStringLiteral(SENTINEL_SOURCE_DIR "/resources/fonts/RobotoMono/RobotoMono-Regular.ttf");
    params.fontPx = 64; params.charset = " 0123456789.+-<>kMBDELTACUMVO";
    ASSERT_TRUE(atlas.build(params));
    chart::CvdLayer cvd;
    chart::FlowFooterLayer footer(cvd);
    cvd.presentation().flowFooter = true; cvd.presentation().stripPx = 60;
    auto frame = footprintChartFrame(); frame.context.surfaceBounds = {0, 0, 400, 220};
    auto source = footprintSeries();
    for (int i = 1; i < 4; ++i) { source.bars[i * 1000] = source.bars[0]; source.bars[i * 1000].bar.startMs = i * 1000; }
    auto feedFrame = footprintFrame(); feedFrame.raster.drawnEndMs = 4000;
    chart::CvdFeed::build(cvd.mailbox().back(), source, feedFrame, chart::CvdReset::Visible, 4);
    cvd.mailbox().post(); cvd.sync(frame); footer.sync(frame);
    std::unique_ptr<QSGNode> root(footer.createContainer());
    ChartTextRenderer text;
    chart::LayerNodeContext context{nullptr, root.get(), text, atlas, true};
    text.beginFrame(root.get(), nullptr, atlas);
    footer.updateNodes(frame, context); footer.submitLabels(frame, context);
    ASSERT_EQ(footer.columnCount(), 3); ASSERT_FALSE(footer.glyphs().empty());
    std::string rendered;
    for (const auto& glyph : footer.glyphs()) {
        for (const auto c : params.charset) {
            if (atlas.glyph(c).uv == glyph.uv) { rendered += c.toLatin1(); break; }
        }
    }
    EXPECT_EQ(rendered, "+3.00+3.0011.2+3.00+6.0011.2+3.00+9.0011.2DELTACUMVOL");
    for (const auto& glyph : footer.glyphs()) {
        EXPECT_GE(glyph.rect.left(), frame.context.surfaceBounds.left());
        EXPECT_LE(glyph.rect.right(), frame.context.surfaceBounds.right());
        EXPECT_GE(glyph.rect.top(), footer.bounds().top());
        EXPECT_LE(glyph.rect.bottom(), footer.bounds().bottom());
    }
    for (int i = 0; i < 4; ++i) {
        source.bars[1000].bar.delta += 10;
        chart::CvdFeed::build(cvd.mailbox().back(), source, feedFrame, chart::CvdReset::Visible, 4);
        cvd.mailbox().post(); frame.context.raster.leftF += .1;
        footprintAllocations = 0; footprintCounting = true;
        text.beginFrame(root.get(), nullptr, atlas);
        cvd.sync(frame); footer.sync(frame); footer.updateNodes(frame, context); footer.submitLabels(frame, context);
        footprintCounting = false;
        EXPECT_EQ(footprintAllocations.load(), 0u);
    }
    cvd.mailbox().back().series.clear(); cvd.mailbox().post();
    cvd.sync(frame); footer.sync(frame); footer.updateNodes(frame, context);
    EXPECT_FALSE(footer.drawing()); EXPECT_EQ(footer.columnCount(), 0); EXPECT_TRUE(footer.glyphs().empty());

    chart::FootprintCellsLayer cells;
    cells.presentation().enabled = true; cells.presentation().dustNotional = 0;
    ASSERT_TRUE(chart::FlowFeed::bin(cells.mailbox().back(), footprintSeries(), footprintFrame()));
    cells.mailbox().post(); cells.sync(frame);
    std::unique_ptr<QSGNode> cellsRoot(cells.createContainer());
    cells.updateNodes(frame, context); cells.submitLabels(frame, context);
    const auto fullCount = cells.glyphCount(); ASSERT_GT(fullCount, 0u);
    const double inset = cvd.presentation().stripPx + chart::FlowFooterLayout::height;
    cells.submitLabels(frame, context, inset);
    for (const auto& glyph : cells.glyphs()) EXPECT_LE(glyph.rect.bottom(), frame.context.surfaceBounds.bottom() - inset);
    cells.submitLabels(frame, context, frame.context.surfaceBounds.height());
    EXPECT_EQ(cells.glyphCount(), 0u);
    cells.submitLabels(frame, context);
    EXPECT_EQ(cells.glyphCount(), fullCount);
}

TEST(FlowFooter, FooterOnlyPublishesRetainedFlowWithTheSelectedCvdAnchor) {
    chart::CvdLayer cvd;
    chart::CvdFeed feed(cvd.mailbox(), cvd.presentation());
    auto frame = footprintFrame(); frame.raster.drawnStartMs = 1000;
    feed.setPorts({[&] { return frame; }, [] { return QStringLiteral("BTC-USD"); }, [] { return 4; }, [] {}});
    auto batch = std::make_shared<SentinelStreamClient::FlowBatch>();
    batch->symbol = "BTC-USD"; batch->timeframeMs = 1000;
    for (int i = 0; i < 2; ++i) {
        auto& bar = batch->bars.emplace_back().bar;
        bar.startMs = i * 1000; bar.timeframeMs = 1000; bar.baseScale = 100;
        bar.sellVol = 100; bar.buyVol = 300; bar.delta = 200;
    }
    feed.receive(batch);
    cvd.presentation().flowFooter = true;
    for (auto reset : {chart::CvdReset::Visible, chart::CvdReset::UtcDay, chart::CvdReset::TpoSession}) {
        cvd.presentation().reset = reset; feed.publish(); cvd.sync(footprintChartFrame());
        const auto& data = cvd.current();
        ASSERT_EQ(data.series.count, 1);
        EXPECT_EQ(data.series.points[0].value, reset == chart::CvdReset::Visible ? 0 : 4);
        EXPECT_EQ(data.totals[0].delta, 2); EXPECT_EQ(data.totals[0].volume, 4);
        EXPECT_FALSE(cvd.drawing());
    }
}

TEST(FlowFooter, PanAndMagnitudeChangesKeepTheSameAbsoluteColumnPhase) {
    QTemporaryDir cache;
    QString error;
    ASSERT_TRUE(AgentHostMode::activate(cache.path(), {}, &error));
    struct Reset { ~Reset() { AgentHostMode::resetForTests(); } } reset;
    ChartTextAtlas atlas;
    ChartTextAtlas::BuildParams params;
    params.fontPath = QStringLiteral(SENTINEL_SOURCE_DIR "/resources/fonts/RobotoMono/RobotoMono-Regular.ttf");
    params.fontPx = 64; params.charset = " 0123456789.+-<>kMBDELTACUMVO";
    ASSERT_TRUE(atlas.build(params));
    chart::CvdLayer cvd; chart::FlowFooterLayer footer(cvd);
    cvd.presentation().flowFooter = true;
    auto source = footprintSeries();
    for (int i = 0; i <= 12; ++i) {
        auto& bar = source.bars[i * 1000].bar;
        bar.startMs = i * 1000; bar.timeframeMs = 1000; bar.baseScale = 100;
        bar.sellVol = 0; bar.buyVol = bar.delta = i == 12 ? 1000000000000000LL : 1;
    }
    auto frame = footprintChartFrame(); frame.context.surfaceBounds = {0, 0, 600, 180};
    frame.context.raster.colPxF = 50; frame.context.raster.drawnEndMs = 12000;
    ChartTextRenderer text;
    chart::LayerNodeContext context{nullptr, nullptr, text, atlas, true};
    std::unique_ptr<QSGNode> root;
    std::vector<int64_t> previous;
    for (int pan : {0, 1, 0}) {
        frame.context.raster.leftF = pan * 50;
        frame.context.raster.drawnStartMs = pan * 1000;
        frame.context.raster.drawnEndMs = (pan + 12) * 1000;
        auto feedFrame = footprintFrame(); feedFrame.raster = frame.context.raster;
        chart::CvdFeed::build(cvd.mailbox().back(), source, feedFrame, chart::CvdReset::Visible, 4);
        cvd.mailbox().post(); cvd.sync(frame); footer.sync(frame);
        if (!root) root.reset(footer.createContainer());
        context.root = root.get(); footer.updateNodes(frame, context);
        EXPECT_EQ(footer.columnStride(), 2);
        std::vector<int64_t> columns;
        auto* geometry = static_cast<QSGGeometryNode*>(root->firstChild())->geometry();
        const auto* vertices = geometry->vertexDataAsColoredPoint2D();
        for (int i = 0; i < footer.columnCount(); ++i) {
            const auto& centre = vertices[6 + i * 36];
            columns.push_back(int64_t(std::llround((centre.x + pan * 50 - 25) / 50)));
            const auto positive = theme::positive();
            EXPECT_EQ(centre.r, positive.red() * centre.a / 255);
            EXPECT_EQ(centre.g, positive.green() * centre.a / 255);
            EXPECT_EQ(centre.b, positive.blue() * centre.a / 255);
        }
        ASSERT_FALSE(columns.empty());
        for (int64_t index : {4, 6, 8, 10}) EXPECT_EQ(std::count(columns.begin(), columns.end(), index), 1);
        for (const auto index : columns) EXPECT_EQ(index % 2, 0);
        if (pan == 0 && previous.empty()) previous = columns;
        else {
            for (int64_t index : {4, 6, 8, 10})
                EXPECT_EQ(std::count(previous.begin(), previous.end(), index), std::count(columns.begin(), columns.end(), index));
        }
    }
    for (auto& [time, record] : source.bars) { record.bar.delta = 1; record.bar.baseScale = 1000; }
    auto feedFrame = footprintFrame(); feedFrame.raster = frame.context.raster;
    chart::CvdFeed::build(cvd.mailbox().back(), source, feedFrame, chart::CvdReset::Visible, 4);
    cvd.mailbox().post(); cvd.sync(frame); footer.sync(frame); footer.updateNodes(frame, context);
    auto* geometry = static_cast<QSGGeometryNode*>(root->firstChild())->geometry();
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    EXPECT_EQ(geometry->vertexCount(), 6) << "subcent deltas must not draw pills";
#else
    const auto* vertices = geometry->vertexDataAsColoredPoint2D();
    for (int i = 6; i < geometry->vertexCount(); ++i) EXPECT_EQ(vertices[i].a, 0);
#endif
}

TEST(FlowFooter, TpoAndVpSubtreesRespectTheCombinedPaneWithoutAllocating) {
    chart::CvdPresentation presentation;
    chart::VolumeProfileLayer vp(&presentation); chart::TpoLayer tpo(&presentation);
    auto frame = footprintChartFrame();
    ChartTextRenderer text; ChartTextAtlas atlas;
    for (chart::PaneChartLayer* layer : {static_cast<chart::PaneChartLayer*>(&vp), static_cast<chart::PaneChartLayer*>(&tpo)}) {
        presentation.stripPx = 0; presentation.flowFooter = false;
        std::unique_ptr<QSGNode> root(layer->createContainer());
        auto* label = new QSGNode; root->appendChildNode(label);
        chart::LayerNodeContext context{nullptr, root.get(), text, atlas, false};
        layer->sync(frame); layer->updateNodes(frame, context);
        EXPECT_EQ(label->parent(), root.get());
        presentation.hasData = true; presentation.dataTimeframeMs = 1000;
        for (const auto strip : {0, 60}) {
            presentation.flowFooter = true; presentation.stripPx = strip;
            footprintAllocations = 0; footprintCounting = true;
            layer->sync(frame); layer->updateNodes(frame, context);
            footprintCounting = false;
            EXPECT_EQ(footprintAllocations.load(), 0u);
            ASSERT_EQ(root->firstChild()->type(), QSGNode::ClipNodeType);
            auto* clip = static_cast<QSGClipNode*>(root->firstChild());
            EXPECT_EQ(label->parent(), clip);
            EXPECT_EQ(clip->clipRect().bottom(), frame.context.surfaceBounds.bottom() - strip - chart::FlowFooterLayout::height);
        }
        presentation.flowFooter = false; presentation.stripPx = 0;
        layer->sync(frame); layer->updateNodes(frame, context);
        EXPECT_EQ(label->parent(), root.get());
    }
}

TEST(CvdLayer, OpaqueThemePanelDividerAndLineDefaultsReachGeometry) {
    chart::CvdLayer layer;
    layer.presentation().stripPx = 80;
    auto frame = footprintChartFrame();
    chart::CvdFeed::build(layer.mailbox().back(), footprintSeries(), footprintFrame(), chart::CvdReset::Visible, 4);
    layer.mailbox().post(); layer.sync(frame);
    std::unique_ptr<QSGNode> root(layer.createContainer());
    ChartTextRenderer text; ChartTextAtlas atlas;
    chart::LayerNodeContext context{nullptr, root.get(), text, atlas, false};
    layer.updateNodes(frame, context);
    const auto* vertices = static_cast<QSGGeometryNode*>(root->firstChild())->geometry()->vertexDataAsColoredPoint2D();
    for (int i = 0; i < 12; ++i) {
        const auto color = i < 6 ? theme::bg0() : theme::line();
        EXPECT_EQ(vertices[i].a, 255);
        EXPECT_EQ(vertices[i].r, color.red()); EXPECT_EQ(vertices[i].g, color.green()); EXPECT_EQ(vertices[i].b, color.blue());
    }
    EXPECT_FLOAT_EQ(vertices[7].y - vertices[6].y, 1.0f);
    EXPECT_FLOAT_EQ(vertices[19].y - vertices[18].y, 1.5f);
    EXPECT_DOUBLE_EQ(layer.style().fillAlpha, 1.0);
    EXPECT_EQ(layer.gutterValues().min, 0); EXPECT_EQ(layer.gutterValues().max, 0); EXPECT_EQ(layer.gutterValues().last, 0);
}

#include "render/ChartTextLayout.hpp"
namespace {
class CvdPriceTicks final : public AxisModel {
public:
    CvdPriceTicks() { m_ticks = {{1, 50, "1"}, {2, 120, "2"}, {3, 180, "3"}}; }
    void recalculateTicks() override {}
private:
    void calculateTicks() override {}
    QString formatLabel(double) const override { return {}; }
};
}
TEST(CvdLayer, PriceLabelRectsStopAboveStripAndReturnToFullHeightWhenHidden) {
    ChartTextAtlas atlas;
    ChartTextAtlas::BuildParams params;
    params.fontPath = QStringLiteral(SENTINEL_SOURCE_DIR "/resources/fonts/RobotoMono/RobotoMono-Regular.ttf");
    params.fontPx = 48; params.charset = " $0123456789.";
    ASSERT_TRUE(atlas.build(params));
    CvdPriceTicks ticks;
    AxisTextService axes(atlas); axes.setPriceAxisSource(&ticks);
    chart::CvdLayer layer; layer.bindAxes(&axes);
    auto frame = footprintChartFrame();
    frame.context.surfaceBounds = frame.context.itemBounds = QRectF(0, 0, 400, 200);
    ChartTextRenderer text;
    chart::LayerNodeContext context{nullptr, nullptr, text, atlas, false};
    chart::CvdFeed::build(layer.mailbox().back(), footprintSeries(), footprintFrame(), chart::CvdReset::Visible, 4);
    layer.mailbox().post(); layer.presentation().stripPx = 80; layer.sync(frame);
    std::unique_ptr<QSGNode> root(layer.createContainer()); context.root = root.get();
    for (int stripHeight : {80, 0, 80, 0}) {
        layer.presentation().stripPx = stripHeight; layer.sync(frame); layer.updateNodes(frame, context);
        const auto safe = AxisLayout::priceLabelRect(400, 200, 0, axes.renderPriceAxisWidth(), axes.pricePaneTop());
        EXPECT_EQ(safe.bottom(), 200 - stripHeight);
        text.beginFrame(root.get(), nullptr, atlas);
        axes.submitAxisText(text, atlas, 400, 200);
        EXPECT_EQ(text.submittedGlyphs(), stripHeight ? 1 : 3);
        text.endFrame();
        int belowStrip = 0, accepted = 0;
        for (int y = 0; y < 200; ++y) {
            ChartTextRun run; run.text = "$12.3"; run.anchor = {safe.left() + 1, double(y)};
            run.hAlign = ChartTextRun::HorizontalAlign::Left;
            run.scale = 13.0f / atlas.fontPx(); run.useStableMetrics = true; run.pixelSnap = true;
            QRectF rect;
            if (!AxisLayout::runFitsRect(atlas, run, safe, &rect)) continue;
            ++accepted;
            if (stripHeight) EXPECT_FALSE(rect.intersects(layer.gutter()));
            if (rect.top() > 120) ++belowStrip;
        }
        EXPECT_GT(accepted, 0);
        if (stripHeight) EXPECT_EQ(belowStrip, 0); else EXPECT_GT(belowStrip, 0);
    }
}

TEST(CvdLayer, LastLabelTracksCurveEndpointAndClampsClearOfExtrema) {
    QTemporaryDir cache;
    QString error;
    ASSERT_TRUE(AgentHostMode::activate(cache.path(), {}, &error));
    struct Reset { ~Reset() { AgentHostMode::resetForTests(); } } reset;
    ChartTextAtlas atlas;
    ChartTextAtlas::BuildParams params;
    params.fontPath = QStringLiteral(SENTINEL_SOURCE_DIR "/resources/fonts/RobotoMono/RobotoMono-Regular.ttf");
    params.fontPx = 64; params.charset = " 0123456789.-CVD";
    ASSERT_TRUE(atlas.build(params));
    chart::CvdLayer layer;
    layer.presentation().stripPx = 100;
    auto frame = footprintChartFrame();
    frame.context.surfaceBounds.setWidth(400); frame.context.itemBounds = frame.context.surfaceBounds;
    frame.context.raster.drawnEndMs = 3000;
    layer.sync(frame);
    std::unique_ptr<QSGNode> root(layer.createContainer());
    ChartTextRenderer text;
    chart::LayerNodeContext context{nullptr, root.get(), text, atlas, true};
    for (int height : {39, 70, 100, 160}) {
        for (double fraction : {0., .2, .4, .6, .8, 1.}) {
            SCOPED_TRACE(height);
            SCOPED_TRACE(fraction);
            auto& snapshot = layer.mailbox().back();
            snapshot.series.clear(); snapshot.timeframeMs = 1000; snapshot.reset = chart::CvdReset::Visible;
            std::strcpy(snapshot.label.data(), "CVD");
            snapshot.series.push(0, -10, 0, true);
            snapshot.series.push(1000, 20, 0, true);
            const double value = fraction * 20 - 10;
            snapshot.series.push(2000, value - 10, 0, true);
            layer.mailbox().post(); layer.presentation().stripPx = height;
            layer.sync(frame); layer.updateNodes(frame, context);
            text.beginFrame(root.get(), nullptr, atlas); layer.submitLabels(frame, context);
            char formatted[24]; chart::CvdGutterValues::format(value, formatted);
            const auto glyphs = layer.labelGlyphs();
            const size_t count = std::strlen(formatted);
            ASSERT_GE(glyphs.size(), count);
            QRectF last;
            for (const auto& glyph : glyphs.last(count)) last = last.united(glyph.rect);
            const auto* vertices = static_cast<QSGGeometryNode*>(root->firstChild())->geometry()->vertexDataAsColoredPoint2D();
            // The final ribbon's second triangle starts at its endpoint (before stroke thickness).
            const double endpoint = vertices[layer.vertexCount() - 3].y;
            const double separation = height >= 59 ? 19 : 0;
            const double low = layer.strip().top() + 2 + separation + 8.5;
            const double high = layer.strip().bottom() - 2 - separation - 8.5;
            EXPECT_NEAR(last.center().y(), std::clamp(endpoint, low, high), 1e-4);
            for (const auto& glyph : glyphs.first(glyphs.size() - count))
                EXPECT_FALSE(last.intersects(glyph.rect));
            EXPECT_TRUE(layer.gutter().contains(last));
            text.endFrame();
        }
    }
}
