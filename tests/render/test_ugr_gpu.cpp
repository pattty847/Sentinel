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
using heatmap::kHourMs;
using heatmap::kMinuteMs;
constexpr int64_t minute = kMinuteMs;
constexpr int64_t epoch = recording::kHmc2MinMs + 50 * heatmap::kDayMs;

// Minute i of layer "deep" ($10 rows, a wide book) or "near" ($1 rows, a band):
// the fixture of LabItemTest.
recording::Hmc2Record syntheticMinute(const std::string &layer, int64_t i) {
    const bool deep = layer == "deep";
    recording::Hmc2Record r;
    r.header = {"BTC-USD", layer, minute, 100, deep ? 1000 : 100, {}, deep ? 77u : 78u};
    r.bucketStartMs = epoch + i * minute;
    r.observedMs = uint32_t(minute);
    const int64_t mid = (100'000 + (i % 30) * 10) * 100 / r.header.rowTickUnits;
    const int64_t half = deep ? 40 : 200;
    r.bidRowLo = r.askRowLo = mid - half;
    r.bidRowHi = r.askRowHi = mid + half - 1;
    r.midOpen = r.midClose = r.midMin = r.midMax = double(mid) * double(r.header.rowTickUnits) / 100;
    for (int64_t row = mid - half; row < mid + half; ++row) {
        const bool ask = row >= mid;
        const auto code = recording::encodeSize(0.01 * double(1 + (row * 7 + i) % 53), r.header.sizeScale);
        r.entries.push_back({row, ask, code, code, r.observedMs});
    }
    return r;
}
void writeRecording(const QTemporaryDir &dir, int64_t minutes) {
    recording::Hmc2Store writer(dir.path().toStdString());
    for (int64_t i = 0; i < minutes; ++i)
        for (const char *layer : {"deep", "near"}) writer.append(syntheticMinute(layer, i));
}

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
        const bool ok = pump(30'000, [&] { streak = layer().settled() ? streak + 1 : 0; return streak >= 3; });
        if (!ok && error.isEmpty()) error = QStringLiteral("did not settle");
        return ok;
    }
    // Fraction of the view's width the current picture draws from content (span
    // or live bins, not loading) in the last frame.
    double coverage() const {
        const auto mapping = ugr->currentTimeAxisMapping();
        const double lo = mapping.viewStartMs, hi = mapping.viewEndMs;
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
    ugr->setTimeframe(int(5 * minute));
    ASSERT_TRUE(pump(30'000, [&] { return layer().settled(); }, [&] {
        ++frames;
        worst = std::min(worst, coverage());
        fewestPainted = std::min(fewestPainted, painted());
    })) << error.toStdString();
    EXPECT_GE(worst, before - 1e-9) << "a frame of the switch drew less of the view";
    EXPECT_GE(fewestPainted, paintedBefore / 2) << "a frame of the switch blanked the chart";
    EXPECT_EQ(layer().tileStats().drawnTfMs.load(), 5 * minute);
    // And back to 1m (recent-tf): the same.
    worst = 1;
    ugr->setTimeframe(int(minute));
    ASSERT_TRUE(pump(30'000, [&] { return layer().settled(); }, [&] { worst = std::min(worst, coverage()); }))
        << error.toStdString();
    EXPECT_GE(worst, before - 1e-9);
    std::cout << "[ugr] tf switch frames=" << frames << std::endl;
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
    EXPECT_GT(rendered, 3) << "budgeted uploads took several frames";
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
