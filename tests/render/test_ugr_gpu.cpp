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
#include <QGuiApplication>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickView>
#include <QQuickWindow>
#include <QSGOpacityNode>
#include <QTemporaryDir>
#include <QtQml/qqml.h>
#include <gtest/gtest.h>
#include <private/qquickitem_p.h>
#include <climits>
#include <cmath>
#include <iostream>

namespace {
using namespace synthetic_hmc2;
QSGNode *paintRoot(QQuickItem *item) { return QQuickItemPrivate::get(item)->paintNode; }

// One synthetic recording (4 h) for the whole binary: the data path is configured
// once in main() and never reconfigured (a reconfigure with local chunk reads in
// flight is lab-only teardown territory, not the main chart's).
QTemporaryDir *fixtureDir = nullptr;
void configureFixture() {
    fixtureDir = new QTemporaryDir;
    writeRecording(*fixtureDir, 4 * 60);
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
    Trade trade;
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

// Manual tick ($10 over 320 px: at most $3200) cannot hold candles spanning $5000:
// the fit takes the max span, as close to their middle as it can with the newest
// close (where price is now) inside the margin (live A/B 2026-10-02: 5m fit missed it).
TEST_F(UgrGpu, PriceAxisFitUnderTheManualClampKeepsTheNewestCloseInView) {
    gpuOn();
    auto manual = brightSettings();
    manual.tickMode = heatmap::TickMode::Manual;
    manual.manualTick = 1000;
    ugr->setHeatmapChartSettings(manual, true);
    const auto *view = ugr->getViewState();
    ASSERT_DOUBLE_EQ(view->maxPriceSpan(), 3200);
    CandleSeriesBuffer buffer;
    std::vector<CandleSeriesBuffer::CandleBar> bars;
    for (int i = 0; i < 40; ++i) { // 100,000 rising to 105,000
        const int64_t t = viewLo + i * minute;
        const double close = 100'000 + 5000.0 * (i + 1) / 40;
        bars.push_back({t, t + minute, close - 125, close, close - 125, close, 1.0, true, 0, false});
    }
    buffer.applyHistory("BTC-USD", 60, bars);
    ugr->setCandleBuffer(&buffer);
    ASSERT_TRUE(ugr->fitPriceToData());
    EXPECT_DOUBLE_EQ(view->getMaxPrice() - view->getMinPrice(), 3200);
    EXPECT_NEAR(view->getMaxPrice(), 105'000 + 3200 * UnifiedGridRenderer::kFitPriceMargin, 1e-6)
        << "the newest close one margin below the top";
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

// Owner decision: after a timeframe switch the price fits the new timeframe's
// candles. Held already: in the switch's single viewport change. Not held yet: the
// switch keeps the price and the fit lands when they arrive (one more change).
TEST_F(UgrGpu, TimeframeSwitchFitsPriceToTheCandles) {
    gpuOn(); // 40 one-minute columns ending at viewHi
    const auto *view = ugr->getViewState();
    const int64_t tf5 = 5 * minute;
    auto fiveMinuteBars = [&](double base) {
        std::vector<CandleSeriesBuffer::CandleBar> bars;
        for (int64_t t = viewHi - 40 * tf5; t < viewHi; t += tf5) bars.push_back({t, t + tf5, base, base + 100, base - 100, base, 1.0, true, 0, false});
        return bars;
    };
    CandleSeriesBuffer buffer;
    buffer.applyHistory("BTC-USD", 300, fiveMinuteBars(105'000));
    ugr->setCandleBuffer(&buffer);
    uint64_t before = view->getViewportVersion();
    ugr->setTimeframe(int(tf5));
    EXPECT_EQ(view->getViewportVersion() - before, 1u) << "scaled span and fitted price: one change";
    EXPECT_EQ(view->getVisibleTimeEnd() - view->getVisibleTimeStart(), 40 * tf5);
    EXPECT_LE(view->getMinPrice(), 104'900);
    EXPECT_GE(view->getMaxPrice(), 105'100);
    EXPECT_LT(view->getMaxPrice() - view->getMinPrice(), 300) << "fitted, not just widened";
    EXPECT_FALSE(ugr->priceFitPending());
    // 15m candles are not held yet: the switch keeps the price, the fit waits for them.
    const double p0 = view->getMinPrice(), p1 = view->getMaxPrice();
    before = view->getViewportVersion();
    ugr->setTimeframe(int(15 * minute));
    EXPECT_EQ(view->getViewportVersion() - before, 1u);
    EXPECT_EQ(view->getMinPrice(), p0);
    EXPECT_EQ(view->getMaxPrice(), p1);
    EXPECT_TRUE(ugr->priceFitPending());
    // A forming live candle alone is not the history: still pending.
    const int64_t tf15 = 15 * minute, end = view->getVisibleTimeEnd(), start = view->getVisibleTimeStart();
    CandleSeriesBuffer::CandleBar live{(end / tf15 - 1) * tf15, end / tf15 * tf15, 98'000, 98'050, 97'950, 98'000, 1, false, 0, false};
    buffer.applyUpdate("BTC-USD", 900, live, 1, false);
    EXPECT_TRUE(ugr->priceFitPending());
    std::vector<CandleSeriesBuffer::CandleBar> bars;
    for (int64_t t = (start / tf15) * tf15; t < end; t += tf15) bars.push_back({t, t + tf15, 98'000, 98'200, 97'800, 98'000, 1.0, true, 0, false});
    before = view->getViewportVersion();
    buffer.applyHistory("BTC-USD", 900, bars);
    EXPECT_FALSE(ugr->priceFitPending());
    EXPECT_EQ(view->getViewportVersion() - before, 1u) << "the deferred fit is one change";
    EXPECT_LE(view->getMinPrice(), 97'800);
    EXPECT_GE(view->getMaxPrice(), 98'200);
    EXPECT_LT(view->getMaxPrice() - view->getMinPrice(), 600);
    ugr->setCandleBuffer(nullptr);
}

// Live 2026-10-02: 1h history arrives in pages of five bars every ~150 ms, newest
// first; a fixed 2 s wait fitted the first pages only. While pages keep coming the
// fit waits (quiet period), then uses every visible candle, including the oldest page.
TEST_F(UgrGpu, TimeframeSwitchFitWaitsForHistoryPaging) {
    gpuOn(); // 40 columns: 200 minutes of 5m after the switch
    CandleSeriesBuffer buffer;
    ugr->setCandleBuffer(&buffer);
    const auto *view = ugr->getViewState();
    const int64_t tf5 = 5 * minute;
    ugr->setTimeframe(int(tf5));
    ASSERT_TRUE(ugr->priceFitPending());
    const int64_t newest = (view->getVisibleTimeEnd() - 1) / tf5 * tf5;
    // 8 pages of 4 bars (32 of 40 buckets: under the coverage bar), 400 ms apart: 3.2 s.
    for (int page = 0; page < 8; ++page) {
        std::vector<CandleSeriesBuffer::CandleBar> bars;
        for (int i = 0; i < 4; ++i) {
            const int64_t t = newest - int64_t(page * 4 + i) * tf5;
            const double low = page == 7 ? 95'000 : 100'000; // the oldest page holds the low
            bars.push_back({t, t + tf5, 100'100, 100'200, low, 100'150, 1.0, true, 0, false});
        }
        buffer.applyHistory("BTC-USD", 300, bars);
        EXPECT_TRUE(ugr->priceFitPending()) << "page " << page << ": more history is coming";
        QElapsedTimer gap;
        gap.start();
        while (gap.elapsed() < 400) QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    QElapsedTimer timer;
    timer.start();
    while (ugr->priceFitPending() && timer.elapsed() < 3000) QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    EXPECT_FALSE(ugr->priceFitPending());
    EXPECT_LE(view->getMinPrice(), 95'000) << "the oldest page's low is in view";
    EXPECT_GE(view->getMaxPrice(), 100'200);
    ugr->setCandleBuffer(nullptr);
}

// No candle for the new timeframe ever arrives: the fit falls back to the live price
// after kPriceFitFirstWaitMs (the view must not stay where nothing is visible).
TEST_F(UgrGpu, TimeframeSwitchWithoutCandlesFitsTheLivePrice) {
    gpuOn();
    CandleSeriesBuffer buffer; // empty
    ugr->setCandleBuffer(&buffer);
    ugr->setLiveBookTop(109'999, 110'001);
    const auto *view = ugr->getViewState();
    const double span = priceSpan();
    ugr->setTimeframe(int(5 * minute));
    ASSERT_TRUE(ugr->priceFitPending());
    QElapsedTimer timer;
    timer.start();
    while (ugr->priceFitPending() && timer.elapsed() < UnifiedGridRenderer::kPriceFitFirstWaitMs + 2000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    EXPECT_FALSE(ugr->priceFitPending());
    EXPECT_NEAR(view->getMinPrice(), 110'000 - span / 2, 1e-6);
    EXPECT_NEAR(view->getMaxPrice(), 110'000 + span / 2, 1e-6);
    // A user price interaction cancels a pending fit.
    ugr->setTimeframe(int(15 * minute));
    ASSERT_TRUE(ugr->priceFitPending());
    ugr->zoomPriceAt(120, 100, 320);
    EXPECT_FALSE(ugr->priceFitPending());
    ugr->setCandleBuffer(nullptr);
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
    // Auto-fit: a double-click on either axis (QML MouseArea onDoubleClicked) is one
    // viewport change. Price: the visible candles; time: the 4 h recording.
    CandleSeriesBuffer buffer;
    std::vector<CandleSeriesBuffer::CandleBar> bars;
    for (int64_t t = state->getVisibleTimeStart() / minute * minute; t < state->getVisibleTimeEnd(); t += minute)
        bars.push_back({t, t + minute, 101'000, 101'200, 100'800, 101'000, 1.0, true, 0, false});
    buffer.applyHistory("BTC-USD", 60, bars);
    ugr->setCandleBuffer(&buffer);
    int before = changes;
    ASSERT_EQ(apply("doubleClick", "priceAxis", 20, 200), 200);
    EXPECT_EQ(changes - before, 1) << "price axis double-click: one change";
    EXPECT_NEAR(state->getMinPrice(), 100'800 - 400 * UnifiedGridRenderer::kFitPriceMargin, 1e-6);
    EXPECT_NEAR(state->getMaxPrice(), 101'200 + 400 * UnifiedGridRenderer::kFitPriceMargin, 1e-6);
    const double fittedLo = state->getMinPrice();
    QElapsedTimer waited; // the time fit needs the recording's availability (data thread)
    waited.start();
    while (ugr->gpuHeatmapLayer()->liveAnchorMs() <= 0 && waited.elapsed() < 10'000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    ASSERT_GT(ugr->gpuHeatmapLayer()->liveAnchorMs(), 0);
    before = changes;
    ASSERT_EQ(apply("doubleClick", "timeAxis", 500, 10), 200);
    EXPECT_EQ(changes - before, 1) << "time axis double-click: one change";
    EXPECT_NEAR(double(state->getVisibleTimeStart()), double(epoch), double(2 * minute)) << "the recording's start";
    EXPECT_GT(state->getVisibleTimeEnd(), epoch + 4 * kHourMs) << "and its live end";
    EXPECT_EQ(state->getMinPrice(), fittedLo) << "time only";
    ugr->setCandleBuffer(nullptr);
    ugr->setHeatmapRenderer("legacy"); // its controller goes now
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
