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
#include "chart/shell/ChartLookSettings.hpp"
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
    layer.presentation().enabled = true; layer.presentation().cellsSource = true;
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
    const double bidLength = v[14].x - v[12].x, askLength = v[20].x - v[18].x;
    EXPECT_NEAR(bidLength / askLength, 240.0 / 440, 1e-6);
    EXPECT_FLOAT_EQ(v[14].x, 54.5); EXPECT_FLOAT_EQ(v[18].x, 55.5);
    EXPECT_DOUBLE_EQ(layer.cards().size(), 2);
    layer.presentation().windowIntensity = true;
    layer.sync(frame); layer.updateNodes(frame, context);
    v = geometry->vertexDataAsColoredPoint2D();
    EXPECT_NEAR(v[14].x - v[12].x, bidLength / 2, 1e-5);
    EXPECT_NEAR(v[20].x - v[18].x, askLength / 2, 1e-5);
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
    auto frame = footprintFrame(); frame.raster.drawnEndMs = 3000000;
    for (int i = -1000; i < 4000; ++i) {
        series.bars[i * 1000] = series.bars[0]; series.bars[i * 1000].bar.startMs = i * 1000;
    }
    ASSERT_TRUE(chart::FlowFeed::bin(*out, series, frame));
    EXPECT_TRUE(out->valid); EXPECT_EQ(out->barCount, chart::FlowSnapshot::kMaxBars);
    EXPECT_GT(out->truncatedBars, 0);
    EXPECT_EQ(out->bars[0].startMs, 950000) << "the far past is trimmed";
    EXPECT_EQ(out->bars[size_t(out->barCount - 1)].startMs, 2999000) << "never the newest edge";
    // A view that fits the cap shrinks its overscan instead of truncating.
    frame.raster.drawnEndMs = 1500000;
    ASSERT_TRUE(chart::FlowFeed::bin(*out, series, frame));
    EXPECT_EQ(out->truncatedBars, 0);
    EXPECT_EQ(out->fromMs, -274000); EXPECT_EQ(out->toMs, 1774000);
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
    return frame;
}
int visibleBars(const chart::FlowSnapshot& out, const MappingFrameContext& frame) {
    int count = 0;
    for (int i = 0; i < out.barCount; ++i)
        count += double(out.bars[size_t(i)].startMs) + 1000 > frame.raster.drawnStartMs &&
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
TEST(FootprintBands, StripDrawsOneTintedCellPerBandWithoutCardOrSpine) {
    chart::FootprintCellsLayer layer;
    layer.presentation().enabled = true; layer.presentation().cellsSource = true;
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
    const auto bid = chart::ChartLookSettings::footprintBid(), ask = chart::ChartLookSettings::footprintAsk();
    EXPECT_EQ(v[0].a, 255 * bid.alpha() / 255) << "the bid-dominant band is the bar's busiest";
    EXPECT_EQ(v[0].g, bid.green() * v[0].a / 255);
    EXPECT_EQ(v[6].g, ask.green() * v[6].a / 255);
    EXPECT_LT(v[12].a, v[6].a) << "intensity follows band volume";
    layer.submitLabels(frame, context);
    EXPECT_EQ(layer.glyphCount(), 0u);
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
    ASSERT_TRUE(layer.drawing()); ASSERT_EQ(layer.vertexCount(), 12);
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
    EXPECT_EQ(value.footprintSource, "q16");
    EXPECT_TRUE(heatmap::applySettingsPatch(value, {{"footprintSource", "cells"}, {"footprintIntensity", "window"},
        {"footprintCandle", "full"}, {"footprintNumbers", false}}).isEmpty());
    store.save("test", value);
    auto loaded = store.load("test", ClientHeatmapConfig{});
    EXPECT_EQ(loaded.footprintSource, "cells");
    EXPECT_EQ(loaded.footprintIntensity, "window");
    EXPECT_EQ(loaded.footprintCandle, "full"); EXPECT_FALSE(loaded.footprintNumbers);
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
    EXPECT_TRUE(visibility.setProperty("footprintCandle", "full"));
    EXPECT_EQ(visibility.footprintIntensity(), "window");
    EXPECT_FALSE(visibility.footprintNumbers()); EXPECT_EQ(visibility.footprintCandle(), "full");
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
    params.fontPx = 64; params.charset = " 0123456789.+-,:/<>$%kMBABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    ASSERT_TRUE(atlas.build(params));
    const double widest = chart::kFlowMagnitudeGlyphs * atlas.glyph(QChar('8')).advance * chart::kFootprintTextPx / atlas.fontPx();
    EXPECT_LE(widest, (chart::kFootprintNumbersFitColumn - chart::kFootprintCandleSpace) / 2 - 3);
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
    for (const auto& glyph : layer.glyphs()) EXPECT_NEAR(glyph.color.alphaF(), .22, .005);
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
    EXPECT_FALSE(look.spine(frame.context.raster));
    look.cellsSource = true;
    layers.footprintCells.sync(frame);
    EXPECT_FALSE(layers.footprint.selected(frame));
    EXPECT_TRUE(layers.footprintCells.drawing());
    EXPECT_TRUE(look.spineBar(frame.context.raster, 0));
    EXPECT_FALSE(look.spineBar(frame.context.raster, 1000));
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
    EXPECT_FALSE(layers.cvd.presentation().drawable(1000));
    chart::CvdFeed::build(layers.cvd.mailbox().back(), footprintSeries(), footprintFrame(), chart::CvdReset::Visible, 4);
    layers.cvd.mailbox().post();
    EXPECT_TRUE(layers.cvd.presentation().drawable(1000));
    EXPECT_FALSE(layers.cvd.presentation().drawable(60000));
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
    auto labelledSeries = footprintSeries();
    labelledSeries.bars[1000] = labelledSeries.bars[0];
    labelledSeries.bars[1000].bar.startMs = 1000;
    chart::CvdFeed::build(layer.mailbox().back(), labelledSeries, footprintFrame(), chart::CvdReset::Visible, 4);
    layer.mailbox().post(); layer.sync(frame);
    std::unique_ptr<QSGNode> root(layer.createContainer());
    ChartTextRenderer text;
    chart::LayerNodeContext context{nullptr, root.get(), text, atlas, true};
    layer.updateNodes(frame, context); layer.submitLabels(frame, context);
    ASSERT_EQ(text.submittedGlyphs(), int(std::strlen(layer.current().label.data())) + 5)
        << "3.00 maximum and exact zero minimum must both have glyphs";
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
    QSGNode root;
    layers.registry.onRootRebuilt(&root);
    auto* textRoot = root.lastChild();
    auto frame = footprintChartFrame(); frame.context.surfaceBounds.setHeight(200);
    ChartTextRenderer text; ChartTextAtlas atlas;
    chart::LayerNodeContext context{nullptr, &root, text, atlas, false};
    layers.registry.sync(frame); layers.registry.updateNodes(frame, context);
    EXPECT_EQ(root.childCount(), 7); EXPECT_EQ(layers.flowFooter.createContainer(), nullptr);
    layers.cvd.presentation().flowFooter = true;
    chart::CvdFeed::build(layers.cvd.mailbox().back(), footprintSeries(), footprintFrame(), chart::CvdReset::Visible, 4);
    layers.cvd.mailbox().post(); layers.registry.sync(frame); layers.registry.updateNodes(frame, context);
    EXPECT_EQ(root.childCount(), 8); EXPECT_EQ(root.lastChild(), textRoot);
    EXPECT_TRUE(layers.flowFooter.drawing()); EXPECT_FALSE(layers.cvd.drawing());
    layers.cvd.presentation().stripPx = 60;
    layers.registry.sync(frame); layers.registry.updateNodes(frame, context);
    EXPECT_EQ(root.childCount(), 9);
    EXPECT_EQ(layers.flowFooter.bounds().bottom(), layers.cvd.strip().top());
    EXPECT_EQ(layers.flowFooter.bounds().top(), 85);
    EXPECT_EQ(layers.cvd.presentation().bottomInset(1000, 200), 115);
    layers.cvd.presentation().flowFooter = false;
    layers.registry.sync(frame); layers.registry.updateNodes(frame, context);
    EXPECT_FALSE(layers.flowFooter.drawing());
    EXPECT_EQ(static_cast<QSGOpacityNode*>(root.childAtIndex(7))->opacity(), 0);
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
    cells.presentation().enabled = true; cells.presentation().cellsSource = true; cells.presentation().dustNotional = 0;
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
