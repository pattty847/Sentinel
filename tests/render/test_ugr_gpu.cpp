// Slice S6b: the main chart (UnifiedGridRenderer) on the GPU heatmap path, over
// a synthetic HMC2 recording through the shared HeatmapDataService (LabData's
// local configurator; the real recording is never touched). GPU cases use the
// selected QRhi backend (lab::OffscreenQuick) and skip with the reason when it
// cannot create one: a skipped case is no result.
#include "UnifiedGridRenderer.h"
#include "CoordinateSystem.h"
#include "datasources/CandleSeriesBuffer.hpp"
#include "lab/LabData.hpp"
#include "lab/OffscreenQuick.hpp"
#include "lab/RhiBackend.hpp"
#include "mainwindow/AgentApiInput.hpp"
#include "mainwindow/QmlSceneController.h"
#include "models/PriceAxisModel.hpp"
#include "models/TimeAxisModel.hpp"
#include "render/AlgoOverlayRenderer.hpp"
#include "render/CandlestickBatched.hpp"
#include "render/CandlestickOverlayItem.hpp"
#include "render/DataProcessor.hpp"
#include "render/HeatmapIntensityNode.hpp"
#include "render/HeatmapOverlayRenderer.hpp"
#include "render/LabTextItem.hpp"
#include "render/PaperTradeOverlayModel.hpp"
#include "render/PaperTradeOverlayRenderer.hpp"
#include "render/heatmap/HeatmapGpuLayer.hpp"
#include "render/heatmap/HeatmapPalette.hpp"
#include "servermodel/Hmc2Store.hpp"
#include "heatmap/LocalChunkTransport.hpp"
#include "../servermodel/FakeChunkTransport.hpp"
#include "SyntheticHmc2Fixture.hpp"
#include "marketdata/model/TradeData.h"
#include <QElapsedTimer>
#include <QEventLoop>
#include <QGuiApplication>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickView>
#include <QQuickWindow>
#include <QSGOpacityNode>
#include <QTemporaryDir>
#include <QTimer>
#include <QtQml/qqml.h>
#include <gtest/gtest.h>
#include <private/qquickitem_p.h>
#include <atomic>
#include <climits>
#include <cmath>
#include <functional>
#include <iostream>
#include <optional>
#include <stdexcept>

namespace {
using namespace synthetic_hmc2;
QSGNode *paintRoot(QQuickItem *item) { return QQuickItemPrivate::get(item)->paintNode; }

// One synthetic recording (4 h) for the whole binary: the data path is configured
// once in main() and never reconfigured (a reconfigure with local chunk reads in
// flight is lab-only teardown territory, not the main chart's).
QTemporaryDir *fixtureDir = nullptr;
// A second recorded symbol at ~$0.00001 (price scale 1e10): a different price scale
// for the symbol-switch carry (its rows are 1e-8 near and 1e-7 deep).
recording::Hmc2Record tinyMinute(const std::string &layer, int64_t i) {
    auto r = syntheticMinute(layer, i);
    r.header.symbol = "TINY-USD";
    r.header.priceScale = 1e10;
    r.midOpen = r.midClose = r.midMin = r.midMax = r.midOpen * 100 / 1e10;
    return r;
}
void configureFixture() {
    fixtureDir = new QTemporaryDir;
    writeRecording(*fixtureDir, 4 * 60);
    lab::LabData::setLocalSymbolsForTest({"BTC-USD", "TINY-USD"});
    {
        recording::Hmc2Store writer(fixtureDir->path().toStdString());
        for (int64_t i = 0; i < 4 * 60; ++i)
            for (const char *layer : {"deep", "near"}) writer.append(tinyMinute(layer, i));
    }
    lab::LabData::configure(fixtureDir->path().toStdString(), epoch + 4 * kHourMs);
}

heatmap::HeatmapChartSettings brightSettings() {
    heatmap::HeatmapChartSettings s;
    s.palettePreset = "Fire";
    s.sensitivityMin = 0.001; // the fixture's sizes (0.01..0.53) span the palette
    s.sensitivityMax = 1;
    return s;
}

class UgrGpu : public testing::Test {
protected:
    std::unique_ptr<lab::OffscreenQuick> scene;
    UnifiedGridRenderer *ugr = nullptr;
    QString error;
    // 4 h of history; views well inside it.
    const int64_t viewLo = epoch + 2 * kHourMs, viewHi = epoch + 2 * kHourMs + 40 * minute;
    void SetUp() override {
        if (const QString why = lab::gpuUnavailableReason(); !why.isEmpty()) GTEST_SKIP() << "GPU case skipped: " << why.toStdString();
        scene = std::make_unique<lab::OffscreenQuick>();
        ASSERT_TRUE(scene->create(QSize(640, 320), &error)) << error.toStdString();
        scene->window()->setColor(Qt::black);
        ugr = new UnifiedGridRenderer(scene->window()->contentItem());
        ugr->setSize(QSizeF(640, 320));
        ugr->setHeatmapService(&lab::LabData::instance().service());
        ugr->setActiveSymbol("BTC-USD");
        ugr->setTimeframe(int(minute));
        ugr->setHeatmapChartSettings(brightSettings());
    }
    void TearDown() override {
        delete ugr;
        ugr = nullptr;
        if (scene) scene->renderFrame(&error);
        scene.reset();
    }
    heatmap::gpu::HeatmapGpuLayer &layer() const { return *ugr->gpuHeatmapLayer(); }
    void gpuOn() {
        ugr->setHeatmapRenderer("gpu");
        ugr->setViewport(viewLo, viewHi, 99'900, 100'300);
    }
    // Renders frames (offscreen rendering emits no frameSwapped: ask for each)
    // until done() holds or `ms` pass; onFrame runs after every frame.
    template <class Done, class OnFrame>
    bool pump(int ms, Done done, OnFrame onFrame) {
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < ms) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            ugr->update();
            image = scene->renderFrame(&error);
            if (image.isNull()) return false;
            onFrame();
            if (done()) return true;
        }
        return false;
    }
    template <class Done>
    bool pump(int ms, Done done) { return pump(ms, done, [] {}); }
    // Frames only when the scene asks for one (a real render loop): no update()
    // from the test, so the chart's own scheduling must keep work moving.
    template <class Done>
    bool pumpOnRequest(int ms, Done done, int *rendered = nullptr) {
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < ms) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            if (scene->frameRequested()) {
                image = scene->renderFrame(&error);
                if (image.isNull()) return false;
                if (rendered) ++*rendered;
            }
            if (done()) return true;
        }
        return false;
    }
    double timeSpan() const {
        return double(ugr->getViewState()->getVisibleTimeEnd() - ugr->getViewState()->getVisibleTimeStart());
    }
    double priceSpan() const { return ugr->getViewState()->getMaxPrice() - ugr->getViewState()->getMinPrice(); }
    void recordingMode() {
        ClientConfig config;
        config.heatmap.source = "recording";
        config.heatmap.initialPricePct = 5;
        ugr->applyClientConfig(config);
    }
    bool frames(int n) {
        int left = n;
        pump(10'000, [&] { return --left <= 0; });
        return left <= 0 && !image.isNull();
    }
    bool settle() {
        int streak = 0;
        const bool ok = pump(30'000, [&] {
            const auto &st = layer().tileStats();
            streak = layer().settled() && !st.crossfading.load() && !st.holding.load() ? streak + 1 : 0;
            return streak >= 3;
        });
        if (!ok && error.isEmpty()) error = QStringLiteral("did not settle");
        return ok;
    }
    // Fraction of the view's width the current picture draws from content (span
    // or live bins, not loading) in the last frame.
    double coverage() const {
        const auto mapping = ugr->currentTimeAxisMapping();
        return coverage(mapping.viewStartMs, mapping.viewEndMs);
    }
    // Drawn fraction of [lo, hi): the view, or the range a view had before it changed.
    double coverage(double lo, double hi) const {
        double drawn = 0;
        for (const auto &s : layer().tileStats().segments())
            if (s.layer == 0 && s.kind != heatmap::gpu::HeatmapTileStats::Segment::Loading)
                drawn += std::max(0.0, std::min(hi, double(s.hiMs)) - std::max(lo, double(s.loMs)));
        return hi > lo ? drawn / (hi - lo) : 0;
    }
    QImage image;
};

TEST_F(UgrGpu, SettlesOnTheProductionPath) {
    gpuOn();
    ASSERT_TRUE(settle()) << error.toStdString();
    const auto &s = layer().tileStats();
    EXPECT_EQ(s.errors.load(), 0u);
    EXPECT_EQ(s.missingDraws.load(), 0u);
    EXPECT_GT(s.residentSources.load(), 0u);
    EXPECT_GT(layer().tickUnits(), 0);
    EXPECT_DOUBLE_EQ(ugr->heatmapTickSize(), layer().tickPrice()) << "heatmapTickSize reports the drawn tick";
    EXPECT_GT(coverage(), 0.99);
    // The paint root is the gpu one: a plain node whose first child gates the tile node.
    QSGNode *root = paintRoot(ugr);
    ASSERT_TRUE(root);
    EXPECT_EQ(root->type(), QSGNode::BasicNodeType);
    ASSERT_TRUE(root->firstChild());
    EXPECT_EQ(root->firstChild()->type(), QSGNode::OpacityNodeType);
    EXPECT_EQ(static_cast<QSGOpacityNode *>(root->firstChild())->opacity(), 1.0);
    EXPECT_EQ(root->firstChild()->firstChild()->type(), QSGNode::RenderNodeType);
    // The limits clamp the view: one column per pixel at 1m over 640 px.
    EXPECT_EQ(ugr->getViewState()->maxTimeSpanMs(), 640.0 * minute);
}

// Plan section 2 "Mapping": candles map through the viewport-only TimeAxisMapping;
// the heatmap's column edges are where the mapping puts the timeframe boundaries,
// and a candle's body is centred in its heatmap column (pixel column check).
TEST_F(UgrGpu, CandlesAlignWithHeatmapColumns) {
    ugr->setHeatmapRenderer("gpu");
    // 40 columns over 640 px, starting a third of a column in (not minute-aligned).
    const int64_t lo = viewLo + minute / 3;
    ugr->setViewport(lo, lo + 40 * minute, 99'900, 100'300);
    ASSERT_TRUE(settle()) << error.toStdString();
    const QImage heat = image;
    const auto mapping = ugr->currentTimeAxisMapping();
    ASSERT_TRUE(mapping.valid);
    // Expected column edges (pixel index where the next column starts).
    std::vector<int> edges;
    for (int64_t t = (lo / minute + 1) * minute; t < lo + 40 * minute; t += minute)
        edges.push_back(int(std::ceil(mapping.timeToScreenX(double(t)) - 0.5)));
    // Measured edges: x where the colour changes from x-1 in most rows.
    auto edgeStrength = [&](int x) {
        int changes = 0;
        for (int y = 5; y < 315; y += 2) changes += heat.pixel(x - 1, y) != heat.pixel(x, y);
        return changes;
    };
    int matched = 0;
    for (const int e : edges) {
        if (e < 2 || e > 637) continue;
        const int best = std::max({edgeStrength(e - 1), edgeStrength(e), edgeStrength(e + 1)});
        EXPECT_GE(best, 40) << "a heatmap column edge at x=" << e;
        matched += best >= 40;
    }
    EXPECT_GE(matched, 35);
    for (int x = 2; x < 638; ++x) {
        if (edgeStrength(x) < 80) continue;
        const bool near = std::any_of(edges.begin(), edges.end(), [&](int e) { return std::abs(e - x) <= 1; });
        EXPECT_TRUE(near) << "a strong vertical edge at x=" << x << " where no column boundary maps";
    }
    // Candles over the chart: full-height green bodies, one per minute.
    CandleSeriesBuffer buffer;
    std::vector<CandleSeriesBuffer::CandleBar> bars;
    for (int64_t t = (lo / minute) * minute; t < lo + 41 * minute; t += minute)
        bars.push_back({t, t + minute, 99'800, 100'400, 99'800, 100'400, 1.0, true, 0, false});
    buffer.applyHistory("BTC-USD", 60, bars);
    auto *candles = new CandlestickOverlayItem(scene->window()->contentItem());
    candles->setSize(ugr->size());
    candles->setMappingProvider(ugr);
    candles->setCandleBuffer(&buffer);
    candles->setSymbol("BTC-USD");
    candles->setTimeframeSec(60);
    ASSERT_TRUE(frames(5)) << error.toStdString();
    const QImage withCandles = image;
    int checked = 0;
    for (size_t i = 0; i + 1 < edges.size(); ++i) {
        const int a = edges[i], b = edges[i + 1];
        if (a < 2 || b > 637) continue;
        // The body: pixels at mid height that differ from the heatmap-only frame.
        int first = -1, last = -1;
        for (int x = a; x < b; ++x)
            if (withCandles.pixel(x, 160) != heat.pixel(x, 160) && withCandles.pixel(x, 120) != heat.pixel(x, 120)) {
                if (first < 0) first = x;
                last = x;
            }
        ASSERT_GE(first, 0) << "a candle body in the column [" << a << ", " << b << ")";
        EXPECT_NEAR((first + last) * 0.5, (a + b - 1) * 0.5, 1.0) << "the body is centred in its heatmap column";
        ++checked;
    }
    EXPECT_GE(checked, 35);
    delete candles;
}

// Spec rules 6-8 and plan section 2: a timeframe switch keeps the old picture
// until the new one is ready (the controller serial switches; nothing clears the
// heatmap), so the drawn coverage never shrinks.
TEST_F(UgrGpu, TimeframeSwitchNeverShrinksCoverage) {
    gpuOn();
    ASSERT_TRUE(settle()) << error.toStdString();
    const double before = coverage();
    ASSERT_GT(before, 0.99);
    // Pixels the heatmap colours (not the black background): the image itself
    // never blanks, whatever the node's stats say.
    auto painted = [&] {
        int n = 0;
        for (int y = 2; y < 320; y += 4)
            for (int x = 2; x < 640; x += 4) n += image.pixel(x, y) != qRgb(0, 0, 0);
        return n;
    };
    const int paintedBefore = painted();
    ASSERT_GT(paintedBefore, 1000);
    double worst = 1;
    int frames = 0, fewestPainted = INT_MAX;
    // The switch widens the view five-fold about its end (TimeframeSwitchKeepsTheColumnsOnScreen),
    // so the range that was on screen is what must never lose coverage; the newly revealed
    // range draws the loading hatch (not black) until the 5m spans land.
    const double lo0 = viewLo, hi0 = viewHi;
    ugr->setTimeframe(int(5 * minute));
    ASSERT_TRUE(pump(30'000, [&] { return layer().settled(); }, [&] {
        ++frames;
        worst = std::min(worst, coverage(lo0, hi0));
        fewestPainted = std::min(fewestPainted, painted());
    })) << error.toStdString();
    EXPECT_GE(worst, before - 1e-9) << "a frame of the switch drew less of the view";
    EXPECT_GE(fewestPainted, paintedBefore / 2) << "a frame of the switch blanked the chart";
    EXPECT_EQ(layer().tileStats().drawnTfMs.load(), 5 * minute);
    // 40 of the 200 minutes lie before the recording: nothing there, everything else drawn.
    EXPECT_GT(coverage(), 0.79);
    // And back to 1m (recent-tf): the view returns to the original 40 minutes.
    worst = 1;
    ugr->setTimeframe(int(minute));
    ASSERT_TRUE(pump(30'000, [&] { return layer().settled(); }, [&] { worst = std::min(worst, coverage(lo0, hi0)); }))
        << error.toStdString();
    EXPECT_GE(worst, before - 1e-9);
    std::cout << "[ugr] tf switch frames=" << frames << std::endl;
}

// Spec rule 1: a longer timeframe is how to see further back. A timeframe switch keeps
// the columns on screen (the span scales by the timeframe ratio about the view end),
// instead of keeping the time span (S6d A/B: four 1h columns, then 1,600 1m columns).
TEST_F(UgrGpu, TimeframeSwitchKeepsTheColumnsOnScreen) {
    gpuOn(); // 40 one-minute columns ending at viewHi, auto-scroll off
    ASSERT_TRUE(settle()) << error.toStdString();
    const auto *view = ugr->getViewState();
    // A switch is one viewport change (INV-003): the new limits never re-clamp the old
    // view as a separate step before the scaled view lands (S6d review major 2).
    auto switchTf = [&](int64_t tfMs) {
        const uint64_t before = view->getViewportVersion();
        ugr->setTimeframe(int(tfMs));
        return view->getViewportVersion() - before;
    };
    EXPECT_EQ(switchTf(5 * minute), 1u) << "1m -> 5m";
    EXPECT_EQ(view->getVisibleTimeEnd(), viewHi) << "the view end is the anchor";
    EXPECT_EQ(view->getVisibleTimeEnd() - view->getVisibleTimeStart(), 40 * 5 * minute) << "40 columns of 5m";
    EXPECT_EQ(switchTf(minute), 1u) << "5m -> 1m";
    EXPECT_EQ(view->getVisibleTimeEnd(), viewHi);
    EXPECT_EQ(view->getVisibleTimeEnd() - view->getVisibleTimeStart(), 40 * minute) << "40 columns of 1m again";
    // The clamp still rules: 640 px cannot show more than 640 columns.
    ugr->setViewport(viewHi - 600 * minute, viewHi, 99'900, 100'300);
    EXPECT_EQ(switchTf(5 * minute), 1u);
    EXPECT_LE(view->getVisibleTimeEnd() - view->getVisibleTimeStart(), 640 * 5 * minute);
    EXPECT_EQ(view->getVisibleTimeEnd(), viewHi);
    // Shorter timeframe from a view wider than its 1 column/px limit: the old span is read
    // before the new limit re-clamps it (S6d run 2: 72 h of 1h became 30.8 min of 1m), and
    // the limit is not published on its own first (it would clamp 72 h to 640 min).
    ugr->setTimeframe(int(60 * minute));
    ugr->setViewport(viewHi - 72 * kHourMs, viewHi, 99'900, 100'300);
    ASSERT_EQ(view->getVisibleTimeEnd() - view->getVisibleTimeStart(), 72 * kHourMs);
    EXPECT_EQ(switchTf(minute), 1u) << "72 h at 1h -> 1m";
    EXPECT_EQ(view->getVisibleTimeEnd() - view->getVisibleTimeStart(), 72 * minute) << "72 columns of 1m";
    EXPECT_EQ(view->getVisibleTimeEnd(), viewHi);
    ASSERT_TRUE(pump(30'000, [&] { return layer().settled(); }, [] {})) << error.toStdString();
}

// Follow-live: the switch lands on the live edge with the new span, still in one
// viewport change (no separate re-anchor after the scaled view).
TEST_F(UgrGpu, TimeframeSwitchUnderFollowLiveIsOneViewportChange) {
    gpuOn();
    ASSERT_TRUE(settle()) << error.toStdString();
    ugr->enableAutoScroll(true);
    const auto *view = ugr->getViewState();
    const int64_t anchor = layer().liveAnchorMs();
    ASSERT_GT(anchor, 0);
    const uint64_t before = view->getViewportVersion();
    ugr->setTimeframe(int(5 * minute));
    EXPECT_EQ(view->getViewportVersion() - before, 1u);
    const int64_t span = view->getVisibleTimeEnd() - view->getVisibleTimeStart();
    EXPECT_EQ(span, 40 * 5 * minute);
    const int64_t tf = 5 * minute;
    const int64_t liveEnd = (anchor + tf - 1) / tf * tf;
    EXPECT_EQ(view->getVisibleTimeEnd(), liveEnd + std::max<int64_t>(tf, int64_t(double(span) * 0.08)))
        << "the padding follows the new span";
    ASSERT_TRUE(pump(30'000, [&] { return layer().settled(); }, [] {})) << error.toStdString();
}

// Legacy mode is not part of the gpu switch: setTimeframe leaves its viewport alone
// (its stream service resets the span later, from the new timeframe's data).
TEST_F(UgrGpu, LegacyTimeframeSwitchLeavesTheViewport) {
    ugr->setViewport(viewLo, viewHi, 99'900, 100'300);
    const auto *view = ugr->getViewState();
    const uint64_t before = view->getViewportVersion();
    ugr->setTimeframe(int(5 * minute));
    EXPECT_EQ(view->getViewportVersion(), before);
    EXPECT_EQ(view->getVisibleTimeStart(), viewLo);
    EXPECT_EQ(view->getVisibleTimeEnd(), viewHi);
    EXPECT_EQ(view->maxTimeSpanMs(), 0.0) << "no gpu limits in legacy mode";
}

// FM-104 on the main chart: the chart moves to a new scene (a new QRhi); the node
// reports the loss, the controller rebuilds the released images, it settles again.
TEST_F(UgrGpu, ComesBackAfterTheQRhiIsRecreated) {
    gpuOn();
    ASSERT_TRUE(settle()) << error.toStdString();
    const uint64_t uploads = layer().tileStats().sourcesUploaded.load();
    ugr->setParentItem(nullptr);
    ugr->setParent(nullptr); // the old content item must not delete it with its window
    ASSERT_FALSE(scene->renderFrame(&error).isNull());
    scene = std::make_unique<lab::OffscreenQuick>();
    ASSERT_TRUE(scene->create(QSize(640, 320), &error)) << error.toStdString();
    ugr->setParentItem(scene->window()->contentItem());
    ASSERT_TRUE(settle()) << error.toStdString();
    EXPECT_GT(layer().tileStats().sourcesUploaded.load(), uploads) << "released images came back and were uploaded";
    EXPECT_EQ(layer().tileStats().errors.load(), 0u);
}

// Overlays keep their QSGNode hosting on the gpu root: TPO letters draw (after the
// gated tile node, so on top of it), and the gate blocks the tile node while the
// heatmap layer is off.
TEST_F(UgrGpu, TpoOverlayDrawsOnTopOfTheGpuRoot) {
    gpuOn();
    ASSERT_TRUE(settle()) << error.toStdString();
    ugr->setTpoLayerEnabled(true); // the heatmap layer goes off (existing layer rule)
    ASSERT_TRUE(frames(5)) << error.toStdString();
    const QImage plain = image;
    TradeOverlayGrid grid;
    grid.startMs = viewLo - 30 * minute;
    grid.endMs = grid.startMs + 24 * kHourMs;
    grid.maxPrice = 100'300;
    grid.tick = 10;
    grid.generation = 1;
    QByteArray letters(40, '\0');
    for (int r = 10; r < 30; ++r) letters[r] = 'A';
    emit ugr->getDataProcessor()->tpoColumnReady(0, 48, 40, letters, grid.startMs, grid.endMs, 30 * minute, grid);
    ASSERT_TRUE(frames(5)) << error.toStdString();
    int changed = 0;
    for (int y = 0; y < 320; y += 2)
        for (int x = 0; x < 640; x += 2) changed += image.pixel(x, y) != plain.pixel(x, y);
    EXPECT_GT(changed, 200) << "the TPO profile draws on the gpu root";
    QSGNode *root = paintRoot(ugr);
    ASSERT_TRUE(root && root->firstChild());
    EXPECT_EQ(root->firstChild()->type(), QSGNode::OpacityNodeType) << "the tile node is the first (bottom) child";
    EXPECT_EQ(static_cast<QSGOpacityNode *>(root->firstChild())->opacity(), 0.0) << "heatmap layer off: gated";
    EXPECT_GE(root->childCount(), 2) << "overlays follow the tile node";
}

// The renderer flips at runtime both ways: gpu mutes the legacy band stream and
// draws the tile node with the clamps; legacy restores the HeatmapIntensityNode
// root, unmutes the stream, drops the clamps and destroys the controller.
TEST_F(UgrGpu, RendererFlipsBothWays) {
    auto processorEnabled = [&] {
        bool enabled = false;
        QMetaObject::invokeMethod(ugr->getDataProcessor(), [&] { enabled = ugr->getDataProcessor()->heatmapEnabled(); },
                                  Qt::BlockingQueuedConnection);
        return enabled;
    };
    ugr->setViewport(viewLo, viewHi, 99'900, 100'300);
    ASSERT_TRUE(frames(3)) << error.toStdString();
    ASSERT_TRUE(paintRoot(ugr));
    EXPECT_EQ(paintRoot(ugr)->type(), QSGNode::GeometryNodeType) << "legacy root";
    EXPECT_TRUE(processorEnabled());
    EXPECT_EQ(ugr->getViewState()->maxTimeSpanMs(), 0.0);
    ugr->setHeatmapRenderer("gpu");
    ASSERT_TRUE(settle()) << error.toStdString();
    EXPECT_EQ(paintRoot(ugr)->type(), QSGNode::BasicNodeType);
    EXPECT_FALSE(processorEnabled()) << "legacy band stream muted";
    EXPECT_GT(ugr->getViewState()->maxTimeSpanMs(), 0.0);
    EXPECT_EQ(lab::LabData::instance().service().controllerCount(), 1u);
    ugr->setHeatmapRenderer("legacy");
    ASSERT_TRUE(frames(3)) << error.toStdString();
    EXPECT_EQ(paintRoot(ugr)->type(), QSGNode::GeometryNodeType) << "legacy root again";
    EXPECT_TRUE(processorEnabled());
    EXPECT_EQ(ugr->getViewState()->maxTimeSpanMs(), 0.0);
    EXPECT_EQ(lab::LabData::instance().service().controllerCount(), 0u);
    ugr->setHeatmapRenderer("gpu");
    ASSERT_TRUE(settle()) << error.toStdString() << " (second flip)";
}

// Spec rules 1, 2 and 9 through the chart (port of LabItemTest.
// ShiftWheelScalesPriceOnlyWithinTheClamps): Shift+wheel scales price only
// (macOS delivers it as a horizontal delta); Manual clamps price at one row per
// pixel for every input; time zoom-out stops at one column per pixel; Auto
// re-ticks instead of clamping.
TEST_F(UgrGpu, ShiftWheelScalesPriceOnlyWithinTheClamps) {
    gpuOn();
    ASSERT_TRUE(settle()) << error.toStdString();
    auto *view = ugr->getViewState();
    auto timeSpan = [&] { return double(view->getVisibleTimeEnd() - view->getVisibleTimeStart()); };
    auto priceSpan = [&] { return view->getMaxPrice() - view->getMinPrice(); };
    auto wheel = [&](QPoint angle, Qt::KeyboardModifiers modifiers) {
        const QPointF at(320, 160);
        QWheelEvent event(at, scene->window()->mapToGlobal(at), {}, angle, Qt::NoButton, modifiers, Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(scene->window(), &event);
    };
    const double time0 = timeSpan(), price0 = priceSpan();
    wheel({120, 0}, Qt::ShiftModifier); // Shift+wheel as macOS sends it
    EXPECT_EQ(timeSpan(), time0) << "price only";
    EXPECT_NEAR(priceSpan(), price0 / 1.06, 1e-6 * price0);
    const double price1 = priceSpan();
    wheel({0, 120}, Qt::ShiftModifier); // a mouse that keeps the vertical delta
    EXPECT_LT(priceSpan(), price1);
    EXPECT_EQ(timeSpan(), time0);
    // Manual $10: zoom-out stops at one row per pixel (320 px -> $3200).
    auto manual = brightSettings();
    manual.tickMode = heatmap::TickMode::Manual;
    manual.manualTick = 1000;
    ugr->setHeatmapChartSettings(manual, true);
    for (int i = 0; i < 80; ++i) wheel({-120, 0}, Qt::ShiftModifier);
    EXPECT_NEAR(priceSpan(), 3200, 1e-6);
    // Plain wheel zoom-out stops at one column per pixel (640 px at 1m).
    for (int i = 0; i < 80; ++i) wheel({0, -120}, Qt::NoModifier);
    EXPECT_LE(timeSpan(), 640.0 * minute);
    EXPECT_GE(timeSpan(), 639.0 * minute);
    EXPECT_NEAR(priceSpan(), 3200, 1e-6) << "the Manual price clamp holds for plain wheel too";
    ASSERT_TRUE(settle()) << error.toStdString();
    EXPECT_EQ(layer().tickUnits(), 1000) << "Manual draws the locked tick";
    ugr->setHeatmapChartSettings(brightSettings()); // Auto re-ticks instead of clamping
    for (int i = 0; i < 10; ++i) wheel({-120, 0}, Qt::ShiftModifier);
    EXPECT_GT(priceSpan(), 3200);
}

// Review blocker 1: MainWindowGPU destroys its HeatmapDataService (a member)
// before the base QWidget deletes the chart. The chart's layer must not touch
// the controller or the service afterwards (Guard Malloc run in CMake).
TEST_F(UgrGpu, TheServiceMayDieBeforeTheChart) {
    // A passive transport: the controller is created and requests chunks, and no
    // read is in flight at teardown (a local reader's in-flight read on teardown is
    // the lab-only FM-125, unrelated to this order).
    FakeChunkTransport *transport = nullptr;
    auto service = std::make_unique<heatmap::HeatmapDataService>([&](QObject *) -> heatmap::ChunkTransport * {
        transport = new FakeChunkTransport;
        return transport;
    }, heatmap::HeatmapBudgets{}, [](heatmap::ChunkTransport &t) { static_cast<FakeChunkTransport &>(t).goOnline(); });
    ugr->setHeatmapService(service.get());
    gpuOn();
    ASSERT_TRUE(frames(3)) << error.toStdString();
    ASSERT_NE(layer().controller(), nullptr);
    EXPECT_EQ(service->controllerCount(), 1u);
    service.reset(); // as ~MainWindowGPU's members go first
    EXPECT_EQ(layer().controller(), nullptr) << "the layer forgot the controller the service deleted";
    ASSERT_TRUE(frames(3)) << error.toStdString();
    ugr->setTimeframe(int(5 * minute)); // GUI-side work after the service is gone
    ASSERT_TRUE(frames(2)) << error.toStdString();
    delete ugr; // as the base QWidget deletes the dock
    ugr = nullptr;
    ASSERT_FALSE(scene->renderFrame(&error).isNull());
}

// Review blocker 2: a book-seeded GPU start leaves the legacy stream's bootstrap
// and initial price centring pending; flipping to legacy must adopt the view,
// not re-apply initial_price_pct on the next book or trade.
TEST_F(UgrGpu, FlipToLegacyKeepsABookSeededView) {
    recordingMode();
    ugr->setHeatmapRenderer("gpu");
    ugr->setLiveBookTop(100'100, 100'101);
    ASSERT_TRUE(ugr->getViewState()->isTimeWindowValid());
    const qint64 start = ugr->getViewState()->getVisibleTimeStart(), end = ugr->getViewState()->getVisibleTimeEnd();
    EXPECT_NEAR(priceSpan(), 102.4, 1e-9) << "the seed: 5% of a 2048-row $1 band";
    ASSERT_TRUE(frames(3)) << error.toStdString();
    ugr->setHeatmapRenderer("legacy");
    ugr->setLiveBookTop(100'120, 100'121);
    Trade trade{};
    trade.product_id = "BTC-USD";
    trade.price = 100'125;
    ugr->onTradeReceived(trade);
    ASSERT_TRUE(frames(3)) << error.toStdString();
    EXPECT_NEAR(priceSpan(), 102.4, 1e-9) << "the legacy resume adopted the view";
    EXPECT_EQ(ugr->getViewState()->getVisibleTimeStart(), start);
    EXPECT_EQ(ugr->getViewState()->getVisibleTimeEnd(), end);
}
TEST_F(UgrGpu, FlipToLegacyAfterAGpuSymbolSwitchKeepsTheView) {
    recordingMode();
    ugr->setHeatmapRenderer("gpu");
    ugr->setLiveBookTop(100'100, 100'101);
    ugr->setActiveSymbol("ETH-USD"); // resets the legacy price centring while gpu draws
    ugr->setLiveBookTop(3'000, 3'000.5);
    const double lo = ugr->getViewState()->getMinPrice();
    EXPECT_NEAR(priceSpan(), 102.4, 1e-9);
    EXPECT_LT(lo, 3'000);
    ugr->setHeatmapRenderer("legacy");
    ugr->setLiveBookTop(3'010, 3'010.5);
    ASSERT_TRUE(frames(3)) << error.toStdString();
    EXPECT_NEAR(priceSpan(), 102.4, 1e-9);
    EXPECT_DOUBLE_EQ(ugr->getViewState()->getMinPrice(), lo);
}

// Review major 3: the chart schedules its own frames while the node has work
// (budgeted uploads, a crossfade) and stops when idle; the test renders only
// when the scene asks.
TEST_F(UgrGpu, NodeWorkGetsItsFramesWithoutOutsideRedraws) {
    gpuOn();
    ASSERT_TRUE(settle()) << error.toStdString();
    // A tick change with a long crossfade.
    auto s = brightSettings();
    s.crossfadeMs = 400;
    s.tickMode = heatmap::TickMode::Manual;
    s.manualTick = 500; // $5
    ugr->setHeatmapChartSettings(s, true);
    const auto &stats = layer().tileStats();
    int rendered = 0;
    bool faded = false;
    ASSERT_TRUE(pumpOnRequest(3000, [&] {
        faded = faded || stats.crossfading.load();
        return faded && !stats.crossfading.load() && layer().settled();
    }, &rendered)) << "the crossfade stalled (frames rendered: " << rendered << ")";
    // Small upload budget: a new timeframe's sources page in over many frames.
    s.uploadBudgetBytes = 64 << 10;
    ugr->setHeatmapChartSettings(s);
    rendered = 0;
    ugr->setTimeframe(int(15 * minute));
    ASSERT_TRUE(pumpOnRequest(15'000, [&] { return layer().settled(); }, &rendered))
        << "uploads stalled (frames rendered: " << rendered << ")";
    EXPECT_GE(rendered, 2) << "budgeted uploads took more than one frame";
    // Idle: no more frame requests.
    rendered = 0;
    pumpOnRequest(500, [] { return false; }, &rendered);
    EXPECT_LE(rendered, 2) << "an idle chart stops asking for frames";
}

// Review major 4: follow-live activation returns to the live anchor (the
// recording's end here: no live subscription) from history and from the future.
TEST_F(UgrGpu, FollowLiveReturnsFromHistoryAndFromTheFuture) {
    gpuOn(); // a historical view (2 h before the end); follow-live off
    ASSERT_TRUE(settle()) << error.toStdString();
    const double span = timeSpan();
    const int64_t anchor = layer().liveAnchorMs();
    ASSERT_EQ(anchor, epoch + 4 * kHourMs);
    const int64_t expectedEnd = anchor + std::max<int64_t>(minute, int64_t(span * 0.08));
    ugr->enableAutoScroll(true);
    EXPECT_EQ(ugr->getViewState()->getVisibleTimeEnd(), expectedEnd) << "from history";
    EXPECT_EQ(timeSpan(), span);
    ugr->setViewport(epoch + 10 * kHourMs, epoch + 10 * kHourMs + int64_t(span), 99'900, 100'300); // the future
    ugr->enableAutoScroll(true);
    EXPECT_EQ(ugr->getViewState()->getVisibleTimeEnd(), expectedEnd) << "from the future";
    ASSERT_TRUE(settle()) << error.toStdString();
}

// Owner decision (auto-fit): double-click on the price axis fits price to the visible
// candles' high/low plus a small margin, in one viewport change, keeping follow-live.
TEST_F(UgrGpu, PriceAxisFitLandsTheVisibleCandlesInView) {
    gpuOn(); // 99,900..100,300 over [viewLo, viewHi)
    CandleSeriesBuffer buffer;
    std::vector<CandleSeriesBuffer::CandleBar> bars;
    for (int64_t t = viewLo; t < viewHi; t += minute) {
        const double base = 101'000 + double((t - viewLo) / minute) * 10; // 101,000 .. 101,390 + 50
        bars.push_back({t, t + minute, base, base + 50, base - 20, base + 30, 1.0, true, 0, false});
    }
    // A candle outside the view does not count.
    bars.push_back({viewHi + 10 * minute, viewHi + 11 * minute, 90'000, 120'000, 80'000, 90'000, 1.0, true, 0, false});
    buffer.applyHistory("BTC-USD", 60, bars);
    ugr->setCandleBuffer(&buffer);
    const auto *view = ugr->getViewState();
    const uint64_t before = view->getViewportVersion();
    const int64_t t0 = view->getVisibleTimeStart(), t1 = view->getVisibleTimeEnd();
    ASSERT_TRUE(ugr->fitPriceToData());
    EXPECT_EQ(view->getViewportVersion() - before, 1u) << "one viewport change";
    EXPECT_EQ(view->getVisibleTimeStart(), t0) << "price only";
    EXPECT_EQ(view->getVisibleTimeEnd(), t1);
    const double lo = 101'000 - 20, hi = 101'000 + 39 * 10 + 50;
    const double margin = (hi - lo) * UnifiedGridRenderer::kFitPriceMargin;
    EXPECT_NEAR(view->getMinPrice(), lo - margin, 1e-6);
    EXPECT_NEAR(view->getMaxPrice(), hi + margin, 1e-6);
    // Following live stays on (a price fit is not a pan).
    ugr->enableAutoScroll(true);
    ugr->setViewport(view->getVisibleTimeStart(), view->getVisibleTimeEnd(), 99'000, 99'100);
    ugr->enableAutoScroll(true);
    ASSERT_TRUE(ugr->autoScrollEnabled());
    ugr->fitPriceToData();
    EXPECT_TRUE(ugr->autoScrollEnabled());
    ugr->setCandleBuffer(nullptr);
}

// Manual tick ($10 over 320 px: at most $3200) cannot hold candles spanning $5000.
// Owner decision 2026-10-02: keep the Manual tick and show the max span centred on the
// current price (book mid, then last trade, then the newest close), within one row.
TEST_F(UgrGpu, PriceAxisFitUnderTheManualClampCentresTheCurrentPrice) {
    gpuOn();
    auto manual = brightSettings();
    manual.tickMode = heatmap::TickMode::Manual;
    manual.manualTick = 1000;
    ugr->setHeatmapChartSettings(manual, true);
    const auto *view = ugr->getViewState();
    // The limits need BTC's price scale (its first snapshot).
    ASSERT_TRUE(pump(30'000, [&] { return layer().priceScaleCurrent() && view->maxPriceSpan() > 0; }));
    ASSERT_DOUBLE_EQ(view->maxPriceSpan(), 3200);
    const double row = 10;
    CandleSeriesBuffer buffer;
    std::vector<CandleSeriesBuffer::CandleBar> bars;
    for (int i = 0; i < 40; ++i) { // 100,000 rising to 105,000
        const int64_t t = viewLo + i * minute;
        const double close = 100'000 + 5000.0 * (i + 1) / 40;
        bars.push_back({t, t + minute, close - 125, close, close - 125, close, 1.0, true, 0, false});
    }
    buffer.applyHistory("BTC-USD", 60, bars);
    ugr->setCandleBuffer(&buffer);
    auto centre = [&] { return (view->getMinPrice() + view->getMaxPrice()) / 2; };
    // No book or trade yet: the newest close.
    ASSERT_TRUE(ugr->fitPriceToData());
    EXPECT_DOUBLE_EQ(view->getMaxPrice() - view->getMinPrice(), 3200) << "the Manual max span, tick kept";
    EXPECT_NEAR(centre(), 105'000, row) << "centred on the newest close";
    // A trade: the last trade price.
    Trade trade{};
    trade.product_id = "BTC-USD";
    trade.price = 104'200;
    ugr->addTrade(trade);
    ASSERT_TRUE(ugr->fitPriceToData());
    EXPECT_NEAR(centre(), 104'200, row) << "centred on the last trade";
    // A book: its mid wins.
    ugr->setLiveBookTop(103'499, 103'501);
    ASSERT_TRUE(ugr->fitPriceToData());
    EXPECT_NEAR(centre(), 103'500, row) << "centred on the book mid";
    EXPECT_DOUBLE_EQ(view->getMaxPrice() - view->getMinPrice(), 3200);
    EXPECT_EQ(layer().manualMode(), true) << "never switched to Auto";
    EXPECT_EQ(layer().manualTickUnits(), 1000) << "never coarsened";
    ugr->setCandleBuffer(nullptr);
}

// No visible candle: the price axis fit keeps the span and centres the live price.
TEST_F(UgrGpu, PriceAxisFitWithoutCandlesCentresTheLivePrice) {
    gpuOn();
    ugr->setLiveBookTop(101'999, 102'001);
    const auto *view = ugr->getViewState();
    const double span = priceSpan();
    const uint64_t before = view->getViewportVersion();
    ASSERT_TRUE(ugr->fitPriceToData());
    EXPECT_EQ(view->getViewportVersion() - before, 1u);
    EXPECT_NEAR(view->getMinPrice(), 102'000 - span / 2, 1e-6);
    EXPECT_NEAR(view->getMaxPrice(), 102'000 + span / 2, 1e-6);
}

// Owner decision (auto-fit): double-click on the time axis shows the data's available
// range (the 4 h recording here) inside the 1 column/px limit, live edge padded.
TEST_F(UgrGpu, TimeAxisFitShowsTheAvailableRange) {
    gpuOn(); // 40 minutes in the middle of the recording
    const auto *view = ugr->getViewState();
    const int64_t anchor = layer().liveAnchorMs();
    ASSERT_EQ(anchor, epoch + 4 * kHourMs);
    ASSERT_EQ(layer().oldestAvailableMs(), epoch);
    const double p0 = view->getMinPrice(), p1 = view->getMaxPrice();
    const uint64_t before = view->getViewportVersion();
    ASSERT_TRUE(ugr->fitTimeToData());
    EXPECT_EQ(view->getViewportVersion() - before, 1u) << "one viewport change";
    EXPECT_EQ(view->getMinPrice(), p0) << "time only";
    EXPECT_EQ(view->getMaxPrice(), p1);
    const int64_t span = view->getVisibleTimeEnd() - view->getVisibleTimeStart();
    EXPECT_LE(view->getVisibleTimeStart(), epoch + minute) << "the oldest data is in view";
    EXPECT_GE(view->getVisibleTimeStart(), epoch - 2 * minute);
    EXPECT_EQ(view->getVisibleTimeEnd(), anchor + std::max<int64_t>(minute, int64_t(double(span) * 0.08)))
        << "the live edge one padding inside";
    EXPECT_FALSE(ugr->autoScrollEnabled()) << "a historical view stays historical";
    // Wider data than the limit (a 100 px chart: 100 one-minute columns): the max span.
    // Following live it ends at the live edge; otherwise it keeps the view centre.
    ugr->setSize(QSizeF(100, 320));
    const int64_t maxSpan = int64_t(view->maxTimeSpanMs());
    ASSERT_LT(maxSpan, 4 * kHourMs);
    ugr->setViewport(viewLo, viewLo + 20 * minute, 99'900, 100'300);
    ASSERT_TRUE(ugr->fitTimeToData());
    EXPECT_EQ(view->getVisibleTimeEnd() - view->getVisibleTimeStart(), maxSpan);
    EXPECT_NEAR(double(view->getVisibleTimeStart() + view->getVisibleTimeEnd()) / 2, double(viewLo + 10 * minute),
                double(minute));
    ugr->enableAutoScroll(true);
    ASSERT_TRUE(ugr->fitTimeToData());
    EXPECT_EQ(view->getVisibleTimeEnd() - view->getVisibleTimeStart(), maxSpan);
    EXPECT_EQ(view->getVisibleTimeEnd(), anchor + std::max<int64_t>(minute, int64_t(double(maxSpan) * 0.08)));
    EXPECT_TRUE(ugr->autoScrollEnabled());
    ugr->setSize(QSizeF(640, 320));
}

// Review minor 3: a view that starts inside a candle (1m candles, view from :30 s):
// the first overlapping candle counts for the fit, with the extreme in it.
TEST_F(UgrGpu, PriceFitCountsTheCandleUnderTheLeftEdge) {
    gpuOn();
    CandleSeriesBuffer buffer;
    std::vector<CandleSeriesBuffer::CandleBar> bars;
    for (int64_t t = viewLo; t < viewLo + 40 * minute; t += minute) {
        const double low = t == viewLo ? 95'000 : 100'000; // the extreme in the first candle
        bars.push_back({t, t + minute, 100'100, 100'200, low, 100'150, 1.0, true, 0, false});
    }
    buffer.applyHistory("BTC-USD", 60, bars);
    ugr->setCandleBuffer(&buffer);
    ugr->setViewport(viewLo + 30'000, viewLo + 30'000 + 39 * minute, 99'900, 100'300);
    ASSERT_TRUE(ugr->fitPriceToData());
    EXPECT_LE(ugr->getViewState()->getMinPrice(), 95'000) << "the candle that straddles the left edge counts";
    ugr->setCandleBuffer(nullptr);
}

// ── Auto price scale (docs/research/2026-10-viewport-autoscale.md) ─────────────
// Candles every tf over [from, to): bar i has low base + i * step and high low + 50.
std::vector<CandleSeriesBuffer::CandleBar> risingBars(int64_t from, int64_t to, int64_t tf, double base,
                                                      double step = 10) {
    std::vector<CandleSeriesBuffer::CandleBar> bars;
    int i = 0;
    for (int64_t t = from / tf * tf; t < to; t += tf, ++i) {
        const double low = base + step * i;
        bars.push_back({t, t + tf, low + 10, low + 50, low, low + 40, 1.0, true, 0, false});
    }
    return bars;
}
// The fitted range for the candles overlapping [start, end): high/low plus the margin.
std::pair<double, double> expectedFit(const std::vector<CandleSeriesBuffer::CandleBar> &bars, int64_t start,
                                      int64_t end) {
    double lo = 1e300, hi = -1e300;
    for (const auto &b : bars)
        if (b.timeStartMs < end && b.timeEndMs > start) lo = std::min(lo, b.low), hi = std::max(hi, b.high);
    const double margin = (hi - lo) * UnifiedGridRenderer::kFitPriceMargin;
    return {lo - margin, hi + margin};
}

// While on, the price follows the visible candles as the view moves in time (keyboard
// pan, drag, chart wheel, time axis zoom), each in ONE viewport change; drags and
// keyboard pans move time only; the wheel zooms time and keeps it on.
TEST_F(UgrGpu, AutoScaleFollowsTheVisibleCandlesAcrossTimeChanges) {
    gpuOn(); // explicit price: auto price scale off
    ASSERT_FALSE(ugr->autoPriceScale()) << "an explicit price range turns it off";
    CandleSeriesBuffer buffer;
    const auto bars = risingBars(viewLo - 4 * kHourMs, viewHi + kHourMs, minute, 90'000);
    buffer.applyHistory("BTC-USD", 60, bars);
    ugr->setCandleBuffer(&buffer);
    const auto *view = ugr->getViewState();
    auto expectFitted = [&](const char *what) {
        const auto [lo, hi] = expectedFit(bars, view->getVisibleTimeStart(), view->getVisibleTimeEnd());
        EXPECT_NEAR(view->getMinPrice(), lo, 1e-6) << what;
        EXPECT_NEAR(view->getMaxPrice(), hi, 1e-6) << what;
    };
    uint64_t v = view->getViewportVersion();
    ugr->setAutoPriceScale(true);
    EXPECT_TRUE(ugr->autoPriceScale());
    EXPECT_EQ(view->getViewportVersion() - v, 1u);
    expectFitted("on");
    v = view->getViewportVersion();
    ugr->panLeft();
    EXPECT_EQ(view->getViewportVersion() - v, 1u) << "a time pan and its refit: one change";
    expectFitted("keyboard pan");
    // A drag with vertical motion moves time only.
    const int64_t t0 = view->getVisibleTimeStart();
    ugr->beginPanAt(320, 160);
    ugr->updatePanAt(160, 40);
    EXPECT_EQ(view->getPanVisualOffset().y(), 0.0) << "no vertical pan while on";
    v = view->getViewportVersion();
    ugr->endPanAt();
    EXPECT_EQ(view->getViewportVersion() - v, 1u);
    EXPECT_GT(view->getVisibleTimeStart(), t0) << "dragged left: later times";
    expectFitted("drag");
    v = view->getViewportVersion();
    ugr->panUp();
    EXPECT_EQ(view->getViewportVersion(), v) << "keyboard vertical pan ignored";
    // The chart wheel zooms time only and keeps it on.
    const double span0 = timeSpan();
    v = view->getViewportVersion();
    ugr->zoomAt(-240, 320, 160);
    EXPECT_EQ(view->getViewportVersion() - v, 1u);
    EXPECT_GT(timeSpan(), span0);
    EXPECT_TRUE(ugr->autoPriceScale()) << "the wheel keeps it on";
    expectFitted("wheel zoom out");
    v = view->getViewportVersion();
    ugr->zoomTimeAt(240, 320);
    EXPECT_EQ(view->getViewportVersion() - v, 1u);
    expectFitted("time axis zoom");
    ugr->setCandleBuffer(nullptr);
}

// Candle updates refit only when they are in view and change the fit: one change, or none.
TEST_F(UgrGpu, AutoScaleRefitsOnCandleUpdatesOnlyWhenTheFitChanges) {
    gpuOn();
    CandleSeriesBuffer buffer;
    auto bars = risingBars(viewLo, viewHi, minute, 100'000);
    buffer.applyHistory("BTC-USD", 60, bars);
    ugr->setCandleBuffer(&buffer);
    ugr->setAutoPriceScale(true);
    const auto *view = ugr->getViewState();
    const double lo0 = view->getMinPrice(), hi0 = view->getMaxPrice();
    uint64_t v = view->getViewportVersion();
    auto update = [&](int64_t t, double low, double high, uint64_t seq, const char *symbol = "BTC-USD") {
        buffer.applyUpdate(symbol, 60, {t, t + minute, low, high, low, high, 1.0, false, 0, false}, seq, false);
    };
    update(viewLo + 10 * minute, 100'120, 100'140, 1); // inside the range
    EXPECT_EQ(view->getViewportVersion(), v) << "no new extreme: no viewport change";
    update(viewLo + 20 * minute, 100'200, 101'000, 2); // a new high in view
    EXPECT_EQ(view->getViewportVersion() - v, 1u);
    EXPECT_GT(view->getMaxPrice(), 101'000) << "the new high is in view";
    EXPECT_NEAR(view->getMinPrice(), 100'000 - 1'000 * UnifiedGridRenderer::kFitPriceMargin, 1e-6) << "refitted";
    EXPECT_LT(view->getMinPrice(), lo0);
    EXPECT_GT(view->getMaxPrice(), hi0);
    v = view->getViewportVersion();
    update(viewHi + 30 * minute, 50'000, 150'000, 3); // out of view
    update(viewLo + 5 * minute, 50'000, 150'000, 4, "ETH-USD"); // another symbol
    EXPECT_EQ(view->getViewportVersion(), v) << "nothing in view changed";
    // Off: a new extreme in view leaves the range alone.
    ugr->setAutoPriceScale(false);
    update(viewLo + 21 * minute, 99'000, 102'000, 5);
    EXPECT_EQ(view->getViewportVersion(), v);
    ugr->setCandleBuffer(nullptr);
}

// A price zoom (axis drag/wheel) turns it off; off, the range stays where the user left
// it across time pans and timeframe switches, and drags pan price again. On, a
// timeframe switch lands on the new timeframe's candles in its single viewport change,
// and history arriving later refits.
TEST_F(UgrGpu, PriceZoomTurnsAutoScaleOffAndTheRangeSurvivesATimeframeSwitch) {
    gpuOn();
    CandleSeriesBuffer buffer;
    const int64_t tf5 = 5 * minute, tf15 = 15 * minute;
    buffer.applyHistory("BTC-USD", 60, risingBars(viewLo - kHourMs, viewHi + kHourMs, minute, 100'000));
    const auto bars5 = risingBars(viewHi - 60 * tf5, viewHi + 10 * tf5, tf5, 105'000, 20);
    buffer.applyHistory("BTC-USD", 300, bars5);
    ugr->setCandleBuffer(&buffer);
    ugr->setAutoPriceScale(true);
    const auto *view = ugr->getViewState();
    ugr->zoomPriceAt(120, 100, 320);
    EXPECT_FALSE(ugr->autoPriceScale()) << "a price zoom turns it off";
    const double lo = view->getMinPrice(), hi = view->getMaxPrice();
    ugr->setTimeframe(int(tf5));
    EXPECT_EQ(view->getMinPrice(), lo) << "off: the range survives the switch";
    EXPECT_EQ(view->getMaxPrice(), hi);
    ugr->panLeft();
    EXPECT_EQ(view->getMinPrice(), lo) << "and a time pan";
    ugr->beginPanAt(320, 160);
    ugr->updatePanAt(320, 200);
    EXPECT_EQ(view->getPanVisualOffset().y(), 40.0) << "off: vertical pan again";
    ugr->endPanAt();
    EXPECT_GT(view->getMinPrice(), lo);
    // On again (the price-axis double-click), back to 1m, then 5m: fitted in one change.
    ugr->setTimeframe(int(minute));
    ASSERT_TRUE(ugr->fitPriceToData());
    EXPECT_TRUE(ugr->autoPriceScale());
    uint64_t v = view->getViewportVersion();
    ugr->setTimeframe(int(tf5));
    EXPECT_EQ(view->getViewportVersion() - v, 1u) << "scaled span and fitted price: one change";
    {
        const auto [flo, fhi] = expectedFit(bars5, view->getVisibleTimeStart(), view->getVisibleTimeEnd());
        EXPECT_NEAR(view->getMinPrice(), flo, 1e-6);
        EXPECT_NEAR(view->getMaxPrice(), fhi, 1e-6);
    }
    // 15m not held yet: the switch keeps the price; the history page refits (one change).
    const double plo = view->getMinPrice();
    v = view->getViewportVersion();
    ugr->setTimeframe(int(tf15));
    EXPECT_EQ(view->getViewportVersion() - v, 1u);
    EXPECT_EQ(view->getMinPrice(), plo);
    const auto bars15 = risingBars(view->getVisibleTimeStart(), view->getVisibleTimeEnd(), tf15, 98'000, 0);
    v = view->getViewportVersion();
    buffer.applyHistory("BTC-USD", 900, bars15);
    EXPECT_EQ(view->getViewportVersion() - v, 1u);
    const auto [flo, fhi] = expectedFit(bars15, view->getVisibleTimeStart(), view->getVisibleTimeEnd());
    EXPECT_NEAR(view->getMinPrice(), flo, 1e-6);
    EXPECT_NEAR(view->getMaxPrice(), fhi, 1e-6);
    ugr->setCandleBuffer(nullptr);
}

// A symbol switch keeps the state. On: the new symbol's candles fit. Off: the zoom is
// carried as a percentage (span / price) with the current price at the same height,
// here from BTC ~86k to a 0.00001 symbol. The time window (Now column, bar width) stays.
TEST_F(UgrGpu, SymbolSwitchKeepsTheAutoScaleState) {
    gpuOn();
    CandleSeriesBuffer buffer;
    const auto btc = risingBars(viewLo - kHourMs, viewHi + kHourMs, minute, 85'500);
    std::vector<CandleSeriesBuffer::CandleBar> tiny;
    for (int64_t t = viewLo - kHourMs; t < viewHi + kHourMs; t += minute)
        tiny.push_back({t, t + minute, 0.0000100, 0.0000104, 0.0000097, 0.0000101, 1.0, true, 0, false});
    buffer.applyHistory("BTC-USD", 60, btc);
    buffer.applyHistory("TINY-USD", 60, tiny);
    ugr->setCandleBuffer(&buffer);
    const auto *view = ugr->getViewState();
    const int64_t t0 = view->getVisibleTimeStart(), t1 = view->getVisibleTimeEnd();
    // On.
    ugr->setAutoPriceScale(true);
    ugr->setActiveSymbol("TINY-USD");
    EXPECT_TRUE(ugr->autoPriceScale());
    {
        const auto [lo, hi] = expectedFit(tiny, t0, t1);
        EXPECT_NEAR(view->getMinPrice(), lo, 1e-15) << "the new symbol's candles fit";
        EXPECT_NEAR(view->getMaxPrice(), hi, 1e-15);
    }
    EXPECT_EQ(view->getVisibleTimeStart(), t0) << "the time window stays";
    EXPECT_EQ(view->getVisibleTimeEnd(), t1);
    ugr->setActiveSymbol("BTC-USD");
    {
        const auto [lo, hi] = expectedFit(btc, t0, t1);
        EXPECT_NEAR(view->getMinPrice(), lo, 1e-6);
        EXPECT_NEAR(view->getMaxPrice(), hi, 1e-6);
    }
    // Off: 85,500..87,500 with the book at 86,000 (a quarter up the view).
    ugr->setViewport(t0, t1, 85'500, 87'500);
    ASSERT_FALSE(ugr->autoPriceScale());
    ugr->setLiveBookTop(85'999, 86'001);
    ugr->setActiveSymbol("TINY-USD");
    EXPECT_FALSE(ugr->autoPriceScale());
    ugr->setLiveBookTop(0.0000099, 0.0000101);
    const double mid = 0.00001, span = mid * 2'000 / 86'000;
    EXPECT_NEAR(view->getMaxPrice() - view->getMinPrice(), span, span * 1e-9) << "the same span / price";
    EXPECT_NEAR(view->getMinPrice(), mid - 0.25 * span, span * 1e-9) << "the price at the same height";
    EXPECT_EQ(view->getVisibleTimeStart(), t0);
    EXPECT_EQ(view->getVisibleTimeEnd(), t1);
    // And back: the carry works both ways.
    ugr->setActiveSymbol("BTC-USD");
    ugr->setLiveBookTop(85'999, 86'001);
    EXPECT_NEAR(view->getMinPrice(), 85'500, 1e-6);
    EXPECT_NEAR(view->getMaxPrice(), 87'500, 1e-6);
    ugr->setCandleBuffer(nullptr);
}

// Review item 2: a drag commits only at release, so the fit and the candle checks use
// the window on screen (the committed one shifted by the drag): revealed candles fit
// during the drag, updates beyond the committed end refit, a cancelled drag refits back.
TEST_F(UgrGpu, AutoScaleFitsTheDisplayedWindowDuringADrag) {
    gpuOn();
    CandleSeriesBuffer buffer;
    const auto bars = risingBars(viewLo - 4 * kHourMs, viewHi + kHourMs, minute, 90'000);
    buffer.applyHistory("BTC-USD", 60, bars);
    ugr->setCandleBuffer(&buffer);
    ugr->setAutoPriceScale(true);
    auto *view = ugr->getViewState();
    const int64_t t0 = view->getVisibleTimeStart(), t1 = view->getVisibleTimeEnd();
    ugr->beginPanAt(320, 160);
    ugr->updatePanAt(160, 160); // a quarter of the width left: 10 minutes later
    const auto [s, e] = view->displayedTimeWindow();
    EXPECT_EQ(s, t0 + 10 * minute);
    EXPECT_EQ(e, t1 + 10 * minute);
    EXPECT_EQ(view->getVisibleTimeStart(), t0) << "nothing committed before release";
    {
        const auto [lo, hi] = expectedFit(bars, s, e);
        EXPECT_NEAR(view->getMinPrice(), lo, 1e-6) << "fitted to the displayed window during the drag";
        EXPECT_NEAR(view->getMaxPrice(), hi, 1e-6);
    }
    auto update = [&](int64_t t, double low, double high, uint64_t seq) {
        buffer.applyUpdate("BTC-USD", 60, {t, t + minute, low, high, low, high, 1.0, false, 0, false}, seq, false);
    };
    uint64_t v = view->getViewportVersion();
    update(t0 + 2 * minute, 50'000, 150'000, 1); // committed, no longer on screen
    EXPECT_EQ(view->getViewportVersion(), v) << "an off-screen candle does not refit";
    update(t1 + 5 * minute, 90'500, 99'000, 2); // revealed by the drag (after the committed end)
    EXPECT_EQ(view->getViewportVersion() - v, 1u) << "a revealed candle refits";
    EXPECT_GT(view->getMaxPrice(), 99'000);
    ugr->endPanAt();
    EXPECT_EQ(view->getVisibleTimeStart(), t0 + 10 * minute);
    EXPECT_GT(view->getMaxPrice(), 99'000);
    // A cancelled drag keeps the time: the price fits the committed window again.
    const double lo1 = view->getMinPrice(), hi1 = view->getMaxPrice();
    ugr->beginPanAt(320, 160);
    ugr->updatePanAt(600, 160); // reveals older, lower candles
    EXPECT_LT(view->getMinPrice(), lo1);
    view->handlePanEnd(false);
    EXPECT_DOUBLE_EQ(view->getMinPrice(), lo1);
    EXPECT_DOUBLE_EQ(view->getMaxPrice(), hi1);
    ugr->setCandleBuffer(nullptr);
}

// Review items 3-5: POST /api/v1/viewport resolves its flags and final window first
// and commits once; explicit price bounds (even the current ones) turn the auto price
// scale off; a fit that cannot happen changes nothing (the A toggle still arms).
TEST_F(UgrGpu, ViewportRequestsResolveFlagsAndCommitOnce) {
    gpuOn();
    CandleSeriesBuffer buffer;
    const auto bars = risingBars(viewLo - 4 * kHourMs, epoch + 5 * kHourMs, minute, 90'000);
    buffer.applyHistory("BTC-USD", 60, bars);
    ugr->setCandleBuffer(&buffer);
    auto *view = ugr->getViewState();
    using Request = UnifiedGridRenderer::ViewportRequest;
    ugr->setAutoPriceScale(true);
    Request equal;
    equal.priceMin = view->getMinPrice();
    equal.priceMax = view->getMaxPrice();
    uint64_t v = view->getViewportVersion();
    EXPECT_EQ(ugr->applyViewportRequest(equal), QString());
    EXPECT_FALSE(ugr->autoPriceScale()) << "explicit bounds equal to the current ones still turn it off";
    EXPECT_EQ(view->getViewportVersion(), v);
    // autoScale + followLive: one change at the live edge, fitted.
    Request both;
    both.autoScale = true;
    both.followLive = true;
    v = view->getViewportVersion();
    EXPECT_EQ(ugr->applyViewportRequest(both), QString());
    EXPECT_EQ(view->getViewportVersion() - v, 1u) << "flags resolved first, one commit";
    EXPECT_TRUE(ugr->autoPriceScale());
    EXPECT_TRUE(ugr->autoScrollEnabled());
    const int64_t anchor = layer().liveAnchorMs();
    EXPECT_EQ(view->getVisibleTimeEnd(), anchor + std::max<int64_t>(minute, int64_t(timeSpan() * 0.08)));
    {
        const auto [lo, hi] = expectedFit(bars, view->getVisibleTimeStart(), view->getVisibleTimeEnd());
        EXPECT_NEAR(view->getMinPrice(), lo, 1e-6);
        EXPECT_NEAR(view->getMaxPrice(), hi, 1e-6);
    }
    // Time bounds with autoScale: one change, landed fitted, follow off.
    Request time;
    time.startMs = viewLo;
    time.endMs = viewHi;
    time.autoScale = true;
    v = view->getViewportVersion();
    EXPECT_EQ(ugr->applyViewportRequest(time), QString());
    EXPECT_EQ(view->getViewportVersion() - v, 1u);
    EXPECT_FALSE(ugr->autoScrollEnabled());
    {
        const auto [lo, hi] = expectedFit(bars, viewLo, viewHi);
        EXPECT_NEAR(view->getMinPrice(), lo, 1e-6);
    }
    // A symbol with nothing known (no data, book, trade or candle): fits fail and
    // change nothing; the A toggle still arms (on, waiting for candles).
    Request off;
    off.autoScale = false;
    ugr->applyViewportRequest(off);
    ugr->setActiveSymbol("ETH-USD");
    const double lo0 = view->getMinPrice();
    Request fitPrice;
    fitPrice.fit = "price";
    v = view->getViewportVersion();
    EXPECT_EQ(ugr->applyViewportRequest(fitPrice), QString("fit_unavailable"));
    EXPECT_FALSE(ugr->autoPriceScale()) << "a failed fit leaves the auto price scale off";
    Request reset;
    reset.fit = "default";
    EXPECT_EQ(ugr->applyViewportRequest(reset), QString("fit_unavailable")) << "no live anchor for ETH-USD";
    EXPECT_FALSE(ugr->autoScrollEnabled());
    EXPECT_FALSE(ugr->autoPriceScale());
    EXPECT_EQ(view->getViewportVersion(), v);
    EXPECT_EQ(view->getMinPrice(), lo0);
    ugr->setAutoPriceScale(true);
    EXPECT_TRUE(ugr->autoPriceScale()) << "the toggle arms while there is nothing to fit";
    // The legacy renderer has no auto price scale.
    ugr->setActiveSymbol("BTC-USD");
    ugr->setHeatmapRenderer("legacy");
    Request on;
    on.autoScale = true;
    EXPECT_EQ(ugr->applyViewportRequest(on), QString("auto_scale_unavailable"));
    EXPECT_FALSE(ugr->autoPriceScale());
    ugr->setCandleBuffer(nullptr);
}

// Review item 1: the auto-off carry across price scales and unpriced switches. The
// previous symbol's snapshot (price scale) must not size the new symbol's limits (a
// Manual tick on a $0.00001 symbol would clamp a $2,000 BTC span to $0.000032); a symbol
// that never got a price keeps the earlier symbol's carry; unusable prices and a user
// price action before the price arrives are handled.
TEST_F(UgrGpu, SymbolSwitchCarryAcrossPriceScalesAndUnpricedSwitches) {
    gpuOn();
    auto manual = brightSettings();
    manual.tickMode = heatmap::TickMode::Manual;
    manual.manualTick = 1000; // $10 on BTC: at most $3200 over 320 px
    ugr->setHeatmapChartSettings(manual, true);
    ASSERT_TRUE(settle()) << error.toStdString();
    auto *view = ugr->getViewState();
    ASSERT_DOUBLE_EQ(view->maxPriceSpan(), 3200);
    const int64_t t0 = view->getVisibleTimeStart(), t1 = view->getVisibleTimeEnd();
    auto expectCarried = [&](double mid, const char *what) {
        const double span = mid * 0.02; // 2,000 / 100,000
        EXPECT_NEAR(view->getMaxPrice() - view->getMinPrice(), span, span * 1e-6) << what;
        EXPECT_NEAR(view->getMinPrice(), mid - 0.25 * span, span * 1e-6) << what;
        EXPECT_EQ(view->getVisibleTimeStart(), t0) << what;
        EXPECT_EQ(view->getVisibleTimeEnd(), t1) << what;
    };
    ugr->setViewport(t0, t1, 99'500, 101'500); // auto off; the book a quarter up
    ugr->setLiveBookTop(99'999, 100'001);
    // BTC -> TINY (price scale 1e10, recorded).
    ugr->setActiveSymbol("TINY-USD");
    EXPECT_FALSE(layer().priceScaleCurrent());
    EXPECT_EQ(view->maxPriceSpan(), 0.0) << "TINY's limits are unknown, not BTC's";
    ugr->setLiveBookTop(0, 0);             // unusable tops do not consume the carry
    ugr->setLiveBookTop(std::nan(""), 1);
    ugr->setLiveBookTop(0.0000099, 0.0000101);
    expectCarried(0.00001, "TINY before its snapshot");
    ASSERT_TRUE(pump(30'000, [&] { return layer().priceScaleCurrent() && view->maxPriceSpan() > 0; }))
        << "TINY's snapshot never arrived: current=" << layer().priceScaleCurrent() << " max=" << view->maxPriceSpan()
        << " avail=" << bool(lab::LabData::instance().service().availability("TINY-USD")) << " scale=" << layer().priceScale();
    EXPECT_NEAR(view->maxPriceSpan(), 3.2e-5, 1e-12) << "TINY's own Manual limit";
    expectCarried(0.00001, "TINY after its snapshot");
    // TINY -> ETH (not recorded): the controller publishes ETH snapshots before any ETH
    // availability, still at TINY's price scale; they must not size ETH's limits (a
    // $0.000032 Manual limit would clamp the carried $60).
    ugr->setActiveSymbol("ETH-USD");
    ugr->setLiveBookTop(2'999, 3'001);
    expectCarried(3'000, "ETH before its first snapshot");
    ASSERT_TRUE(pump(30'000, [&] {
        const auto snapshot = layer().snapshot();
        return snapshot && snapshot->symbol == "ETH-USD";
    }));
    ASSERT_TRUE(frames(3)) << error.toStdString(); // its limitsChanged delivered
    EXPECT_FALSE(layer().priceScaleCurrent()) << "ETH's price scale is unknown";
    EXPECT_EQ(view->maxPriceSpan(), 0.0);
    expectCarried(3'000, "ETH after its first snapshot");
    // ETH -> BTC: ETH's snapshot is still the newest; no limit from it clamps BTC.
    ugr->setActiveSymbol("BTC-USD");
    EXPECT_EQ(view->maxPriceSpan(), 0.0);
    ugr->setLiveBookTop(99'999, 100'001);
    expectCarried(100'000, "BTC before its snapshot");
    ASSERT_TRUE(pump(30'000, [&] { return layer().priceScaleCurrent() && view->maxPriceSpan() == 3200; }));
    expectCarried(100'000, "BTC after its snapshot");
    // BTC -> ETH (no price this time; candles at $3,000 held) -> TINY: BTC's carry, not
    // one computed from ETH's candles against BTC-priced bounds.
    CandleSeriesBuffer buffer;
    buffer.applyHistory("ETH-USD", 60, risingBars(t0, t1, minute, 3'000, 0));
    ugr->setCandleBuffer(&buffer);
    ugr->setActiveSymbol("ETH-USD");
    ugr->setActiveSymbol("TINY-USD");
    ugr->setLiveBookTop(0.0000099, 0.0000101);
    expectCarried(0.00001, "TINY after an unpriced ETH");
    // A user price action before the new symbol's price cancels the pending carry.
    ugr->setActiveSymbol("BTC-USD");
    ugr->zoomPriceAt(120, 160, 320);
    ugr->setLiveBookTop(99'999, 100'001);
    EXPECT_GT(std::abs((view->getMaxPrice() - view->getMinPrice()) - 2'000), 100) << "the default seed, no carry";
    EXPECT_NEAR((view->getMaxPrice() + view->getMinPrice()) / 2, 100'000, 1e-6);
    ugr->setCandleBuffer(nullptr);
}

// Review round 3: a fit is all or nothing. Before any snapshot (no book, trade or
// candle: no price) the time part fits but the price part cannot, so "both" changes
// nothing and reports the failure; "time" alone still works.
TEST_F(UgrGpu, FitRequestsAreAllOrNothing) {
    gpuOn();
    auto *view = ugr->getViewState();
    ASSERT_GT(layer().liveAnchorMs(), 0) << "the time part can fit";
    ASSERT_FALSE(layer().snapshot());
    const int64_t t0 = view->getVisibleTimeStart(), t1 = view->getVisibleTimeEnd();
    const uint64_t v = view->getViewportVersion();
    UnifiedGridRenderer::ViewportRequest both;
    both.fit = "both";
    EXPECT_EQ(ugr->applyViewportRequest(both), QString("fit_unavailable"));
    EXPECT_EQ(view->getViewportVersion(), v) << "nothing changed";
    EXPECT_EQ(view->getVisibleTimeStart(), t0);
    EXPECT_EQ(view->getVisibleTimeEnd(), t1);
    EXPECT_FALSE(ugr->autoPriceScale());
    UnifiedGridRenderer::ViewportRequest time;
    time.fit = "time";
    EXPECT_EQ(ugr->applyViewportRequest(time), QString());
    EXPECT_NE(view->getVisibleTimeStart(), t0);
}

// Review round 3: a switch before any snapshot exists. The fallback scale (100) is
// nobody's: the carry stays pending and is applied again from the then current price
// once the new symbol's recorded scale arrives.
TEST_F(UgrGpu, CarryWaitsForTheScaleWhenNoSnapshotExistsYet) {
    gpuOn();
    auto manual = brightSettings();
    manual.tickMode = heatmap::TickMode::Manual;
    manual.manualTick = 1000;
    ugr->setHeatmapChartSettings(manual, true);
    auto *view = ugr->getViewState();
    ASSERT_FALSE(layer().snapshot());
    EXPECT_FALSE(layer().priceScaleCurrent());
    EXPECT_EQ(view->maxPriceSpan(), 0.0) << "no snapshot: the limits are unknown";
    const int64_t t0 = view->getVisibleTimeStart(), t1 = view->getVisibleTimeEnd();
    ugr->setViewport(t0, t1, 99'500, 101'500);
    ugr->setLiveBookTop(99'999, 100'001);
    ugr->setActiveSymbol("TINY-USD");
    ugr->setLiveBookTop(0.0000099, 0.0000101);
    auto expectCarried = [&](double mid, const char *what) {
        const double span = mid * 0.02;
        EXPECT_NEAR(view->getMaxPrice() - view->getMinPrice(), span, span * 1e-6) << what;
        EXPECT_NEAR(view->getMinPrice(), mid - 0.25 * span, span * 1e-6) << what;
    };
    expectCarried(0.00001, "at the first price");
    ugr->setLiveBookTop(0.0000109, 0.0000111); // the price moves before the scale is known
    ASSERT_TRUE(pump(30'000, [&] { return layer().priceScaleCurrent() && view->maxPriceSpan() > 0; }));
    ASSERT_TRUE(frames(3)) << error.toStdString();
    expectCarried(0.000011, "applied again once TINY's scale is known");
}

// Review round 3: a live-only symbol (no recorded availability) never gets a price
// scale. kCarryLiveOnlyWaitMs after its first price the carry is applied from the live
// price and consumed, with no Manual max span.
TEST_F(UgrGpu, LiveOnlySymbolCarryAppliesAfterTheWait) {
    gpuOn();
    auto manual = brightSettings();
    manual.tickMode = heatmap::TickMode::Manual;
    manual.manualTick = 1000;
    ugr->setHeatmapChartSettings(manual, true);
    auto *view = ugr->getViewState();
    const int64_t t0 = view->getVisibleTimeStart(), t1 = view->getVisibleTimeEnd();
    ugr->setViewport(t0, t1, 99'500, 101'500);
    ugr->setLiveBookTop(99'999, 100'001);
    ugr->setActiveSymbol("ETH-USD"); // not recorded
    ugr->setLiveBookTop(2'999, 3'001);
    auto expectCarried = [&](double mid, const char *what) {
        const double span = mid * 0.02;
        EXPECT_NEAR(view->getMaxPrice() - view->getMinPrice(), span, span * 1e-6) << what;
        EXPECT_NEAR(view->getMinPrice(), mid - 0.25 * span, span * 1e-6) << what;
    };
    expectCarried(3'000, "at the first price (pending)");
    ugr->setLiveBookTop(3'099, 3'101);
    auto wait = [&](int ms) {
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    };
    wait(UnifiedGridRenderer::kCarryLiveOnlyWaitMs + 600);
    EXPECT_FALSE(layer().priceScaleCurrent()) << "no recorded scale for ETH-USD";
    expectCarried(3'100, "applied from the live price after the wait");
    EXPECT_EQ(view->maxPriceSpan(), 0.0) << "no Manual max span without a scale";
    // Consumed: later prices do not move the view.
    ugr->setLiveBookTop(3'199, 3'201);
    wait(UnifiedGridRenderer::kCarryLiveOnlyWaitMs + 600);
    expectCarried(3'100, "consumed");
}

// The Now column (the live bucket) keeps its screen x and the bar width stays across
// timeframe and symbol switches (a view that is not following live, Now in view).
TEST_F(UgrGpu, NowColumnStaysPutAcrossTimeframeAndSymbolSwitches) {
    gpuOn();
    const auto *view = ugr->getViewState();
    const int64_t anchor = layer().liveAnchorMs();
    ASSERT_EQ(anchor, epoch + 4 * kHourMs);
    // 40 one-minute bars with the Now centre at 70% of the width.
    const int64_t start = anchor - minute / 2 - 28 * minute;
    ugr->setViewport(start, start + 40 * minute, 99'900, 100'300);
    ASSERT_FALSE(ugr->autoScrollEnabled());
    auto nowX = [&](int64_t tf) {
        const double centre = double((anchor + tf - 1) / tf * tf) - double(tf) / 2;
        return (centre - double(view->getVisibleTimeStart())) / timeSpan() * ugr->width();
    };
    auto barPx = [&](int64_t tf) { return ugr->width() * double(tf) / timeSpan(); };
    const double x0 = nowX(minute), bar0 = barPx(minute);
    EXPECT_NEAR(x0, 0.7 * 640, 1e-6);
    for (const int64_t tf : {5 * minute, 15 * minute, 60 * minute, minute}) {
        ugr->setTimeframe(int(tf));
        EXPECT_NEAR(nowX(tf), x0, 0.5) << "Now column at tf " << tf;
        EXPECT_NEAR(barPx(tf), bar0, 1e-6) << "bar width at tf " << tf;
    }
    const int64_t s0 = view->getVisibleTimeStart(), e0 = view->getVisibleTimeEnd();
    ugr->setActiveSymbol("ETH-USD");
    EXPECT_EQ(view->getVisibleTimeStart(), s0) << "a symbol switch keeps the time window";
    EXPECT_EQ(view->getVisibleTimeEnd(), e0);
    ugr->setActiveSymbol("BTC-USD");
    EXPECT_EQ(view->getVisibleTimeStart(), s0);
}

// The time-axis double-click: the default view (initial span ending one padding past
// the live bucket, follow-live and auto price scale on) in one viewport change, from
// anywhere.
TEST_F(UgrGpu, TimeAxisResetGoesToTheDefaultView) {
    gpuOn(); // history, auto price scale off
    const auto *view = ugr->getViewState();
    const int64_t anchor = layer().liveAnchorMs();
    ASSERT_GT(anchor, 0);
    uint64_t v = view->getViewportVersion();
    ASSERT_TRUE(ugr->resetView());
    EXPECT_EQ(view->getViewportVersion() - v, 1u);
    EXPECT_TRUE(ugr->autoScrollEnabled());
    EXPECT_TRUE(ugr->autoPriceScale());
    const double span = timeSpan();
    EXPECT_EQ(view->getVisibleTimeEnd(), anchor + std::max<int64_t>(minute, int64_t(span * 0.08)));
    // From a different zoom in the future: the same default view.
    ugr->setViewport(epoch + 10 * kHourMs, epoch + 10 * kHourMs + 300 * minute, 99'000, 99'100);
    ASSERT_TRUE(ugr->resetView());
    EXPECT_EQ(timeSpan(), span);
    EXPECT_EQ(view->getVisibleTimeEnd(), anchor + std::max<int64_t>(minute, int64_t(span * 0.08)));
}

// FM-141 (left-over of the auto-fit slice): in gpu mode the candle that starts before
// the view start and ends inside it is drawn (it was dropped: the slice selected by bar
// start, and the filter cut at the view start).
TEST_F(UgrGpu, CandleUnderTheLeftEdgeIsDrawn) {
    ugr->setHeatmapRenderer("gpu");
    ugr->setHeatmapLayerEnabled(false); // black background: only the candles draw
    // 40 columns of 16 px from the middle of a candle: half of the first is visible.
    const int64_t lo = viewLo + minute / 2;
    ugr->setViewport(lo, lo + 40 * minute, 99'900, 100'300);
    CandleSeriesBuffer buffer;
    std::vector<CandleSeriesBuffer::CandleBar> bars;
    for (int64_t t = viewLo; t < lo + 41 * minute; t += minute)
        bars.push_back({t, t + minute, 99'800, 100'400, 99'800, 100'400, 1.0, true, 0, false});
    buffer.applyHistory("BTC-USD", 60, bars);
    ASSERT_TRUE(frames(3)) << error.toStdString(); // the chart publishes its mapping first
    auto *candles = new CandlestickOverlayItem(scene->window()->contentItem());
    candles->setSize(ugr->size());
    candles->setMappingProvider(ugr);
    candles->setCandleBuffer(&buffer);
    candles->setSymbol("BTC-USD");
    candles->setTimeframeSec(60);
    ASSERT_TRUE(frames(5)) << error.toStdString();
    int lit = 0;
    for (int x = 0; x < 4; ++x) lit += image.pixel(x, 160) != qRgb(0, 0, 0);
    EXPECT_EQ(lit, 4) << "the left-edge candle's visible half is drawn";
    int second = 0;
    for (int x = 12; x < 20; ++x) second += image.pixel(x, 160) != qRgb(0, 0, 0);
    EXPECT_GT(second, 0) << "the next candle too (sanity)";
    delete candles;
}

// Review major 5: recording availability only (no book, no trade, no API
// viewport): a time-only view from availability, the price from decoded data.
TEST_F(UgrGpu, ColdStartFromTheRecordingAlone) {
    ugr->setHeatmapRenderer("gpu");
    ASSERT_TRUE(pumpOnRequest(15'000, [&] { return layer().settled(); })) << "never drew: " << error.toStdString();
    EXPECT_GT(ugr->getViewState()->getMinPrice(), 99'000);
    EXPECT_LT(ugr->getViewState()->getMaxPrice(), 101'000);
    EXPECT_GT(ugr->getViewState()->getVisibleTimeEnd(), epoch + 4 * kHourMs) << "at the recording's end";
}

// Review minor 7: heatmap gamma/contrast/floor reach the GPU palette.
TEST_F(UgrGpu, ToneChangesReachTheGpuPicture) {
    gpuOn();
    ASSERT_TRUE(settle()) << error.toStdString();
    ASSERT_TRUE(frames(2)) << error.toStdString();
    const QImage before = image;
    ugr->setHeatmapContrast(3.0);
    ugr->setHeatmapGamma(0.5);
    ASSERT_TRUE(frames(3)) << error.toStdString();
    int changed = 0;
    for (int y = 2; y < 320; y += 4)
        for (int x = 2; x < 640; x += 4) changed += image.pixel(x, y) != before.pixel(x, y);
    EXPECT_GT(changed, 1000) << "the tone mapping changed the picture";
}

// Owner decision 4: the legacy renderer draws the chart preset from the same
// palette image with heatmap_intensity.frag; its colour for a recording code is
// the reference HeatmapTileNode matches (PresetPaletteMatchesTheLegacyColourForEachCode).
class LegacyHost : public QQuickItem {
public:
    LegacyHost() { setFlag(ItemHasContents, true); }
    HeatmapOverlayRenderer overlay;
    std::vector<uint16_t> codes; // one column of rows, top first
    float lo = 0, hi = 1;
    QSGNode *updatePaintNode(QSGNode *old, UpdatePaintNodeData *) override {
        auto *node = old ? static_cast<HeatmapIntensityNode *>(old) : new HeatmapIntensityNode();
        overlay.setGridDimensions(1, int(codes.size()));
        overlay.setIntensityBytesPerCell(2);
        std::vector<HeatmapOverlayRenderer::PendingUpload> uploads;
        QByteArray column(int(codes.size()) * 2, 0);
        std::memcpy(column.data(), codes.data(), size_t(column.size()));
        uploads.push_back({0, column});
        const QRectF r(0, 0, width(), height());
        overlay.applyToNode(window(), node, true, 1.05f, 1.15f, 0.01f, true, 0.0f, r, QRectF(0, 0, 1, codes.size()),
                            uploads);
        node->setValueMode(true, lo, hi);
        node->setRowGrouping(1, 0);
        return node;
    }
};
TEST(UgrPalette, LegacyRendererDrawsThePresetAsTheSharedReference) {
    if (const QString why = lab::gpuUnavailableReason(); !why.isEmpty()) GTEST_SKIP() << "GPU case skipped: " << why.toStdString();
    lab::OffscreenQuick quick;
    QString error;
    ASSERT_TRUE(quick.create(QSize(40, 160), &error)) << error.toStdString();
    quick.window()->setColor(Qt::black);
    auto *host = new LegacyHost;
    host->setParentItem(quick.window()->contentItem());
    host->setSize(QSizeF(40, 160));
    const auto window = heatmap::gpu::codeWindow(0.01, 5);
    host->lo = window.floor;
    host->hi = window.floor + window.range;
    // Eight rows: bid and ask codes across the window.
    for (const double size : {0.02, 0.2, 1.0, 4.0})
        for (const bool ask : {false, true}) host->codes.push_back(uint16_t(recording::encodeSize(size) | (ask ? 0x8000 : 0)));
    for (const char *preset : {"Fire", "Matrix"}) {
        const auto gradients = *heatmap::gpu::presetGradients(preset);
        host->overlay.setBidGradient(HeatmapOverlayRenderer::toColorStops(gradients.bid));
        host->overlay.setAskGradient(HeatmapOverlayRenderer::toColorStops(gradients.ask));
        host->overlay.setPaletteGamma(gradients.gamma);
        host->update();
        QImage image;
        for (int i = 0; i < 3; ++i) {
            host->update();
            image = quick.renderFrame(&error);
            ASSERT_FALSE(image.isNull()) << error.toStdString();
        }
        const auto palette = heatmap::gpu::makePalette(gradients, {1.05f, 1.15f, 0.01f});
        for (size_t row = 0; row < host->codes.size(); ++row) {
            const auto want = heatmap::gpu::legacyRecordingColor(host->codes[row], window, *palette);
            const QColor got = image.pixelColor(20, int(row * 20 + 10));
            EXPECT_NEAR(got.red(), want[0] * 255, 3) << preset << " row " << row;
            EXPECT_NEAR(got.green(), want[1] * 255, 3) << preset << " row " << row;
            EXPECT_NEAR(got.blue(), want[2] * 255, 3) << preset << " row " << row;
        }
    }
    delete host;
    quick.renderFrame(&error);
}

// The S6a input route on the real chart QML (DepthChartView.qml): a synthesized
// wheel reaches UnifiedGridRenderer and changes the viewport exactly once; the
// price axis WheelHandler zooms price only; a time-axis MouseArea drag obeys the
// one-column-per-pixel clamp (spec rule 9).
TEST(UgrInput, InputRouteReachesTheChartAndAxisAreasOncePerWheel) {
    QQuickView view;
    view.setResizeMode(QQuickView::SizeRootObjectToView);
    view.rootContext()->setContextProperty("uiTheme", nullptr);
    view.rootContext()->setContextProperty("dataSource", nullptr);
    view.rootContext()->setContextProperty("chartModeController", nullptr);
    view.setSource(QUrl::fromLocalFile(QStringLiteral(SENTINEL_SOURCE_DIR "/libs/gui/qml/DepthChartView.qml")));
    ASSERT_EQ(view.status(), QQuickView::Ready) << view.errors().value(0).toString().toStdString();
    view.resize(1000, 600);
    view.rootObject()->setSize(QSizeF(1000, 600)); // the view is never exposed: size the root directly
    QCoreApplication::processEvents();
    auto *ugr = view.rootObject()->findChild<UnifiedGridRenderer *>("unifiedGridRenderer");
    ASSERT_TRUE(ugr);
    ugr->setHeatmapService(&lab::LabData::instance().service());
    ugr->setActiveSymbol("BTC-USD");
    ugr->setTimeframe(int(minute));
    ugr->setHeatmapRenderer("gpu");
    const int64_t lo = epoch + kHourMs;
    ugr->setViewport(lo, lo + 60 * minute, 99'900, 100'300);
    QCoreApplication::processEvents();
    int changes = 0;
    QObject::connect(ugr->getViewState(), &GridViewState::viewportChanged, ugr, [&] { ++changes; });
    AgentApi::InputDispatcher input;
    const QRectF chart = ugr->mapRectToScene(ugr->boundingRect());
    ASSERT_GT(chart.width(), 500);
    auto apply = [&](const char *kind, const char *target, double x, double y, int delta = 0) {
        AgentApi::InputCommand c{kind, target, x, y, delta, {}};
        return input.apply(&view, chart, c).status;
    };
    auto *state = ugr->getViewState();
    auto timeSpan = [&] { return double(state->getVisibleTimeEnd() - state->getVisibleTimeStart()); };
    auto priceSpan = [&] { return state->getMaxPrice() - state->getMinPrice(); };
    const double time0 = timeSpan(), price0 = priceSpan();
    ASSERT_EQ(apply("wheel", "chart", 300, 200, 120), 200);
    EXPECT_EQ(changes, 1) << "one wheel, one viewport change";
    EXPECT_LT(timeSpan(), time0);
    EXPECT_LT(priceSpan(), price0);
    const double time1 = timeSpan();
    ASSERT_EQ(apply("wheel", "priceAxis", 20, 200, 120), 200);
    EXPECT_EQ(changes, 2) << "the price axis WheelHandler: one change";
    EXPECT_EQ(timeSpan(), time1) << "price only";
    // Time axis drag outward far past the clamp (MouseArea zoomTimeAt).
    ASSERT_EQ(apply("dragStart", "timeAxis", 500, 10), 200);
    for (int x = 480; x > 0; x -= 20) ASSERT_EQ(apply("dragMove", "timeAxis", x, 10), 200);
    ASSERT_EQ(apply("dragEnd", "timeAxis", 20, 10), 200);
    EXPECT_GT(changes, 2);
    const double dpr = view.effectiveDevicePixelRatio();
    EXPECT_DOUBLE_EQ(timeSpan(), std::floor(ugr->width() * dpr * double(minute))) << "spec rule 9: the axis drag clamps";
    const double timeSpanBefore = timeSpan();
    // A double-click on either axis (QML MouseArea onDoubleClicked) is one viewport
    // change. Price: auto price scale on, the visible candles fitted.
    CandleSeriesBuffer buffer;
    std::vector<CandleSeriesBuffer::CandleBar> bars;
    for (int64_t t = epoch; t < epoch + 4 * kHourMs; t += minute)
        bars.push_back({t, t + minute, 101'000, 101'200, 100'800, 101'000, 1.0, true, 0, false});
    buffer.applyHistory("BTC-USD", 60, bars);
    ugr->setCandleBuffer(&buffer);
    ASSERT_FALSE(ugr->autoPriceScale()) << "the axis zooms turned it off";
    int before = changes;
    ASSERT_EQ(apply("doubleClick", "priceAxis", 20, 200), 200);
    EXPECT_EQ(changes - before, 1) << "price axis double-click: one change";
    EXPECT_TRUE(ugr->autoPriceScale()) << "the price-axis double-click turns it on";
    EXPECT_NEAR(state->getMinPrice(), 100'800 - 400 * UnifiedGridRenderer::kFitPriceMargin, 1e-6);
    EXPECT_NEAR(state->getMaxPrice(), 101'200 + 400 * UnifiedGridRenderer::kFitPriceMargin, 1e-6);
    // A price-axis drag turns it off.
    ASSERT_EQ(apply("dragStart", "priceAxis", 20, 200), 200);
    ASSERT_EQ(apply("dragMove", "priceAxis", 20, 260), 200);
    ASSERT_EQ(apply("dragEnd", "priceAxis", 20, 260), 200);
    EXPECT_FALSE(ugr->autoPriceScale()) << "a price-axis drag turns it off";
    // The "A" toggle in the axis corner: a click turns it on (fitted), another off.
    auto *button = view.rootObject()->findChild<QQuickItem *>("autoPriceScaleButton");
    ASSERT_TRUE(button);
    EXPECT_TRUE(button->isVisible()) << "shown with the gpu renderer";
    auto click = [&] {
        const QPointF at = button->mapToScene(QPointF(button->width() / 2, button->height() / 2));
        for (auto type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
            QMouseEvent event(type, at, at, view.mapToGlobal(at), Qt::LeftButton,
                              type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton, Qt::NoModifier);
            QCoreApplication::sendEvent(&view, &event);
        }
    };
    before = changes;
    click();
    EXPECT_TRUE(ugr->autoPriceScale()) << "the A toggle turns it on";
    EXPECT_EQ(changes - before, 1);
    EXPECT_NEAR(state->getMinPrice(), 100'800 - 400 * UnifiedGridRenderer::kFitPriceMargin, 1e-6);
    QElapsedTimer pause; // not a double-click
    pause.start();
    while (pause.elapsed() < 600) QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    click();
    EXPECT_FALSE(ugr->autoPriceScale()) << "and off";
    // Time: the default view (follow-live and auto price scale on).
    QElapsedTimer waited; // the reset needs the recording's availability (data thread)
    waited.start();
    while (ugr->gpuHeatmapLayer()->liveAnchorMs() <= 0 && waited.elapsed() < 10'000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    ASSERT_GT(ugr->gpuHeatmapLayer()->liveAnchorMs(), 0);
    before = changes;
    ASSERT_EQ(apply("doubleClick", "timeAxis", 500, 10), 200);
    EXPECT_EQ(changes - before, 1) << "time axis double-click: one change";
    EXPECT_TRUE(ugr->autoScrollEnabled());
    EXPECT_TRUE(ugr->autoPriceScale());
    const double span = double(state->getVisibleTimeEnd() - state->getVisibleTimeStart());
    EXPECT_EQ(state->getVisibleTimeEnd(),
              epoch + 4 * kHourMs + std::max<int64_t>(minute, int64_t(span * 0.08))) << "the live edge, padded";
    EXPECT_LT(span, 0.5 * timeSpanBefore) << "the default span, not the clamped zoom-out";
    ugr->setCandleBuffer(nullptr);
    ugr->setHeatmapRenderer("legacy"); // its controller goes now
}
// Review item 7 (pre-existing): DepthChartView's root `symbol` property shadowed the
// context property QmlSceneController set, so the candle overlay and the paper-trade
// model stayed on BTC-USD after a symbol switch.
TEST(UgrInput, CandlesFollowTheActiveSymbol) {
    QQuickView view;
    view.rootContext()->setContextProperty("uiTheme", nullptr);
    view.rootContext()->setContextProperty("dataSource", nullptr);
    view.rootContext()->setContextProperty("chartModeController", nullptr);
    view.setSource(QUrl::fromLocalFile(QStringLiteral(SENTINEL_SOURCE_DIR "/libs/gui/qml/DepthChartView.qml")));
    ASSERT_EQ(view.status(), QQuickView::Ready) << view.errors().value(0).toString().toStdString();
    auto *candles = view.rootObject()->findChild<CandlestickOverlayItem *>();
    auto *paper = view.rootObject()->findChild<PaperTradeOverlayModel *>("paperTradeOverlayModel");
    ASSERT_TRUE(candles);
    ASSERT_TRUE(paper);
    QmlSceneController scene(&view);
    scene.updateSymbolInContext("ETH-USD");
    EXPECT_EQ(candles->symbol(), QString("ETH-USD")) << "the candles follow the active symbol";
    EXPECT_EQ(paper->symbol(), QString("ETH-USD"));
    scene.updateSymbolInContext("BTC-USD");
    EXPECT_EQ(candles->symbol(), QString("BTC-USD"));
}

// A std::runtime_error from a chunk build or an availability scan must become a
// failed request (the fetcher retries it), never leave the transport's Qt slot.
// This executable used to link vcpkg's libskia.a, whose private `typeinfo for
// std::exception` made `catch (const std::exception &)` miss a libc++-thrown
// std::runtime_error: before the catch (...) fix this test aborted in
// std::terminate (FM-145). skia is gone now (tests/link guards that); the test
// still pins the transport's own no-throw-out-of-slot behaviour.
TEST(LocalChunkTransportFaults, ARuntimeErrorBecomesAFailedReplyAndTheWorkerSurvives) {
    ASSERT_NE(fixtureDir, nullptr);
    std::atomic<int> builds{0}, scans{0};
    heatmap::LocalChunkTransport::TestHooks hooks;
    hooks.pollIntervalMs = 0;
    hooks.beforeAvailability = [&](recording::Hmc2Reader &, std::stop_token) {
        if (++scans == 2) throw std::runtime_error("scripted availability failure");
    };
    hooks.beforeBuild = [&](recording::Hmc2Reader &, std::stop_token) {
        if (++builds == 1) throw std::runtime_error("scripted build failure");
    };
    heatmap::LocalChunkTransport local(fixtureDir->path().toStdString(), hooks);
    QEventLoop loop;
    std::optional<heatmap::ChunkAvailability> available;
    std::vector<quint64> received;
    std::vector<std::pair<quint64, QString>> failures;
    int availabilityCount = 0;
    QObject::connect(&local, &heatmap::ChunkTransport::availability, &loop, [&](heatmap::ChunkAvailability a) {
        available = std::move(a); ++availabilityCount; loop.quit();
    }, Qt::QueuedConnection);
    QObject::connect(&local, &heatmap::ChunkTransport::received, &loop, [&](quint64 id, heatmap::ChunkFramePtr) {
        received.push_back(id); loop.quit();
    }, Qt::QueuedConnection);
    QObject::connect(&local, &heatmap::ChunkTransport::failed, &loop,
                     [&](quint64 id, heatmap::OptionalChunkKey, QString code, QString) {
        failures.emplace_back(id, code); loop.quit();
    }, Qt::QueuedConnection);
    auto await = [&](const std::function<bool()> &done) {
        QTimer watchdog;
        watchdog.setSingleShot(true);
        QObject::connect(&watchdog, &QTimer::timeout, &loop, &QEventLoop::quit);
        watchdog.start(10'000);
        while (!done() && watchdog.isActive()) loop.exec();
        return done();
    };
    local.start({lab::kSymbol});
    ASSERT_TRUE(await([&] { return available.has_value(); }));
    ASSERT_FALSE(available->sources.empty());
    const std::string source = available->sources.front().id;

    // The first build throws: an error reply for that request, and the process lives.
    const auto bad = local.request(lab::kSymbol, source, minute, {epoch}, {});
    ASSERT_TRUE(await([&] { return !failures.empty(); }));
    EXPECT_EQ(failures.front().first, bad);
    EXPECT_EQ(failures.front().second, QStringLiteral("build_failed"));
    EXPECT_TRUE(received.empty());

    // The worker is still serving: a later request for a valid key succeeds.
    const auto good = local.request(lab::kSymbol, source, minute, {epoch}, {});
    ASSERT_TRUE(await([&] { return !received.empty(); }));
    EXPECT_EQ(received.front(), good);

    // A throwing availability scan keeps the last snapshot (it has no request to fail)
    // and does not stop the worker: the next request is still answered.
    local.refreshAvailability(lab::kSymbol); // scan 2 throws
    const auto after = local.request(lab::kSymbol, source, minute, {epoch + kHourMs}, {});
    ASSERT_TRUE(await([&] { return received.size() == 2; }));
    EXPECT_EQ(received.back(), after);
    EXPECT_EQ(availabilityCount, 1);
    EXPECT_EQ(failures.size(), 1u);
}
} // namespace

int main(int argc, char **argv) {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    Q_INIT_RESOURCE(sentinel_ui_fonts);
    qmlRegisterModule("Sentinel", 1, 0);
    qmlRegisterType<UnifiedGridRenderer>("Sentinel", 1, 0, "UnifiedGridRenderer");
    qmlRegisterType<CoordinateSystem>("Sentinel", 1, 0, "CoordinateSystem");
    qmlRegisterType<TimeAxisModel>("Sentinel", 1, 0, "TimeAxisModel");
    qmlRegisterType<PriceAxisModel>("Sentinel", 1, 0, "PriceAxisModel");
    qmlRegisterType<AlgoOverlayRenderer>("Sentinel", 1, 0, "AlgoOverlayRenderer");
    qmlRegisterType<PaperTradeOverlayModel>("Sentinel", 1, 0, "PaperTradeOverlayModel");
    qmlRegisterType<PaperTradeOverlayRenderer>("Sentinel", 1, 0, "PaperTradeOverlayRenderer");
    qmlRegisterModule("Sentinel.Charts", 1, 0);
    qmlRegisterType<LabTextItem>("Sentinel.Charts", 1, 0, "LabTextItem");
    qmlRegisterType<CandlestickBatched>("Sentinel.Charts", 1, 0, "CandlestickBatched");
    qmlRegisterType<CandlestickOverlayItem>("Sentinel.Charts", 1, 0, "CandlestickOverlayItem");
    ::testing::InitGoogleTest(&argc, argv);
    std::cout << "[sentinel] " << lab::describeRhi().toStdString() << std::endl;
    configureFixture();
    return RUN_ALL_TESTS();
}
