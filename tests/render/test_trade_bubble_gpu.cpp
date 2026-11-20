// Trade bubbles through the real Qt Quick scene graph (QSGBatchRenderer on the selected
// QRhi backend, lab::OffscreenQuick): what the bubble node draws after its mesh was empty,
// and the renderer's footprint-number dim wiring. GPU cases skip with the reason when no
// backend can be created: a skipped case is no result.
#include "UnifiedGridRenderer.h"
#include "GoldenFlowSource.hpp"
#include "SyntheticHmc2Fixture.hpp"
#include "chart/feeds/TradeBubbleFeed.hpp"
#include "chart/layers/ChartStack.hpp"
#include "lab/LabData.hpp"
#include "lab/OffscreenQuick.hpp"
#include "lab/RhiBackend.hpp"
#include "mainwindow/ChartAgentPort.hpp"
#include "render/TradeBubbleOverlayItem.hpp"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QGuiApplication>
#include <QQuickWindow>
#include <QSettings>
#include <QTemporaryDir>
#include <gtest/gtest.h>
#include <iostream>
#include <memory>

// Reaches the renderer's footprint layer (UnifiedGridRenderer befriends this name).
struct TradeBubbleRendererTest {
    static const chart::FootprintCellsLayer& footprint(const UnifiedGridRenderer& ugr) { return ugr.m_layers.footprintCells; }
};

namespace {
using namespace synthetic_hmc2;
const int64_t viewLo = epoch + 2 * kHourMs;

// A bubble provider with no chart: the frame the renderer would publish, held directly.
class FrameSource final : public QObject, public chart::ITradeBubbleFrameSource {
public:
    std::shared_ptr<trade_bubbles::Tape> tape = std::make_shared<trade_bubbles::Tape>();
    std::shared_ptr<trade_bubbles::RenderFrame> frame = std::make_shared<trade_bubbles::RenderFrame>(tape);
    std::shared_ptr<trade_bubbles::RenderFrame> tradeBubbleRenderFrame() const override { return frame; }
};

TimeAxisMapping mapping() {
    TimeAxisMapping m;
    m.valid = true;
    m.viewStartMs = m.dataStartMs = 1000;
    m.viewEndMs = 2000;
    m.viewMinPrice = m.dataMinPrice = 100;
    m.viewMaxPrice = m.dataMaxPrice = 200;
    m.appendMs = 100;
    m.tickSize = 1;
    m.drawRect = {0, 0, 320, 160};
    m.srcRect = {0, 0, 10, 100};
    return m;
}

class BubbleScene : public testing::Test {
protected:
    std::unique_ptr<lab::OffscreenQuick> scene;
    FrameSource source;
    TradeBubbleOverlayItem* bubbles = nullptr;
    QString error;
    QImage image;
    void SetUp() override {
        if (const QString why = lab::gpuUnavailableReason(); !why.isEmpty()) GTEST_SKIP() << "GPU case skipped: " << why.toStdString();
        scene = std::make_unique<lab::OffscreenQuick>();
        ASSERT_TRUE(scene->create(QSize(320, 160), &error)) << error.toStdString();
        scene->window()->setColor(Qt::black);
        source.frame->mapping = mapping();
        source.frame->enabled = true;
        source.frame->options = {chart::bubbles::Mode::Individual, {0, 150, 1}, 0};
        bubbles = new TradeBubbleOverlayItem; // parented after construction, as QML does
        bubbles->setParentItem(scene->window()->contentItem());
        bubbles->setSize(QSizeF(320, 160));
        bubbles->setRenderer(&source);
        // QQuickRenderControl::sync() does not emit afterSynchronizing (the window render
        // loops do): deliver it between sync and render, as a loop would.
        QObject::connect(scene->window(), &QQuickWindow::beforeRendering, scene->window(), [this] {
            QMetaObject::invokeMethod(scene->window(), "afterSynchronizing", Qt::DirectConnection);
        }, Qt::DirectConnection);
    }
    void TearDown() override {
        if (bubbles) bubbles->setRenderer(nullptr);
        if (scene) scene->renderFrame(&error);
        scene.reset();
    }
    void frames(int n) {
        for (int i = 0; i < n; ++i) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            bubbles->update();
            image = scene->renderFrame(&error);
            ASSERT_FALSE(image.isNull()) << error.toStdString();
        }
    }
    // The buy fill at the bubble's centre over the black window: #2EBD85 at 40% opacity,
    // times the 35% passive factor of a first execution, is about (6, 26, 19).
    bool bubbleAt(double timeMs, double price) const {
        const auto m = mapping();
        const double dpr = double(image.width()) / 320;
        const QColor c = image.pixelColor(int(m.timeToScreenX(timeMs) * dpr), int(m.priceToScreenY(price) * dpr));
        std::cout << "bubble centre rgb=" << c.red() << ',' << c.green() << ',' << c.blue() << '\n';
        return c.green() >= 12 && c.green() > c.red() + 5;
    }
};

// A chart opens before its first trade: the node's mesh starts empty and later fills.
TEST_F(BubbleScene, ANodeThatStartsEmptyDrawsItsFirstTrade) {
    frames(3);
    EXPECT_FALSE(bubbleAt(1500, 150));
    ASSERT_TRUE(source.tape->append({1500, 150, 1, AggressorSide::Buy, "1"}));
    frames(3);
    EXPECT_TRUE(bubbleAt(1500, 150)) << "the first trade after an empty mesh is not drawn";
}

// Hiding and showing the bubbles empties the mesh and fills it again.
TEST_F(BubbleScene, AMeshEmptiedAndRefilledDrawsAgain) {
    ASSERT_TRUE(source.tape->append({1500, 150, 1, AggressorSide::Buy, "1"}));
    frames(3);
    ASSERT_TRUE(bubbleAt(1500, 150)) << "a node created with a trade draws it";
    source.frame->enabled = false;
    frames(3);
    EXPECT_FALSE(bubbleAt(1500, 150));
    source.frame->enabled = true;
    frames(3);
    EXPECT_TRUE(bubbleAt(1500, 150)) << "bubbles shown again are not drawn";
}

// The main chart with footprint cells 128 x 20 px (numbers need about 96 px columns) and bubbles set to dim
// over footprint numbers: the renderer passes the footprint's drawn-number state to the
// bubble frame.
class DimScene : public testing::Test {
protected:
    std::unique_ptr<lab::OffscreenQuick> scene;
    UnifiedGridRenderer* ugr = nullptr;
    GoldenFlowSource flow;
    QString error;
    void SetUp() override {
        if (const QString why = lab::gpuUnavailableReason(); !why.isEmpty()) GTEST_SKIP() << "GPU case skipped: " << why.toStdString();
        scene = std::make_unique<lab::OffscreenQuick>();
        ASSERT_TRUE(scene->create(QSize(640, 320), &error)) << error.toStdString();
        scene->window()->setColor(Qt::black);
        ugr = new UnifiedGridRenderer(scene->window()->contentItem());
        ugr->setSize(QSizeF(640, 320));
        ugr->setHeatmapService(&lab::LabData::instance().service());
        ugr->setActiveSymbol("BTC-USD");
        ugr->view()->setTimeframe(int(minute));
        settings("dim");
        ugr->view()->setViewport(viewLo, viewLo + 5 * minute, 100'000, 100'016);
        frames(4);
        ugr->layers()->setFootprintEnabled(true);
        ugr->agentPort().bindFlowSource(flow);
        frames(2);
        flow.deliver(viewLo - minute, 12, 100'000, 16);
    }
    void TearDown() override {
        delete ugr;
        ugr = nullptr;
        if (scene) scene->renderFrame(&error);
        scene.reset();
    }
    void settings(const char* overFootprint) {
        heatmap::HeatmapChartSettings s;
        s.showTrades = true;
        s.bubblesOverFootprint = overFootprint;
        ugr->setHeatmapChartSettings(s);
    }
    void frames(int n) {
        for (int i = 0; i < n; ++i) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            ugr->update();
            ASSERT_FALSE(scene->renderFrame(&error).isNull()) << error.toStdString();
        }
    }
    template <class Done> bool pump(int ms, Done done) {
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < ms) {
            frames(1);
            if (done()) return true;
        }
        return false;
    }
    const chart::FootprintCellsLayer& footprint() const { return TradeBubbleRendererTest::footprint(*ugr); }
    float opacity() const { return ugr->tradeBubbleRenderFrame()->opacityScale; }
};

TEST_F(DimScene, BubblesDimOnlyWhileFootprintNumbersAreDrawn) {
    ASSERT_TRUE(pump(15'000, [&] { return footprint().drawingNumbers(); }))
        << "the footprint numbers never drew: stage=" << int(footprint().stage()) << " drawing=" << footprint().drawing()
        << " glyphs=" << footprint().glyphCount() << " bars=" << ugr->footprintPresentation()->coverage.barCount
        << " colPx=" << ugr->currentFrameContext().raster.colPxF << " rowPx=" << ugr->currentFrameContext().raster.rowPxF;
    // Each change reaches the bubble frame within a few frames (settings re-decide the text fit).
    const auto reaches = [&](float value) { return pump(3'000, [&] { return opacity() == value; }); };
    EXPECT_TRUE(reaches(.35f)) << "footprint numbers are drawn: the bubbles dim; opacity=" << opacity();
    settings("over");
    EXPECT_TRUE(reaches(1.f)) << "over: no dim";
    settings("dim");
    EXPECT_TRUE(reaches(.35f)) << "dim again; opacity=" << opacity();
    // The numbers stage with every number below the dust threshold draws no glyph: no dim.
    ugr->layers()->setFootprintDustNotional(1e15);
    ASSERT_TRUE(pump(5'000, [&] { return footprint().glyphCount() == 0; }));
    frames(1);
    EXPECT_EQ(footprint().stage(), chart::FootprintStage::Numbers);
    EXPECT_FALSE(footprint().drawingNumbers());
    EXPECT_FLOAT_EQ(opacity(), 1.f) << "a numbers stage with zero glyphs must not dim the bubbles";
    ugr->layers()->setFootprintDustNotional(0);
    ASSERT_TRUE(pump(5'000, [&] { return footprint().drawingNumbers(); }));
    EXPECT_TRUE(reaches(.35f));
    ugr->layers()->setFootprintEnabled(false);
    EXPECT_TRUE(reaches(1.f)) << "no footprint: no dim";
}
} // namespace

int main(int argc, char** argv) {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    lab::selectQuickSceneGraph(); // before any QQuickWindow: Qt fixes the backend at the first one
    Q_INIT_RESOURCE(sentinel_ui_fonts);
    Q_INIT_RESOURCE(heatmap_shaders); // trade_bubble qsb: static lib resource
    ::testing::InitGoogleTest(&argc, argv);
    std::cout << "[sentinel] " << lab::describeRhi().toStdString() << std::endl;
    QTemporaryDir dir;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.filePath("settings"));
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, dir.filePath("system-settings"));
    writeRecording(dir, 4 * 60);
    lab::LabData::setLocalSymbolsForTest({"BTC-USD"});
    lab::LabData::configure(dir.path().toStdString(), epoch + 4 * kHourMs);
    return RUN_ALL_TESTS();
}
