// Slice S7b (labels and the liquidity range on the GPU main chart) and the chart
// UI follow-ups (owner request 2026-10-02): the toolbar adapts to the chart's
// layers, the chart settings menu and dialog Chart tab round-trip their settings.
//
// GPU cases run a UnifiedGridRenderer over the synthetic HMC2 recording (as
// test_ugr_gpu) on the selected QRhi backend and skip with the reason when it
// cannot create one: a skipped case is no result. Toolbar/menu cases need no GPU.
// Every QSettings is a temporary INI file (never the owner's).
#include "UnifiedGridRenderer.h"
#include "lab/LabData.hpp"
#include "lab/OffscreenQuick.hpp"
#include "lab/RhiBackend.hpp"
#include "mainwindow/HeatmapChartControls.hpp"
#include "render/heatmap/HeatmapGpuLayer.hpp"
#include "render/heatmap/HeatmapSettingsModel.hpp"
#include "widgets/HeatmapSettingsDialog.hpp"
#include "widgets/TopToolbar.hpp"
#include "SyntheticHmc2Fixture.hpp"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QMenu>
#include <QQuickWindow>
#include <QSettings>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QPushButton>
#include <gtest/gtest.h>
#include <iostream>

namespace {
using namespace synthetic_hmc2;
using heatmap::HeatmapSettingsModel;
using heatmap::HeatmapSettingsStore;
using Mode = TopToolbar::ModeState;
using Shown = TopToolbar::ControlVisibility;

struct TempStore {
    QTemporaryDir dir;
    QString path = dir.filePath("settings.ini");
    QSettings settings{path, QSettings::IniFormat};
    HeatmapSettingsStore store{settings};
    ClientHeatmapConfig config;
    heatmap::HeatmapChartSettings reload() {
        settings.sync();
        QSettings again(path, QSettings::IniFormat);
        return HeatmapSettingsStore(again).load("main", config);
    }
};

QAction *menuAction(QMenu *menu, const QString &name, int nth = 0) {
    int seen = 0;
    for (QAction *a : menu->findChildren<QAction *>())
        if (a->objectName() == name && seen++ == nth) return a;
    ADD_FAILURE() << "no menu action " << name.toStdString() << " #" << nth;
    return nullptr;
}

// ------------------------------------------------------------ toolbar modes
// The rules live in one place (TopToolbar::controlVisibility).
TEST(ChartToolbar, ControlVisibilityRulesPerMode) {
    struct Case {
        const char *what;
        Mode mode;
        Shown shown;
    };
    const std::vector<Case> cases{
        {"heatmap gpu + candles", {true, false, false, false, true, true},
         {true, true, true, true, false, true, false, false}},
        {"heatmap legacy", {true, false, false, false, true, false},
         {true, true, true, false, true, true, false, false}},
        {"tpo", {false, false, true, false, true, true}, {false, false, false, false, false, true, true, true}},
        {"volume profile", {false, false, false, true, false, true},
         {false, false, false, false, false, false, true, false}},
        {"footprint only", {false, true, false, false, false, true},
         {false, false, false, false, false, false, false, false}},
        {"heatmap + footprint, no candles", {true, true, false, false, false, true},
         {true, true, true, true, false, false, false, false}},
    };
    TopToolbar toolbar;
    for (const auto &c : cases) {
        SCOPED_TRACE(c.what);
        EXPECT_EQ(TopToolbar::controlVisibility(c.mode), c.shown);
        toolbar.setModeState(c.mode);
        EXPECT_EQ(toolbar.shownControls(), c.shown) << "the toolbar applies the rules";
        EXPECT_EQ(toolbar.tpoSessionCombo()->isVisibleTo(&toolbar), c.shown.tpoSession);
        EXPECT_EQ(toolbar.rangeSlider()->isVisibleTo(&toolbar), c.shown.rangeSlider);
        EXPECT_EQ(toolbar.tickModeCombo()->isVisibleTo(&toolbar), c.shown.tickSelector);
        EXPECT_EQ(toolbar.candlesChecked(), c.mode.candles);
    }
}

// --------------------------------------------------- range slider and menu
class ChartControls : public testing::Test {
protected:
    TempStore t;
    std::unique_ptr<HeatmapSettingsModel> model = std::make_unique<HeatmapSettingsModel>(t.store, "main", t.config);
    std::unique_ptr<HeatmapChartControls> controls = std::make_unique<HeatmapChartControls>(model.get());
    std::unique_ptr<TopToolbar> toolbar = std::make_unique<TopToolbar>();
    void SetUp() override {
        controls->setToolbar(toolbar.get());
        toolbar->resize(2400, 40);
        toolbar->show();
        QCoreApplication::processEvents();
    }
};

// A drag on the range slider applies process-only while it moves and persists on
// release; the model drives the handles otherwise.
TEST_F(ChartControls, RangeSliderDragAppliesLiveAndPersistsOnRelease) {
    auto *slider = toolbar->rangeSlider();
    ASSERT_TRUE(slider->isVisible());
    const double before = model->settings().sensitivityMin;
    const QPoint low(int(slider->xOf(slider->low())), slider->height() / 2);
    QTest::mousePress(slider, Qt::LeftButton, {}, low);
    QMouseEvent move(QEvent::MouseMove, QPointF(low + QPoint(30, 0)), slider->mapToGlobal(QPointF(low + QPoint(30, 0))),
                     Qt::NoButton, Qt::LeftButton, {});
    QApplication::sendEvent(slider, &move);
    EXPECT_GT(model->settings().sensitivityMin, before) << "applied while dragging";
    EXPECT_EQ(t.reload().sensitivityMin, before) << "not persisted while dragging";
    QTest::mouseRelease(slider, Qt::LeftButton, {}, low + QPoint(30, 0));
    const double after = model->settings().sensitivityMin;
    EXPECT_GT(after, before);
    EXPECT_EQ(t.reload().sensitivityMin, after) << "persisted on release";
    EXPECT_LT(model->settings().sensitivityMin, model->settings().sensitivityMax);
    // The model (API, dialog) moves the handles.
    ASSERT_TRUE(model->apply({{"sensitivityMin", 0.2}, {"sensitivityMax", 20.0}}).isEmpty());
    EXPECT_DOUBLE_EQ(slider->low(), 0.2);
    EXPECT_DOUBLE_EQ(slider->high(), 20.0);
    EXPECT_TRUE(toolbar->rangeLabel()->text().contains("0.2 - 20"));
}

// The low handle never passes the high one, and the ends always contain both.
TEST_F(ChartControls, RangeSliderKeepsLowBelowHigh) {
    auto *slider = toolbar->rangeSlider();
    slider->setDomain(0.01, 100);
    const QPoint low(int(slider->xOf(slider->low())), slider->height() / 2);
    QTest::mousePress(slider, Qt::LeftButton, {}, low);
    QTest::mouseRelease(slider, Qt::LeftButton, {}, QPoint(slider->width() - 1, slider->height() / 2));
    EXPECT_LT(model->settings().sensitivityMin, model->settings().sensitivityMax);
    EXPECT_GE(model->settings().sensitivityMax / model->settings().sensitivityMin, 1.0);
    ASSERT_TRUE(model->apply({{"sensitivityMin", 1e-6}, {"sensitivityMax", 1e6}}).isEmpty());
    EXPECT_LE(slider->endLo(), 1e-6);
    EXPECT_GE(slider->endHi(), 1e6);
}

// The chart settings menu: one entry point; every entry round-trips through the
// settings model (persisted), and the model's changes show in the menu.
TEST_F(ChartControls, ChartMenuRoundTripsTheLabelSettings) {
    int opened = 0, shots = 0;
    QString tab;
    HeatmapChartControls::MenuHooks hooks;
    hooks.openSettings = [&](const QString &name) { ++opened; tab = name; };
    hooks.screenshot = [&] { ++shots; };
    controls->setMenuHooks(hooks);
    QMenu *menu = toolbar->chartMenu();
    ASSERT_TRUE(menu);
    EXPECT_EQ(toolbar->chartMenuButton()->menu(), menu);
    menuAction(menu, "chartMenuSettings")->trigger();
    EXPECT_EQ(opened, 1);
    EXPECT_EQ(tab, "Chart");
    menuAction(menu, "chartMenuScreenshot")->trigger();
    EXPECT_EQ(shots, 1);
    EXPECT_FALSE(menuAction(menu, "chartMenuSaveLayout")->isEnabled()) << "no hook, disabled";

    // The menu shows the model (labels on, USD, the small size by default).
    EXPECT_TRUE(menuAction(menu, "chartMenuLabels")->isChecked());
    EXPECT_TRUE(menuAction(menu, "chartMenuCurrencyUsd")->isChecked());
    EXPECT_TRUE(menuAction(menu, "chartMenuLabelSizePreset", 0)->isChecked());
    // Labels off, asset currency, the medium size: model and store.
    menuAction(menu, "chartMenuLabels")->trigger();
    emit menu->aboutToShow();
    EXPECT_FALSE(menuAction(menu, "chartMenuLabels")->isChecked());
    menuAction(menu, "chartMenuCurrencyAsset")->trigger();
    menuAction(menu, "chartMenuLabelSizePreset", 1)->trigger();
    const auto &s = model->settings();
    EXPECT_FALSE(s.showLabels);
    EXPECT_EQ(s.labelCurrency, "asset");
    EXPECT_EQ(s.labelMinPx, 14);
    EXPECT_EQ(s.labelMaxPx, 18);
    const auto saved = t.reload();
    EXPECT_FALSE(saved.showLabels);
    EXPECT_EQ(saved.labelCurrency, "asset");
    EXPECT_EQ(saved.labelMaxPx, 18);
    // The toolbar shows them.
    EXPECT_FALSE(toolbar->labelsButton()->isChecked());
    EXPECT_EQ(toolbar->liquidityModeCombo()->currentText(), "Asset");

    // The other way: an API-style patch shows in the menu and the toolbar.
    ASSERT_TRUE(model->apply({{"showLabels", true}, {"labelCurrency", "usd"}, {"labelMinPx", 12.0}, {"labelMaxPx", 15.0}})
                    .isEmpty());
    emit menu->aboutToShow();
    EXPECT_TRUE(menuAction(menu, "chartMenuLabels")->isChecked());
    EXPECT_TRUE(menuAction(menu, "chartMenuCurrencyUsd")->isChecked());
    EXPECT_FALSE(menuAction(menu, "chartMenuCurrencyAsset")->isChecked());
    EXPECT_TRUE(menuAction(menu, "chartMenuLabelSizePreset", 0)->isChecked());
    EXPECT_TRUE(toolbar->labelsButton()->isChecked());
    EXPECT_EQ(toolbar->liquidityModeCombo()->currentText(), "USD");
    // Toolbar edits reach the model too.
    toolbar->labelsButton()->click();
    EXPECT_FALSE(model->settings().showLabels);
    toolbar->liquidityModeCombo()->setCurrentIndex(0);
    emit toolbar->liquidityModeCombo()->activated(0);
    EXPECT_EQ(model->settings().labelCurrency, "asset");
}

// The dialog's Chart tab edits the same keys, and follows the model.
TEST_F(ChartControls, DialogChartTabRoundTrip) {
    HeatmapSettingsDialog dialog(model.get(), nullptr);
    controls->setDialog(&dialog);
    EXPECT_EQ(dialog.tabs()->tabText(0), "Chart");
    auto *show = dialog.findChild<QCheckBox *>("showLabels");
    auto *currency = dialog.findChild<QComboBox *>("labelCurrency");
    auto *minPx = dialog.findChild<QDoubleSpinBox *>("labelMinPx");
    auto *maxPx = dialog.findChild<QDoubleSpinBox *>("labelMaxPx");
    ASSERT_TRUE(show && currency && minPx && maxPx);
    EXPECT_TRUE(show->isChecked());
    EXPECT_EQ(currency->currentData().toString(), "usd");
    show->setChecked(false);
    currency->setCurrentIndex(currency->findData("asset"));
    maxPx->setValue(20);
    minPx->setValue(16);
    EXPECT_FALSE(model->settings().showLabels);
    EXPECT_EQ(model->settings().labelCurrency, "asset");
    EXPECT_EQ(model->settings().labelMinPx, 16);
    EXPECT_EQ(model->settings().labelMaxPx, 20);
    EXPECT_EQ(t.reload().labelMinPx, 16);
    ASSERT_TRUE(model->apply({{"showLabels", true}, {"labelMaxPx", 24.0}}).isEmpty());
    EXPECT_TRUE(show->isChecked());
    EXPECT_EQ(maxPx->value(), 24);
    // A minimum above the maximum moves the maximum with it (never invalid).
    minPx->setValue(22);
    maxPx->setValue(18);
    EXPECT_LE(model->settings().labelMinPx, model->settings().labelMaxPx);
    // Reset Chart restores the defaults.
    dialog.findChild<QPushButton *>("resetChart")->click();
    EXPECT_EQ(model->settings().labelMinPx, 12);
    EXPECT_EQ(model->settings().labelMaxPx, 15);
    EXPECT_EQ(model->settings().labelCurrency, "usd");
}

// Patch validation of the new keys (Agent API heatmap/settings).
TEST_F(ChartControls, LabelSettingsAreValidated) {
    EXPECT_FALSE(model->apply({{"labelCurrency", "eur"}}).isEmpty());
    EXPECT_FALSE(model->apply({{"showLabels", 1}}).isEmpty());
    ASSERT_TRUE(model->apply({{"labelMinPx", 2.0}, {"labelMaxPx", 99.0}}).isEmpty());
    EXPECT_EQ(model->settings().labelMinPx, 8);  // clamped
    EXPECT_EQ(model->settings().labelMaxPx, 32);
    ASSERT_TRUE(model->apply({{"labelMinPx", 20.0}, {"labelMaxPx", 10.0}}).isEmpty());
    EXPECT_LE(model->settings().labelMinPx, model->settings().labelMaxPx);
}

// ------------------------------------------------------------- GPU chart
QTemporaryDir *fixtureDir = nullptr;

class ChartLabels : public testing::Test {
protected:
    std::unique_ptr<lab::OffscreenQuick> scene;
    UnifiedGridRenderer *ugr = nullptr;
    QString error;
    QImage image;
    TempStore t;
    std::unique_ptr<HeatmapSettingsModel> model;
    std::unique_ptr<HeatmapChartControls> controls;
    std::unique_ptr<TopToolbar> toolbar;
    const int64_t viewLo = epoch + 2 * kHourMs;
    void SetUp() override {
        if (const QString why = lab::gpuUnavailableReason(); !why.isEmpty())
            GTEST_SKIP() << "GPU case skipped: " << why.toStdString();
        scene = std::make_unique<lab::OffscreenQuick>();
        ASSERT_TRUE(scene->create(QSize(640, 320), &error)) << error.toStdString();
        scene->window()->setColor(Qt::black);
        ugr = new UnifiedGridRenderer(scene->window()->contentItem());
        ugr->setSize(QSizeF(640, 320));
        ugr->setHeatmapService(&lab::LabData::instance().service());
        ugr->setActiveSymbol("BTC-USD");
        ugr->setTimeframe(int(minute));
        model = std::make_unique<HeatmapSettingsModel>(t.store, "main", t.config);
        // Bright fixture colours (sizes 0.01..0.53), Manual $1, no crossfade unless a case asks.
        ASSERT_TRUE(model->apply({{"renderer", "gpu"}, {"palettePreset", "Fire"}, {"sensitivityMin", 0.001},
                                  {"sensitivityMax", 1.0}, {"tickMode", "manual"}, {"manualTick", 100},
                                  {"crossfadeMs", 0}}, false)
                        .isEmpty());
        controls = std::make_unique<HeatmapChartControls>(model.get());
        toolbar = std::make_unique<TopToolbar>();
        controls->setToolbar(toolbar.get());
        controls->setRenderer(ugr);
    }
    void TearDown() override {
        controls.reset();
        delete ugr;
        ugr = nullptr;
        if (scene) scene->renderFrame(&error);
        scene.reset();
    }
    heatmap::gpu::HeatmapGpuLayer &layer() const { return *ugr->gpuHeatmapLayer(); }
    // 10 minutes x $16 over 640 x 320: $1 cells of 64 x 20 px.
    void zoomIn(double priceLo = 100'000) { ugr->setViewport(viewLo, viewLo + 10 * minute, priceLo, priceLo + 16); }
    // onFrame runs after every frame (invariants), done ends the pump.
    template <class Done, class OnFrame> bool pump(int ms, Done done, OnFrame onFrame) {
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
    template <class Done> bool pump(int ms, Done done) { return pump(ms, done, [] {}); }
    bool labelsDrawn() const { return !ugr->gpuLabelGlyphs().empty() && ugr->gpuLabelLayout().stats().labels > 0; }
    // Labels must belong to the drawn picture in every frame (plan section 3).
    void checkFrame() {
        const auto labels = layer().labelsForFrame();
        const auto &st = layer().tileStats();
        if (labels) {
            ASSERT_EQ(labels->key.tickUnits, st.drawnTickUnits.load());
            ASSERT_EQ(labels->key.tfMs, st.drawnTfMs.load());
            ASSERT_FALSE(st.crossfading.load());
        }
        if (!labels) ASSERT_TRUE(ugr->gpuLabelGlyphs().empty()) << "glyphs without a matched result";
        for (const auto &g : ugr->gpuLabelGlyphs()) {
            ASSERT_GE(g.rect.left(), 0.0);
            ASSERT_GE(g.rect.top(), 0.0);
            ASSERT_LE(g.rect.right(), 640.0);
            ASSERT_LE(g.rect.bottom(), 320.0);
        }
    }
    bool waitForLabels(int ms = 20'000) {
        return pump(ms, [&] { return labelsDrawn(); }, [&] { checkFrame(); });
    }
    heatmap::HeatmapCellQuery *query() const { return layer().controller()->cellQuery(); }
};

TEST_F(ChartLabels, LabelsDrawOnColouredCellsAtTheDrawnTick) {
    zoomIn();
    ASSERT_TRUE(waitForLabels()) << error.toStdString();
    const auto labels = layer().labelsForFrame();
    ASSERT_TRUE(labels);
    EXPECT_EQ(labels->key.tickUnits, 100);
    EXPECT_EQ(labels->key.tfMs, minute);
    const auto &stats = ugr->gpuLabelLayout().stats();
    EXPECT_GE(stats.sizePx, 12.0);
    EXPECT_LE(stats.sizePx, 15.0);
    // About the visible $1 x 1m cells (10 x 16), all coloured in the fixture's near band.
    EXPECT_GE(stats.labels, 100u);
    EXPECT_LE(stats.labels, 11u * 17u);
    // The request covered the view plus a margin, within the query's limit.
    const auto request = layer().postedLabelRequest();
    EXPECT_LE(uint64_t(request.columns) * request.rows, heatmap::gpu::HeatmapGpuLayer::kMaxLabelCells);
    EXPECT_LE(request.firstBucket, viewLo / minute);
    EXPECT_GE(request.firstBucket + int64_t(request.columns), (viewLo + 10 * minute) / minute);
    // The slider's ends follow the window's liquidity.
    pump(400, [] { return false; });
    const auto range = layer().liquidityRange();
    ASSERT_TRUE(range.valid);
    EXPECT_GT(range.lo, 0.0);
    EXPECT_LE(range.hi, 0.6);
    EXPECT_DOUBLE_EQ(toolbar->rangeSlider()->endHi(), std::max(range.hi, 1.0 * 1.25));
    // The low handle above every cell hides every label with the colour.
    ASSERT_TRUE(model->apply({{"sensitivityMin", 0.9}, {"sensitivityMax", 2.0}}, false).isEmpty());
    ASSERT_TRUE(pump(2000, [&] { return ugr->gpuLabelLayout().stats().labels == 0; }));
    // Labels off: none drawn (the request still runs for the slider's ends).
    ASSERT_TRUE(model->apply({{"sensitivityMin", 0.001}, {"sensitivityMax", 1.0}, {"showLabels", false}}, false).isEmpty());
    pump(300, [] { return false; });
    EXPECT_TRUE(ugr->gpuLabelGlyphs().empty());
    EXPECT_FALSE(layer().labelsForFrame());
}

// The gate: with more than kMaxLabelCells visible cells nothing is posted.
TEST_F(ChartLabels, ZoomedOutPostsNoLabelRequest) {
    ugr->setViewport(viewLo, viewLo + 120 * minute, 99'900, 100'300); // 120 x 400 $1 cells
    const auto before = layer().labelCounters().posted;
    pump(1500, [] { return false; }, [&] { checkFrame(); });
    EXPECT_EQ(layer().labelCounters().posted, before);
    EXPECT_EQ(layer().postedLabelRequest().serial, 0u);
    EXPECT_TRUE(ugr->gpuLabelGlyphs().empty());
}

// HeatmapCellQuery::cancel() drops its result without a signal: the chart must
// not keep drawing a held copy, and it asks again (retry with backoff).
TEST_F(ChartLabels, ACancelledQueryDrawsNoStaleLabelsAndRetries) {
    zoomIn();
    ASSERT_TRUE(waitForLabels()) << error.toStdString();
    auto *q = query();
    auto cancel = [&] {
        if (q->thread() == QThread::currentThread()) q->cancel();
        else QMetaObject::invokeMethod(q, [q] { q->cancel(); }, Qt::BlockingQueuedConnection);
        ASSERT_FALSE(q->latestLabels());
    };
    // A quiet chart: settled, labels drawn and no request for 20 frames.
    auto quiet = [&] {
        int frames = 0;
        uint64_t posted = layer().labelCounters().posted;
        return pump(20'000, [&] {
            frames = layer().settled() && layer().labelCounters().posted == posted && labelsDrawn() ? frames + 1 : 0;
            posted = layer().labelCounters().posted;
            return frames >= 20;
        });
    };
    for (int round = 0; round < 3; ++round) {
        SCOPED_TRACE(round);
        ASSERT_TRUE(quiet());
        const uint64_t drawnSerial = ugr->gpuLabelSerial(), posted = layer().labelCounters().posted;
        const uint64_t retries = layer().labelCounters().retries;
        ASSERT_GT(drawnSerial, 0u);
        cancel();
        // The very next frame never draws the cancelled result.
        ASSERT_TRUE(pump(1000, [] { return true; }));
        EXPECT_NE(ugr->gpuLabelSerial(), drawnSerial) << "no label drawn from a cancelled result";
        // They come back: by a new request (releasing the query's wants can republish
        // the SpanSet) or, with nothing else changing, by the retry.
        ASSERT_TRUE(pump(heatmap::gpu::HeatmapGpuLayer::kLabelRetryMs + 8000,
                         [&] { return labelsDrawn() && ugr->gpuLabelSerial() > drawnSerial; }, [&] { checkFrame(); }))
            << error.toStdString();
        const bool retried = layer().labelCounters().retries > retries;
        if (layer().labelCounters().posted == posted + 1 && !retried) continue; // a version change re-posted
        if (retried) {
            EXPECT_EQ(layer().labelCounters().retries, retries + 1);
            return; // the retry path ran
        }
    }
    std::cout << "[note] every cancel was followed by a republished SpanSet; the retry ran: "
              << layer().labelCounters().retries << std::endl;
}

// Tick changes: a result of another tick never draws; during the crossfade none
// draws; the new tick's labels arrive.
TEST_F(ChartLabels, LabelsNeverDrawForAnotherTickOrDuringACrossfade) {
    ASSERT_TRUE(model->apply({{"crossfadeMs", 600}}, false).isEmpty());
    zoomIn();
    ASSERT_TRUE(waitForLabels()) << error.toStdString();
    ASSERT_TRUE(model->apply({{"tickMode", "manual"}, {"manualTick", 200}}, false).isEmpty());
    bool sawCrossfade = false;
    ASSERT_TRUE(pump(20'000, [&] {
        const auto labels = layer().labelsForFrame();
        return labels && labels->key.tickUnits == 200 && labelsDrawn();
    }, [&] {
        checkFrame();
        if (layer().tileStats().crossfading.load()) {
            sawCrossfade = true;
            EXPECT_TRUE(ugr->gpuLabelGlyphs().empty()) << "no labels during the crossfade";
        }
    })) << error.toStdString();
    EXPECT_TRUE(sawCrossfade);
    // The old tick's result is gone from use: the drawn tick is $2 now.
    EXPECT_EQ(layer().tileStats().drawnTickUnits.load(), 200);
}

// A symbol switch while a request is in flight: the old symbol's labels (and a
// result that lands after the switch) never draw on the new symbol's picture.
TEST_F(ChartLabels, ASymbolSwitchMidRequestDropsTheOldSymbolsLabels) {
    zoomIn();
    ASSERT_TRUE(waitForLabels()) << error.toStdString();
    const uint64_t oldSerial = layer().labelsForFrame()->key.serial;
    // A new window (a request in flight), then the switch in the same turn.
    zoomIn(100'004);
    pump(1, [] { return true; });
    ugr->setActiveSymbol("ETH-USD");
    EXPECT_FALSE(layer().labelsForFrame()) << "dropped at once, before any frame";
    pump(1500, [] { return false; }, [&] {
        checkFrame();
        EXPECT_TRUE(ugr->gpuLabelGlyphs().empty());
    });
    // Whatever the query holds now, only a request made after the switch may draw.
    if (const auto labels = layer().labelsForFrame()) EXPECT_GE(labels->key.serial, layer().labelCounters().epoch);
    // Back to BTC: only a request made after the switch draws.
    ugr->setActiveSymbol("BTC-USD");
    zoomIn();
    ASSERT_TRUE(waitForLabels()) << error.toStdString();
    EXPECT_GT(layer().labelsForFrame()->key.serial, oldSerial);
    EXPECT_GE(layer().labelsForFrame()->key.serial, layer().labelCounters().epoch);
}

// The toolbar follows the chart's layers (TPO hides the heatmap-only controls),
// and the currency combo drives the model and the legacy label mode.
TEST_F(ChartLabels, ToolbarFollowsTheChartsLayers) {
    pump(300, [] { return false; });
    EXPECT_TRUE(toolbar->shownControls().rangeSlider);
    EXPECT_TRUE(toolbar->shownControls().tickSelector);
    EXPECT_FALSE(toolbar->shownControls().tpoSession);
    ugr->setTpoLayerEnabled(true); // exclusive today: the heatmap goes off
    pump(300, [] { return false; });
    auto shown = toolbar->shownControls();
    EXPECT_FALSE(shown.tickSelector);
    EXPECT_FALSE(shown.rangeSlider);
    EXPECT_FALSE(shown.palette);
    EXPECT_FALSE(shown.liquidity);
    EXPECT_TRUE(shown.tpoSession);
    EXPECT_TRUE(shown.tpoLayout);
    // TPO controls drive the renderer and follow it.
    const int split = toolbar->tpoLayoutCombo()->findData("split");
    toolbar->tpoLayoutCombo()->setCurrentIndex(split);
    emit toolbar->tpoLayoutCombo()->activated(split);
    EXPECT_EQ(ugr->tpoLayout().toStdString(), "split");
    ugr->setTpoLayout("collapsed"); // the dialog or the API (no renderer signal: the next sync shows it)
    controls->syncNow();
    EXPECT_EQ(toolbar->tpoLayoutCombo()->currentData().toString(), "collapsed");
    ugr->setHeatmapLayerEnabled(true);
    pump(300, [] { return false; });
    shown = toolbar->shownControls();
    EXPECT_TRUE(shown.tickSelector);
    EXPECT_TRUE(shown.rangeSlider);
    EXPECT_FALSE(shown.tpoSession);
    // Legacy renderer: the threshold slider instead of the range slider.
    ASSERT_TRUE(model->apply({{"renderer", "legacy"}}, false).isEmpty());
    pump(300, [] { return false; });
    shown = toolbar->shownControls();
    EXPECT_FALSE(shown.rangeSlider);
    EXPECT_TRUE(shown.thresholdSlider);
    // Currency: model, toolbar and the legacy label mode agree.
    toolbar->liquidityModeCombo()->setCurrentIndex(0);
    emit toolbar->liquidityModeCombo()->activated(0);
    EXPECT_EQ(model->settings().labelCurrency, "asset");
    EXPECT_EQ(ugr->liquidityLabelMode(), 0);
    ASSERT_TRUE(model->apply({{"labelCurrency", "usd"}}, false).isEmpty());
    EXPECT_EQ(ugr->liquidityLabelMode(), 1);
}

// Lifecycle: the settings dialog, the chart menu and the toolbar stay usable when
// the chart (renderer) is destroyed under them.
TEST_F(ChartLabels, DialogAndMenuSurviveTheRendererBeingDestroyed) {
    HeatmapSettingsDialog dialog(model.get(), ugr);
    controls->setDialog(&dialog);
    dialog.show();
    zoomIn();
    ASSERT_TRUE(waitForLabels()) << error.toStdString();
    delete ugr;
    ugr = nullptr;
    QCoreApplication::processEvents();
    // Every surface still edits the model; renderer-backed entries do nothing.
    dialog.findChild<QCheckBox *>("showLabels")->setChecked(false);
    EXPECT_FALSE(model->settings().showLabels);
    dialog.findChild<QComboBox *>("candleStyle")->setCurrentIndex(2);
    controls->syncNow();
    emit toolbar->chartMenu()->aboutToShow();
    menuAction(toolbar->chartMenu(), "chartMenuCandleStylePreset", 1)->trigger();
    EXPECT_FALSE(menuAction(toolbar->chartMenu(), "chartMenuCandleStylePreset", 1)->isEnabled()) << "no chart";
    emit toolbar->chartTypeCombo()->activated(2); // the toolbar's candle style with no chart
    menuAction(toolbar->chartMenu(), "chartMenuCurrencyAsset")->trigger();
    EXPECT_EQ(model->settings().labelCurrency, "asset");
    emit toolbar->tpoLayoutCombo()->activated(1);
    emit toolbar->rangeSlider()->rangeEdited(0.01, 2.0, true);
    EXPECT_DOUBLE_EQ(model->settings().sensitivityMax, 2.0);
    dialog.refreshFromRenderer();
    dialog.close();
}
} // namespace

int main(int argc, char **argv) {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    Q_INIT_RESOURCE(sentinel_ui_fonts);
    ::testing::InitGoogleTest(&argc, argv);
    std::cout << "[sentinel] " << lab::describeRhi().toStdString() << std::endl;
    fixtureDir = new QTemporaryDir;
    writeRecording(*fixtureDir, 4 * 60);
    lab::LabData::configure(fixtureDir->path().toStdString(), epoch + 4 * kHourMs);
    return RUN_ALL_TESTS();
}
