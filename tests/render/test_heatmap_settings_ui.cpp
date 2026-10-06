// Slice S6c: the heatmap settings UI on the settings model (the single source of
// truth): the tabbed settings dialog, per-chart and named-workspace persistence,
// the toolbar tick selector, the telemetry dock and Agent API -> UI sync.
// Widgets run offscreen; every QSettings is a temporary INI file (never the
// owner's). GPU cases (a UnifiedGridRenderer over the synthetic HMC2 recording,
// as test_ugr_gpu) skip with the reason when no QRhi can be created.
#include "UnifiedGridRenderer.h"
#include "render/CandlestickOverlayItem.hpp"
#include "render/CandlePixelGeometry.hpp"
#include "CoordinateSystem.h"
#include "lab/LabData.hpp"
#include "lab/OffscreenQuick.hpp"
#include "lab/RhiBackend.hpp"
#include "mainwindow/AgentApiCodec.hpp"
#include "mainwindow/HeatmapChartControls.hpp"
#include "render/heatmap/HeatmapGpuLayer.hpp"
#include "render/heatmap/HeatmapSettingsModel.hpp"
#include "widgets/HeatmapSettingsDialog.hpp"
#include "widgets/HeatmapTelemetryDock.hpp"
#include "widgets/TopToolbar.hpp"
#include "SyntheticHmc2Fixture.hpp"
#include <QAction>
#include <QApplication>
#include <QMainWindow>
#include <QDockWidget>
#include <QStandardItemModel>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QPushButton>
#include <QQuickWindow>
#include <QSettings>
#include <QSGGeometry>
#include <QSignalSpy>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <gtest/gtest.h>
#include "render/TradeBubbleNode.hpp"
#include "render/TradeBubbleOverlayItem.hpp"
#include <QFile>
#include <QQmlEngine>
#include <QQmlContext>
#include <QQmlComponent>
#include <QQuickWindow>
#include <iostream>

struct TradeBubbleRendererTest {
    static void publish(UnifiedGridRenderer& r, const TimeAxisMapping& m) { r.publishTradeBubbleFrame(m); }
    static size_t count(const UnifiedGridRenderer& r) { return r.m_tradeBubbleTape->samples().size(); }
    static int64_t firstTime(const UnifiedGridRenderer& r) { return r.m_tradeBubbleTape->samples().front().timeMs; }
    static bool enabled(const UnifiedGridRenderer& r) { return r.m_showTrades; }
    static double threshold(const UnifiedGridRenderer& r) { return r.m_tradeMinNotional; }
};

struct TradeBubbleOverlayTest {
    static QSGNode* root(TradeBubbleOverlayItem& item, QSGNode* old) { return item.updatePaintNode(old,nullptr); }
    static auto link(TradeBubbleOverlayItem& item) { return item.m_link; }
};

namespace {
using namespace synthetic_hmc2;
using heatmap::HeatmapChartSettings;
using heatmap::HeatmapSettingsModel;
using heatmap::HeatmapSettingsStore;
using heatmap::TickMode;
constexpr uint64_t MiB = 1ull << 20;

// A temporary INI-backed store; reload() reads the file through a second
// QSettings, as a restarted process would.
struct TempStore {
    QTemporaryDir dir;
    QString path = dir.filePath("settings.ini");
    QSettings settings{path, QSettings::IniFormat};
    HeatmapSettingsStore store{settings};
    ClientHeatmapConfig config;
    HeatmapChartSettings reload(const QString &chart = "main") {
        settings.sync();
        QSettings again(path, QSettings::IniFormat);
        return HeatmapSettingsStore(again).load(chart, config);
    }
    heatmap::HeatmapBudgets reloadBudgets() {
        settings.sync();
        QSettings again(path, QSettings::IniFormat);
        return HeatmapSettingsStore(again).loadBudgets(config);
    }
    heatmap::ManualTickMemory reloadTicks() {
        settings.sync();
        QSettings again(path, QSettings::IniFormat);
        return HeatmapSettingsStore(again).loadManualTicks();
    }
    QStringList keys() {
        settings.sync();
        return settings.allKeys();
    }
};

template <class T> T *child(QWidget &parent, const char *name) {
    auto *w = parent.findChild<T *>(name);
    EXPECT_NE(w, nullptr) << name;
    return w;
}

QJsonArray stopsJson(std::initializer_list<std::pair<double, const char *>> stops) {
    QJsonArray out;
    for (const auto &[p, c] : stops) out.append(QJsonObject{{"position", p}, {"color", c}});
    return out;
}

// ------------------------------------------------------------- persistence
TEST(HeatmapSettingsModelTest, PersistsPerChartAndASessionRendererIsNotSaved) {
    TempStore t;
    HeatmapSettingsModel main(t.store, "main", t.config), other(t.store, "other", t.config);
    QSignalSpy changed(&main, &HeatmapSettingsModel::changed);
    ASSERT_TRUE(main.apply({{"palettePreset", "Ocean"}, {"crossfadeMs", 0}, {"minRowPx", 3.0}}).isEmpty());
    EXPECT_EQ(changed.count(), 1);
    ASSERT_TRUE(main.apply({{"renderer", "legacy"}}, false).isEmpty()); // this session only (gpu is the default)
    EXPECT_EQ(main.settings().renderer, "legacy");
    EXPECT_EQ(main.savedRenderer(), "gpu");
    const auto saved = t.reload();
    EXPECT_EQ(saved.palettePreset, "Ocean");
    EXPECT_EQ(saved.crossfadeMs, 0);
    EXPECT_EQ(saved.minRowPx, 3.0);
    EXPECT_EQ(saved.renderer, "gpu");
    EXPECT_EQ(t.reload("other"), other.defaultSettings()); // per chart
    // A later persisted patch saves its own fields only, never the session renderer.
    ASSERT_TRUE(main.apply({{"opacity", 0.5}}).isEmpty());
    EXPECT_EQ(t.reload().renderer, "gpu");
    EXPECT_EQ(t.reload().opacity, 0.5);
    // An invalid patch changes nothing and emits nothing.
    changed.clear();
    EXPECT_FALSE(main.apply({{"palettePreset", "Nope"}}).isEmpty());
    EXPECT_EQ(changed.count(), 0);
    EXPECT_EQ(main.settings().palettePreset, "Ocean");
}

TEST(CandleStyleTest, PhysicalWicksSnapAndDojiIsOnePixel) {
    for (double dpr : {1.0, 1.5, 2.0}) {
        for (double zoom : {0.8, 3.0, 17.0, 64.0}) {
            const double centre = 7.25 + zoom * 0.37;
            for (int width = 1; width <= 3; ++width) {
                const auto span = candle_pixels::stroke(centre, width, dpr);
                QSGGeometry geometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 6);
                auto *v = geometry.vertexDataAsColoredPoint2D();
                v[0].set(span.lo, 0, 1, 2, 3, 255);
                v[1].set(span.hi, 10, 1, 2, 3, 255);
                EXPECT_NEAR((v[1].x - v[0].x) * dpr, width, 1e-5);
                EXPECT_NEAR(v[0].x * dpr, std::round(v[0].x * dpr), 1e-5);
                EXPECT_NEAR(v[1].x * dpr, std::round(v[1].x * dpr), 1e-5);
                if (width == 1) EXPECT_NEAR((v[0].x + v[1].x) * dpr * 0.5 - 0.5,
                                             std::round((v[0].x + v[1].x) * dpr * 0.5 - 0.5), 1e-5);
            }
            const auto doji = candle_pixels::doji(12.37 + zoom, dpr);
            EXPECT_NEAR((doji.hi - doji.lo) * dpr, 1.0, 1e-5);
            EXPECT_NEAR(doji.lo * dpr, std::round(doji.lo * dpr), 1e-5);
        }
    }
}

TEST(CandleStyleTest, DefaultColoursOpacityAndGeometryCapacity) {
    CandlestickOverlayItem item;
    EXPECT_EQ(item.upColor().name(), "#2ebd85");
    EXPECT_EQ(item.downColor().name(), "#f6465d");
    EXPECT_EQ(item.wickColor(), "auto");
    EXPECT_EQ(item.bodyOpacity(), 1.0);
    EXPECT_EQ(candle_pixels::bodyAlpha(item.bodyOpacity()), 255);
    EXPECT_EQ(item.wickWidth(), 1);
    int capacity = 0;
    QSGGeometry geometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0);
    for (int frame = 0; frame < 100; ++frame) {
        const int previous = capacity;
        candle_pixels::setGeometryCount(geometry, capacity, 6 * (frame % 17 + 1));
        EXPECT_GE(capacity, previous);
        EXPECT_EQ(geometry.vertexCount(), 6 * (frame % 17 + 1));
    }
    const int warmed = capacity;
    const void *data = geometry.vertexData();
    for (int frame = 0; frame < 100; ++frame) {
        candle_pixels::setGeometryCount(geometry, capacity, 6 * (frame % 17 + 1));
        EXPECT_EQ(capacity, warmed);
        EXPECT_EQ(geometry.vertexData(), data);
    }
}

TEST(CandleStyleTest, SettingsValidateAndRoundTripPerChart) {
    TempStore t;
    HeatmapSettingsModel main(t.store, "main", t.config), other(t.store, "other", t.config);
    const auto defaults = main.settings();
    EXPECT_EQ(defaults.candleUpColor, "#2EBD85");
    EXPECT_EQ(defaults.candleDownColor, "#F6465D");
    EXPECT_EQ(defaults.candleWickColor, "auto");
    EXPECT_EQ(defaults.candleBodyOpacity, 1);
    EXPECT_EQ(defaults.candleWickWidth, 1);
    ASSERT_TRUE(main.apply({{"candleUpColor", "#123ABC"}, {"candleDownColor", "#E45678"},
                            {"candleWickColor", "#F0F0F0"}, {"candleBodyOpacity", 0.65},
                            {"candleWickWidth", 3}}).isEmpty());
    EXPECT_EQ(main.settings().candleUpColor, "#123ABC");
    EXPECT_EQ(main.settings().candleDownColor, "#E45678");
    EXPECT_EQ(main.settings().candleWickColor, "#F0F0F0");
    EXPECT_DOUBLE_EQ(main.settings().candleBodyOpacity, 0.65);
    EXPECT_EQ(main.settings().candleWickWidth, 3);
    EXPECT_EQ(t.reload(), main.settings());
    EXPECT_EQ(t.reload("other"), other.settings());
    const auto before = main.settings();
    for (const QJsonObject bad : {QJsonObject{{"candleUpColor", "cyan"}},
                                  QJsonObject{{"candleWickColor", "#12"}},
                                  QJsonObject{{"candleWickWidth", 0}},
                                  QJsonObject{{"candleWickWidth", 1.5}},
                                  QJsonObject{{"candleBodyOpacity", "opaque"}}}) {
        EXPECT_FALSE(main.apply(bad).isEmpty());
        EXPECT_EQ(main.settings(), before);
    }
    ASSERT_TRUE(main.apply({{"candleWickColor", "auto"}}).isEmpty());
    EXPECT_EQ(t.reload().candleWickColor, "auto");
}

TEST(CandleStyleTest, ChartControlsApplyLiveAndReset) {
    TempStore t;
    HeatmapSettingsModel model(t.store, "main", t.config);
    HeatmapSettingsDialog dialog(&model, nullptr);
    auto *opacity = child<QSpinBox>(dialog, "candleBodyOpacity");
    auto *width = child<QSpinBox>(dialog, "candleWickWidth");
    EXPECT_EQ(opacity->value(), 100);
    EXPECT_EQ(width->value(), 1);
    EXPECT_NE(child<QWidget>(dialog, "candlePreview"), nullptr);
    width->setValue(3);
    opacity->setValue(65);
    EXPECT_EQ(model.settings().candleWickWidth, 3);
    EXPECT_DOUBLE_EQ(model.settings().candleBodyOpacity, 0.65);
    EXPECT_EQ(t.reload().candleWickWidth, 3);
    EXPECT_EQ(t.reload().candleBodyOpacity, 0.65);
    ASSERT_TRUE(model.apply({{"candleUpColor", "#ABCDEF"}}).isEmpty());
    EXPECT_EQ(child<QPushButton>(dialog, "candleUpColor")->text(), "#ABCDEF");
    child<QPushButton>(dialog, "resetChart")->click();
    EXPECT_EQ(model.settings().candleUpColor, "#2EBD85");
    EXPECT_EQ(width->value(), 1);
    EXPECT_EQ(opacity->value(), 100);
}

TEST(CandleStyleTest, ModelChangesReachTheChartProperties) {
    TempStore t;
    HeatmapSettingsModel model(t.store, "main", t.config);
    UnifiedGridRenderer renderer;
    HeatmapChartControls controls(&model);
    controls.setRenderer(&renderer);
    ASSERT_TRUE(model.apply({{"candleUpColor", "#123ABC"}, {"candleWickColor", "#FEDCBA"},
                             {"candleBodyOpacity", 0.4}, {"candleWickWidth", 2}}).isEmpty());
    EXPECT_EQ(renderer.candleUpColor().name(), "#123abc");
    EXPECT_EQ(renderer.candleWickColor(), "#FEDCBA");
    EXPECT_DOUBLE_EQ(renderer.candleBodyOpacity(), 0.4);
    EXPECT_EQ(renderer.candleWickWidth(), 2);
}

TEST(HeatmapSettingsModelTest, NamedWorkspacesRestoreButLastSessionNever) {
    TempStore t;
    HeatmapSettingsModel model(t.store, "main", t.config);
    ASSERT_TRUE(model.apply({{"palettePreset", "Fire"}, {"liveMinIntervalMs", 900}}).isEmpty());
    model.saveLayout("Scalping");
    model.saveLayout("_last_session");
    ASSERT_TRUE(model.apply({{"palettePreset", "Matrix"}, {"liveMinIntervalMs", 300}}).isEmpty());
    QSignalSpy changed(&model, &HeatmapSettingsModel::changed);
    model.restoreLayout("_last_session"); // live per-chart settings win (INV-088)
    EXPECT_EQ(changed.count(), 0);
    EXPECT_EQ(model.settings().palettePreset, "Matrix");
    for (const auto &key : t.keys()) EXPECT_FALSE(key.startsWith("layouts/_last_session")) << key.toStdString();
    model.restoreLayout("Scalping");
    EXPECT_EQ(changed.count(), 1);
    EXPECT_EQ(model.settings().palettePreset, "Fire");
    EXPECT_EQ(model.settings().liveMinIntervalMs, 900);
    EXPECT_EQ(t.reload().palettePreset, "Fire"); // the restored workspace is the chart's settings now
    // A session-only renderer survives a workspace restore.
    ASSERT_TRUE(model.apply({{"renderer", "legacy"}}, false).isEmpty());
    model.restoreLayout("Scalping");
    EXPECT_EQ(model.settings().renderer, "legacy");
}

TEST(HeatmapSettingsModelTest, ProcessBudgetsValidateApplyAndPersist) {
    TempStore t;
    HeatmapSettingsModel model(t.store, "main", t.config);
    std::vector<heatmap::HeatmapBudgets> applied;
    bool accept = true;
    model.setBudgetSink([&](const heatmap::HeatmapBudgets &b) {
        applied.push_back(b);
        return accept;
    });
    QSignalSpy changed(&model, &HeatmapSettingsModel::budgetsChanged);
    auto b = model.budgets();
    b.decodedChunks = 600 * MiB;
    b.spanSources = 600 * MiB;
    b.cpuCeiling = 1000 * MiB; // below decoded + spans
    EXPECT_FALSE(model.setBudgets(b).isEmpty());
    EXPECT_TRUE(applied.empty());
    b.cpuCeiling = 1400 * MiB;
    accept = false; // the service refuses: nothing is saved
    EXPECT_FALSE(model.setBudgets(b).isEmpty());
    EXPECT_EQ(t.reloadBudgets().cpuCeiling, model.defaultBudgets().cpuCeiling);
    accept = true;
    EXPECT_TRUE(model.setBudgets(b).isEmpty());
    ASSERT_EQ(applied.size(), 2u);
    EXPECT_EQ(applied.back().cpuCeiling, 1400 * MiB);
    EXPECT_EQ(changed.count(), 1);
    EXPECT_EQ(t.reloadBudgets().decodedChunks, 600 * MiB);
    EXPECT_EQ(t.reloadBudgets().cpuCeiling, 1400 * MiB);
}

// ------------------------------------------------------------------ dialog
HeatmapChartSettings nonDefaults() {
    HeatmapChartSettings s;
    s.tickMode = TickMode::Manual;
    s.manualTick = 500;
    s.minRowPx = 3.5;
    s.hysteresis = 0.4;
    s.palettePreset = "Custom";
    s.bidGradient = {{0, "#000000"}, {0.5, "#103050"}, {1, "#40c0ff"}};
    s.askGradient = {{0, "#000000"}, {1, "#ff8000"}};
    s.sensitivityMin = 0.2;
    s.sensitivityMax = 20;
    s.opacity = 0.75;
    s.crossfadeMs = 0;
    s.showBandEdges = true;
    s.gpuCapBytes = 256 * MiB;
    s.uploadBudgetBytes = 4 * MiB;
    s.prefetchTiles = 3;
    s.liveMinIntervalMs = 1200;
    s.renderer = "legacy"; // gpu is the default
    s.showTelemetry = true;
    return s;
}

TEST(HeatmapSettingsDialogTest, EveryTabShowsTheModel) {
    TempStore t;
    t.store.save("main", nonDefaults());
    HeatmapSettingsModel model(t.store, "main", t.config);
    HeatmapSettingsDialog dialog(&model, nullptr);
    HeatmapChartControls controls(&model);
    controls.setDialog(&dialog);
    QStringList tabs;
    for (int i = 0; i < dialog.tabs()->count(); ++i) tabs << dialog.tabs()->tabText(i);
    EXPECT_EQ(tabs, (QStringList{"Chart", "Tick", "Look", "Budgets", "Live", "Debug", "TPO"}));
    EXPECT_EQ(child<QComboBox>(dialog, "tickMode")->currentData().toString(), "manual");
    // No chart, so nothing offered: the locked $5 shows, marked unavailable, and
    // nothing is selectable.
    auto *preset = child<QComboBox>(dialog, "manualTick");
    EXPECT_EQ(preset->currentData().toLongLong(), 500);
    EXPECT_EQ(preset->currentText(), "$5 (unavailable)");
    EXPECT_EQ(preset->count(), 1);
    EXPECT_FALSE(preset->isEnabled());
    EXPECT_EQ(child<QDoubleSpinBox>(dialog, "minRowPx")->value(), 3.5);
    EXPECT_EQ(child<QDoubleSpinBox>(dialog, "hysteresis")->value(), 0.4);
    EXPECT_EQ(child<QComboBox>(dialog, "palettePreset")->currentText(), "Custom");
    EXPECT_EQ(child<HeatmapGradientEditor>(dialog, "bidGradient")->stops(), nonDefaults().bidGradient);
    EXPECT_EQ(child<HeatmapGradientEditor>(dialog, "askGradient")->stops(), nonDefaults().askGradient);
    EXPECT_EQ(child<QDoubleSpinBox>(dialog, "sensitivityMin")->value(), 0.2);
    EXPECT_EQ(child<QDoubleSpinBox>(dialog, "sensitivityMax")->value(), 20);
    EXPECT_EQ(child<QDoubleSpinBox>(dialog, "opacity")->value(), 0.75);
    EXPECT_FALSE(child<QCheckBox>(dialog, "crossfadeOn")->isChecked());
    EXPECT_FALSE(child<QSpinBox>(dialog, "crossfadeMs")->isEnabled());
    EXPECT_TRUE(child<QCheckBox>(dialog, "showBandEdges")->isChecked());
    EXPECT_EQ(child<QSpinBox>(dialog, "gpuCapBytes")->value(), 256);
    EXPECT_EQ(child<QSpinBox>(dialog, "uploadBudgetBytes")->value(), 4096);
    EXPECT_EQ(child<QSpinBox>(dialog, "prefetchTiles")->value(), 3);
    EXPECT_EQ(child<QSpinBox>(dialog, "decodedChunks")->value(), 512);
    EXPECT_EQ(child<QSpinBox>(dialog, "spanSources")->value(), 256);
    EXPECT_EQ(child<QSpinBox>(dialog, "cpuCeiling")->value(), 1024);
    EXPECT_EQ(child<QSpinBox>(dialog, "liveMinIntervalMs")->value(), 1200);
    EXPECT_EQ(child<QComboBox>(dialog, "renderer")->currentData().toString(), "legacy");
    EXPECT_EQ(child<QLabel>(dialog, "savedRenderer")->text(), "legacy");
    EXPECT_TRUE(child<QCheckBox>(dialog, "showTelemetry")->isChecked());
}

TEST(HeatmapSettingsDialogTest, ShortDialogScrollsToLookControlsWithoutChangingPersistence) {
    TempStore t;
    HeatmapSettingsModel model(t.store, "main", t.config);
    HeatmapSettingsDialog dialog(&model, nullptr);
    dialog.resize(520, 480);
    dialog.tabs()->setCurrentIndex(2); // Look has two gradient editors
    dialog.show();
    QCoreApplication::processEvents();
    EXPECT_LE(dialog.height(), 520);
    auto *scroll = child<QScrollArea>(dialog, "lookSettingsScroll");
    auto *opacity = child<QDoubleSpinBox>(dialog, "opacity");
    ASSERT_TRUE(scroll && opacity);
    EXPECT_GT(scroll->verticalScrollBar()->maximum(), 0);
    scroll->ensureWidgetVisible(opacity);
    EXPECT_GT(scroll->verticalScrollBar()->value(), 0);
    opacity->setValue(0.6);
    EXPECT_DOUBLE_EQ(t.reload().opacity, 0.6);
    auto *reset = child<QPushButton>(dialog, "resetLook");
    scroll->ensureWidgetVisible(reset);
    EXPECT_TRUE(reset->isVisible());
    reset->click();
    EXPECT_DOUBLE_EQ(t.reload().opacity, model.defaultSettings().opacity);
    EXPECT_TRUE(child<QCheckBox>(dialog, "showBandEdges")->isHidden());
    dialog.tabs()->setCurrentIndex(3);
    EXPECT_TRUE(child<QSpinBox>(dialog, "prefetchTiles")->isHidden());
}

// Item 2 of the review: only presets loaded data builds are offered; with nothing
// loaded (Auto, no chart) the preset is an empty, disabled loading state.
TEST(HeatmapSettingsDialogTest, NoAvailabilityOffersNoPresets) {
    TempStore t;
    HeatmapSettingsModel model(t.store, "main", t.config);
    HeatmapSettingsDialog dialog(&model, nullptr);
    HeatmapChartControls controls(&model);
    controls.setDialog(&dialog);
    auto *preset = child<QComboBox>(dialog, "manualTick");
    EXPECT_EQ(preset->count(), 0); // no generic ladder, no chart-wide manualTick in Auto
    EXPECT_FALSE(preset->isEnabled());
    EXPECT_FALSE(preset->placeholderText().isEmpty());
    // Offered presets become selectable; nothing else is added.
    TopToolbar::TickSelectorState st;
    st.enabled = true;
    st.drawnUnits = 1000;
    st.offeredUnits = {1000, 2000, 5000};
    dialog.setTickSelectorState(st);
    EXPECT_EQ(preset->count(), 3);
    EXPECT_TRUE(preset->isEnabled());
    EXPECT_EQ(preset->currentData().toLongLong(), 1000);
}

// Item 2 of the review: history only on the deep $10 grid (rule 4 offers its
// presets) and the default Manual $1: the $1 shows as unavailable, and only the
// offered presets are selectable, in the dialog and the toolbar alike.
TEST(HeatmapSettingsDialogTest, DeepOnlyHistoryOffersOnlyItsPresets) {
    TempStore t;
    HeatmapSettingsModel model(t.store, "main", t.config);
    ASSERT_TRUE(model.apply({{"tickMode", "manual"}}).isEmpty());
    ASSERT_EQ(model.settings().manualTick, 100); // the configured default, $1
    HeatmapSettingsDialog dialog(&model, nullptr);
    TopToolbar toolbar;
    TopToolbar::TickSelectorState st;
    st.enabled = true;
    st.manual = true;
    st.manualUnits = model.settings().manualTick;
    st.drawnUnits = 100;
    st.offeredUnits = heatmap::manualPresetUnits(std::vector<int64_t>{1000}, 1'000'000); // deep $10 grid
    ASSERT_FALSE(st.offeredUnits.empty());
    dialog.setTickSelectorState(st);
    toolbar.setTickSelectorState(st);
    for (QComboBox *combo : {child<QComboBox>(dialog, "manualTick"), toolbar.tickPresetCombo()}) {
        EXPECT_EQ(combo->currentData().toLongLong(), 100);
        EXPECT_EQ(combo->currentText(), "$1 (unavailable)");
        auto *items = qobject_cast<QStandardItemModel *>(combo->model());
        ASSERT_NE(items, nullptr);
        std::vector<int64_t> selectable;
        for (int i = 0; i < combo->count(); ++i)
            if (items->item(i)->isEnabled()) selectable.push_back(combo->itemData(i).toLongLong());
        EXPECT_EQ(selectable, st.offeredUnits);
        for (const int64_t u : selectable) EXPECT_EQ(u % 1000, 0) << u;
    }
}

// Item 4 of the review: an identical persisted renderer patch saves the default;
// the open dialog's "Saved default" follows, and the chart does no work.
TEST(HeatmapSettingsDialogTest, SavingTheSessionRendererUpdatesSavedDefault) {
    TempStore t;
    HeatmapSettingsModel model(t.store, "main", t.config);
    HeatmapSettingsDialog dialog(&model, nullptr);
    ASSERT_TRUE(model.apply({{"renderer", "legacy"}}, false).isEmpty());
    EXPECT_EQ(child<QLabel>(dialog, "savedRenderer")->text(), "gpu");
    QSignalSpy changed(&model, &HeatmapSettingsModel::changed);
    QSignalSpy saved(&model, &HeatmapSettingsModel::savedRendererChanged);
    ASSERT_TRUE(model.apply({{"renderer", "legacy"}}, true).isEmpty()); // e.g. the API, persist:true
    EXPECT_EQ(changed.count(), 0); // the effective settings did not change
    EXPECT_EQ(saved.count(), 1);
    EXPECT_EQ(child<QLabel>(dialog, "savedRenderer")->text(), "legacy");
    EXPECT_EQ(t.reload().renderer, "legacy");
}

// Item 5 of the review: Look reset includes the renderer's tone controls; TPO has
// a reset to the configured tpo values.
TEST(HeatmapSettingsDialogTest, ResetCoversRendererBackedControls) {
    TempStore t;
    t.config.gamma = 0.9;
    t.config.contrast = 1.4;
    t.config.shaderFloor = 0.02;
    HeatmapSettingsModel model(t.store, "main", t.config);
    UnifiedGridRenderer ugr;
    ugr.setHeatmapGamma(2.5);
    ugr.setHeatmapContrast(3.0);
    ugr.setHeatmapShaderFloor(0.3);
    ugr.setTpoLayout("split");
    ugr.setTpoTheme("sage");
    ugr.setTpoSessionType(0);
    ugr.setTpoTimeframeMs(3'600'000);
    HeatmapSettingsDialog dialog(&model, &ugr);
    ClientTpoConfig tpo; // collapsed, rainbow, 24h, 30m
    dialog.setTpoDefaults(tpo);
    child<QPushButton>(dialog, "resetLook")->click();
    EXPECT_NEAR(ugr.heatmapGamma(), 0.9, 1e-9);
    EXPECT_NEAR(ugr.heatmapContrast(), 1.4, 1e-9);
    EXPECT_NEAR(ugr.heatmapShaderFloor(), 0.02, 1e-9);
    EXPECT_EQ(ugr.tpoLayout(), "split"); // other tabs untouched
    child<QPushButton>(dialog, "resetTPO")->click();
    EXPECT_EQ(ugr.tpoLayout(), "collapsed");
    EXPECT_EQ(ugr.tpoTheme(), "rainbow");
    EXPECT_EQ(ugr.tpoSessionType(), 4);
    EXPECT_EQ(ugr.tpoTimeframeMs(), 30 * 60'000);
    EXPECT_NEAR(ugr.heatmapGamma(), 0.9, 1e-9);
}

TEST(HeatmapSettingsDialogTest, EveryWidgetWritesItsSettingLiveAndSaved) {
    TempStore t;
    HeatmapSettingsModel model(t.store, "main", t.config);
    std::vector<heatmap::HeatmapBudgets> applied;
    model.setBudgetSink([&](const heatmap::HeatmapBudgets &b) { applied.push_back(b); return true; });
    HeatmapSettingsDialog dialog(&model, nullptr);
    HeatmapChartControls controls(&model); // tick actions go through the controls
    controls.setDialog(&dialog);
    QSignalSpy changed(&model, &HeatmapSettingsModel::changed);
    // Tick
    child<QComboBox>(dialog, "tickMode")->setCurrentIndex(1);
    TopToolbar::TickSelectorState offered;
    offered.enabled = true;
    offered.manual = true;
    offered.manualUnits = 100;
    offered.offeredUnits = {100, 200, 500, 1000, 2000};
    dialog.setTickSelectorState(offered); // as a chart with loaded data reports
    auto *manual = child<QComboBox>(dialog, "manualTick");
    manual->setCurrentIndex(manual->findData(qlonglong(2000)));
    emit manual->activated(manual->currentIndex());
    child<QDoubleSpinBox>(dialog, "minRowPx")->setValue(4);
    child<QDoubleSpinBox>(dialog, "hysteresis")->setValue(0.15);
    // Look
    child<QComboBox>(dialog, "palettePreset")->setCurrentText("Monochrome");
    child<HeatmapGradientEditor>(dialog, "askGradient")->setStopColor(1, "#ff0000"); // selects Custom
    child<QDoubleSpinBox>(dialog, "sensitivityMax")->setValue(80);
    child<QDoubleSpinBox>(dialog, "sensitivityMin")->setValue(0.5);
    child<QDoubleSpinBox>(dialog, "opacity")->setValue(0.6);
    child<QSpinBox>(dialog, "crossfadeMs")->setValue(400);
    child<QCheckBox>(dialog, "showBandEdges")->setChecked(true);
    // Budgets
    child<QSpinBox>(dialog, "gpuCapBytes")->setValue(128);
    child<QSpinBox>(dialog, "uploadBudgetBytes")->setValue(2048);
    child<QSpinBox>(dialog, "prefetchTiles")->setValue(4);
    child<QSpinBox>(dialog, "cpuCeiling")->setValue(2048);
    // Live
    child<QSpinBox>(dialog, "liveMinIntervalMs")->setValue(750);
    // Debug
    child<QCheckBox>(dialog, "showTelemetry")->setChecked(true);

    const auto &s = model.settings();
    EXPECT_EQ(s.tickMode, TickMode::Manual);
    EXPECT_EQ(s.manualTick, 2000);
    EXPECT_EQ(s.minRowPx, 4);
    EXPECT_EQ(s.hysteresis, 0.15);
    EXPECT_EQ(s.palettePreset, "Custom");
    EXPECT_EQ(s.askGradient.back().color, "#ff0000");
    EXPECT_EQ(s.sensitivityMin, 0.5);
    EXPECT_EQ(s.sensitivityMax, 80);
    EXPECT_EQ(s.opacity, 0.6);
    EXPECT_EQ(s.crossfadeMs, 400);
    EXPECT_TRUE(s.showBandEdges);
    EXPECT_EQ(s.gpuCapBytes, 128 * MiB);
    EXPECT_EQ(s.uploadBudgetBytes, 2 * MiB);
    EXPECT_EQ(s.prefetchTiles, 4);
    EXPECT_EQ(s.liveMinIntervalMs, 750);
    EXPECT_TRUE(s.showTelemetry);
    ASSERT_FALSE(applied.empty());
    EXPECT_EQ(applied.back().cpuCeiling, 2048 * MiB);
    EXPECT_GE(changed.count(), 15); // live: each change reached listeners at once
    EXPECT_EQ(t.reload(), s);       // and each was saved

    // Crossfade off is crossfadeMs 0 (hard switch); on restores the ms value.
    child<QCheckBox>(dialog, "crossfadeOn")->setChecked(false);
    EXPECT_EQ(model.settings().crossfadeMs, 0);
    child<QCheckBox>(dialog, "crossfadeOn")->setChecked(true);
    EXPECT_EQ(model.settings().crossfadeMs, 400);

    // Renderer: this session only unless "Make default" is ticked.
    auto *rendererCombo = child<QComboBox>(dialog, "renderer");
    EXPECT_EQ(rendererCombo->itemData(0).toString(), "gpu"); // GPU reads as the default
    rendererCombo->setCurrentIndex(rendererCombo->findData("legacy"));
    EXPECT_EQ(model.settings().renderer, "legacy");
    EXPECT_EQ(t.reload().renderer, "gpu");
    EXPECT_EQ(child<QLabel>(dialog, "savedRenderer")->text(), "gpu");
    child<QCheckBox>(dialog, "makeDefault")->setChecked(true);
    EXPECT_EQ(t.reload().renderer, "legacy");
    EXPECT_EQ(child<QLabel>(dialog, "savedRenderer")->text(), "legacy");
}

TEST(HeatmapSettingsDialogTest, ValidationUsesTheModelClampsAndRejectsBadInput) {
    TempStore t;
    HeatmapSettingsModel model(t.store, "main", t.config);
    model.setBudgetSink([](const heatmap::HeatmapBudgets &) { return true; });
    HeatmapSettingsDialog dialog(&model, nullptr);
    // Min above max: the S6a clamp raises max to 2 x min, and the widget shows it.
    child<QDoubleSpinBox>(dialog, "sensitivityMin")->setValue(100);
    EXPECT_EQ(model.settings().sensitivityMax, 200);
    EXPECT_EQ(child<QDoubleSpinBox>(dialog, "sensitivityMax")->value(), 200);
    // The upload budget never exceeds the GPU cap.
    child<QSpinBox>(dialog, "gpuCapBytes")->setValue(4);
    child<QSpinBox>(dialog, "uploadBudgetBytes")->setValue(64 * 1024);
    EXPECT_EQ(model.settings().uploadBudgetBytes, 4 * MiB);
    EXPECT_EQ(child<QSpinBox>(dialog, "uploadBudgetBytes")->value(), 4096);
    // An out-of-order gradient is refused whole: the model and the editor keep the old stops.
    auto *bid = child<HeatmapGradientEditor>(dialog, "bidGradient");
    const auto before = model.settings();
    bid->setStops({{0, "#000000"}, {0.8, "#202020"}, {1, "#00ffff"}});
    auto *middle = qobject_cast<QDoubleSpinBox *>(bid->table()->cellWidget(1, 0));
    ASSERT_NE(middle, nullptr);
    middle->setValue(1.0); // equal to the last stop: not strictly increasing
    EXPECT_EQ(model.settings().bidGradient, before.bidGradient);
    EXPECT_EQ(bid->stops(), before.bidGradient);
    EXPECT_TRUE(dialog.statusText().contains("Invalid gradient")) << dialog.statusText().toStdString();
    // Process budgets: a ceiling below decoded + spans changes nothing.
    child<QSpinBox>(dialog, "cpuCeiling")->setValue(100);
    EXPECT_EQ(model.budgets().cpuCeiling, model.defaultBudgets().cpuCeiling);
    EXPECT_TRUE(dialog.statusText().contains("CPU ceiling")) << dialog.statusText().toStdString();
}

TEST(HeatmapSettingsDialogTest, ResetRestoresOnlyItsTab) {
    TempStore t;
    t.store.save("main", nonDefaults());
    HeatmapSettingsModel model(t.store, "main", t.config);
    model.setBudgetSink([](const heatmap::HeatmapBudgets &) { return true; });
    auto b = model.budgets();
    b.cpuCeiling = 2048 * MiB;
    ASSERT_TRUE(model.setBudgets(b).isEmpty());
    HeatmapSettingsDialog dialog(&model, nullptr);
    const auto d = model.defaultSettings();
    const auto click = [&](const char *name) { child<QPushButton>(dialog, name)->click(); };

    click("resetTick");
    auto s = model.settings();
    EXPECT_EQ(s.tickMode, d.tickMode);
    EXPECT_EQ(s.manualTick, d.manualTick);
    EXPECT_EQ(s.minRowPx, d.minRowPx);
    EXPECT_EQ(s.hysteresis, d.hysteresis);
    EXPECT_EQ(s.palettePreset, "Custom"); // other tabs untouched
    EXPECT_EQ(child<QDoubleSpinBox>(dialog, "minRowPx")->value(), d.minRowPx);

    click("resetLook");
    s = model.settings();
    EXPECT_EQ(s.palettePreset, d.palettePreset);
    EXPECT_EQ(s.bidGradient, d.bidGradient);
    EXPECT_EQ(s.sensitivityMin, d.sensitivityMin);
    EXPECT_EQ(s.opacity, d.opacity);
    EXPECT_EQ(s.crossfadeMs, d.crossfadeMs);
    EXPECT_EQ(s.showBandEdges, d.showBandEdges);
    EXPECT_EQ(s.gpuCapBytes, 256 * MiB);

    click("resetBudgets");
    s = model.settings();
    EXPECT_EQ(s.gpuCapBytes, d.gpuCapBytes);
    EXPECT_EQ(s.uploadBudgetBytes, d.uploadBudgetBytes);
    EXPECT_EQ(s.prefetchTiles, d.prefetchTiles);
    EXPECT_EQ(model.budgets().cpuCeiling, model.defaultBudgets().cpuCeiling);
    EXPECT_EQ(s.liveMinIntervalMs, 1200);

    click("resetLive");
    EXPECT_EQ(model.settings().liveMinIntervalMs, d.liveMinIntervalMs);
    EXPECT_EQ(model.settings().renderer, "legacy");

    click("resetDebug");
    EXPECT_EQ(model.settings().renderer, d.renderer);
    EXPECT_EQ(model.settings().showTelemetry, d.showTelemetry);
    EXPECT_EQ(model.settings(), d);
    EXPECT_EQ(t.reload(), d); // resets are saved
}

// The Agent API path (codec validation, then the model as MainWindowGPU does):
// the dialog and the toolbar follow the settings object's changed() signal.
TEST(HeatmapSettingsDialogTest, ApiChangesReachTheDialogAndToolbar) {
    TempStore t;
    HeatmapSettingsModel model(t.store, "main", t.config);
    HeatmapSettingsDialog dialog(&model, nullptr);
    TopToolbar toolbar;
    HeatmapChartControls controls(&model);
    controls.setToolbar(&toolbar);
    controls.setDialog(&dialog);
    AgentApi::Request request{"POST", "/api/v1/heatmap/settings", {},
                              R"({"palettePreset":"Fire","tickMode":"manual","manualTick":1000,"crossfadeMs":0,
                                  "liveMinIntervalMs":2000,"persist":false})"};
    const auto validated = AgentApi::validateControl(request, std::nullopt);
    ASSERT_EQ(validated.status, 200) << validated.message.toStdString();
    ASSERT_TRUE(model.apply(validated.body.heatmapSettings, validated.body.persistHeatmapSettings).isEmpty());
    EXPECT_EQ(child<QComboBox>(dialog, "palettePreset")->currentText(), "Fire");
    EXPECT_EQ(child<QComboBox>(dialog, "tickMode")->currentData().toString(), "manual");
    EXPECT_EQ(child<QComboBox>(dialog, "manualTick")->currentData().toLongLong(), 1000);
    EXPECT_FALSE(child<QCheckBox>(dialog, "crossfadeOn")->isChecked());
    EXPECT_EQ(child<QSpinBox>(dialog, "liveMinIntervalMs")->value(), 2000);
    EXPECT_EQ(toolbar.tickModeCombo()->currentIndex(), 1); // Manual, shown though disabled (no GPU chart)
    EXPECT_EQ(toolbar.tickPresetCombo()->currentData().toLongLong(), 1000);
    EXPECT_EQ(t.reload().palettePreset, "Electric"); // persist:false
}

// Qt fixes the scene graph backend at the first QQuickWindow of the process,
// shown or not. The tests below construct plain windows, so main() selects the
// rhi backend first; otherwise every OffscreenQuick after them fails with
// "Scenegraph already initialized, setBackend() request ignored".
TEST(OffscreenQuickTest, CreatesAfterAPlainQuickWindowBecauseMainSelectedTheBackend) {
    if (const QString why = lab::gpuUnavailableReason(); !why.isEmpty())
        GTEST_SKIP() << "GPU case skipped: " << why.toStdString();
    QQuickWindow plain; // never shown; still initializes the scene graph adaptation
    lab::OffscreenQuick scene;
    QString error;
    EXPECT_TRUE(scene.create(QSize(64, 64), &error)) << error.toStdString();
}

TEST(TradeBubbleControls, SiblingOverlayConsumesLatestFrameAndOwnsTypedNode) {
    UnifiedGridRenderer renderer;
    renderer.setActiveSymbol("BTC-USD");
    HeatmapChartSettings settings; settings.showTrades=true;
    renderer.setHeatmapChartSettings(settings);
    Trade trade{}; trade.product_id="BTC-USD"; trade.price=150; trade.size=1; trade.side=AggressorSide::Buy;
    trade.timestamp=std::chrono::system_clock::time_point(std::chrono::milliseconds(1500));
    renderer.onTradeReceived(trade);
    QQuickWindow window, secondWindow; // No showing, rendering, Metal or GPU needed.
    TradeBubbleOverlayItem overlay;
    overlay.setParentItem(window.contentItem());
    overlay.setRenderer(&renderer);
    auto link=TradeBubbleOverlayTest::link(overlay);
    auto* root=TradeBubbleOverlayTest::root(overlay,nullptr); // Item synchronized first.
    ASSERT_NE(link->node,nullptr);
    auto* original=link->node;
    root->prependChildNode(new QSGNode); // Sibling order is irrelevant.
    TimeAxisMapping m; m.valid=true; m.drawRect={0,0,640,320}; m.srcRect={0,0,10,100};
    m.viewStartMs=m.dataStartMs=1000; m.viewEndMs=2000; m.appendMs=100;
    m.viewMinPrice=100; m.viewMaxPrice=m.dataMaxPrice=200; m.tickSize=1;
    TradeBubbleRendererTest::publish(renderer,m); // UGR synchronized afterwards.
    ASSERT_TRUE(QMetaObject::invokeMethod(&window,"afterSynchronizing",Qt::DirectConnection));
    EXPECT_EQ(link->node,original);
    ASSERT_EQ(original->usedVertexCount(),6);
    EXPECT_NEAR(static_cast<const float*>(original->geometry()->vertexData())[0],320-trade_bubbles::Layout::radius(150),1e-4);
    m.srcRect.translate(1,0); m.viewStartMs+=100; m.viewEndMs+=100;
    TradeBubbleRendererTest::publish(renderer,m);
    ASSERT_TRUE(QMetaObject::invokeMethod(&window,"afterSynchronizing",Qt::DirectConnection));
    EXPECT_FLOAT_EQ(original->translation()(0,3),-64);
    EXPECT_EQ(original->rebuildCount(),1);
    EXPECT_EQ(TradeBubbleOverlayTest::root(overlay,root),root);
    delete root;
    EXPECT_EQ(link->node,nullptr);
    ASSERT_TRUE(QMetaObject::invokeMethod(&window,"afterSynchronizing",Qt::DirectConnection));
    root=TradeBubbleOverlayTest::root(overlay,nullptr);
    ASSERT_TRUE(QMetaObject::invokeMethod(&window,"afterSynchronizing",Qt::DirectConnection));
    EXPECT_EQ(link->node->usedVertexCount(),6);
    overlay.setParentItem(secondWindow.contentItem());
    auto secondLink=TradeBubbleOverlayTest::link(overlay);
    EXPECT_NE(secondLink,link);
    root=TradeBubbleOverlayTest::root(overlay,root);
    EXPECT_EQ(link->node,nullptr);
    ASSERT_NE(secondLink->node,nullptr);
    ASSERT_TRUE(QMetaObject::invokeMethod(&secondWindow,"afterSynchronizing",Qt::DirectConnection));
    EXPECT_EQ(secondLink->node->usedVertexCount(),6);
    link=secondLink;
    overlay.setRenderer(nullptr);
    ASSERT_TRUE(QMetaObject::invokeMethod(&secondWindow,"afterSynchronizing",Qt::DirectConnection));
    EXPECT_EQ(link->node->usedVertexCount(),0);
    delete root;
    overlay.setParentItem(nullptr);
}

TEST(TradeBubbleControls, LayerOrderUsesRealQmlBindingAndPersistedChartApiSetting) {
    TempStore t;
    HeatmapSettingsModel model(t.store,"main",t.config);
    UnifiedGridRenderer renderer;
    HeatmapChartControls controls(&model); controls.setRenderer(&renderer);
    HeatmapSettingsDialog dialog(&model,&renderer);
    auto* check=child<QCheckBox>(dialog,"tradesAboveCandles");
    ASSERT_NE(check,nullptr);
    EXPECT_TRUE(check->isChecked());
    EXPECT_TRUE(renderer.tradesAboveCandles());
    QFile source(QStringLiteral(SENTINEL_SOURCE_DIR "/libs/gui/qml/DepthChartView.qml"));
    ASSERT_TRUE(source.open(QIODevice::ReadOnly));
    const auto qml=source.readAll();
    const auto start=qml.indexOf("    TradeBubbleOverlayItem {");
    ASSERT_GE(start,0);
    const auto end=qml.indexOf("\n    }",start);
    ASSERT_GT(end,start);
    // Execute the actual item's bindings, without loading unrelated chart services.
    qmlRegisterType<TradeBubbleOverlayItem>("Sentinel.Charts",1,0,"TradeBubbleOverlayItem");
    QQmlEngine engine; engine.rootContext()->setContextProperty("unifiedGridRenderer",&renderer);
    QQmlComponent component(&engine);
    component.setData("import QtQuick\nimport Sentinel.Charts 1.0\nItem {\n"+qml.mid(start,end-start+6)+"\n}",QUrl());
    std::unique_ptr<QObject> root(component.create());
    ASSERT_NE(root,nullptr) << component.errorString().toStdString();
    auto* overlay=root->findChild<TradeBubbleOverlayItem*>("tradeBubbleOverlay");
    ASSERT_NE(overlay,nullptr);
    EXPECT_DOUBLE_EQ(overlay->z(),2.5);
    check->setChecked(false);
    EXPECT_FALSE(model.settings().tradesAboveCandles);
    EXPECT_FALSE(t.reload().tradesAboveCandles);
    EXPECT_FALSE(renderer.tradesAboveCandles());
    EXPECT_DOUBLE_EQ(overlay->z(),1.5);
    const AgentApi::Request request{"POST","/api/v1/heatmap/settings",{},
        R"({"tradesAboveCandles":true,"persist":false})"};
    const auto validated=AgentApi::validateControl(request,std::nullopt);
    ASSERT_EQ(validated.status,200);
    ASSERT_TRUE(model.apply(validated.body.heatmapSettings,validated.body.persistHeatmapSettings).isEmpty());
    EXPECT_TRUE(check->isChecked());
    EXPECT_DOUBLE_EQ(overlay->z(),2.5);
    EXPECT_FALSE(t.reload().tradesAboveCandles); // transient API patch
    EXPECT_TRUE(t.reload("other").tradesAboveCandles);
    EXPECT_FALSE(model.apply({{"tradesAboveCandles","yes"}}).isEmpty());
    check->setChecked(false);
    child<QPushButton>(dialog,"resetChart")->click();
    EXPECT_TRUE(check->isChecked());
    EXPECT_TRUE(t.reload().tradesAboveCandles);
    ASSERT_TRUE(model.apply({{"renderer","legacy"}},false).isEmpty());
    EXPECT_FALSE(check->isEnabled());
}

TEST(TradeBubbleControls, GuiIngestionKeepsHiddenSessionTradesAndIsolatesSymbols) {
    UnifiedGridRenderer renderer;
    renderer.setActiveSymbol("BTC-USD");
    Trade t{};
    t.product_id = "BTC-USD"; t.price = 100; t.size = 2; t.side = AggressorSide::Buy;
    t.trade_id = "live-123";
    t.timestamp = std::chrono::system_clock::time_point(std::chrono::milliseconds(1791030896789LL));
    EXPECT_FALSE(TradeBubbleRendererTest::enabled(renderer));
    renderer.onTradeReceived(t);
    renderer.onTradeReceived(t); // Subscribe/reconnect snapshot replay of same ID.
    ASSERT_EQ(TradeBubbleRendererTest::count(renderer), 1);
    EXPECT_EQ(TradeBubbleRendererTest::firstTime(renderer), 1791030896789LL);
    heatmap::HeatmapChartSettings settings;
    settings.showTrades = true; settings.tradeMinNotional = 1000;
    renderer.setHeatmapChartSettings(settings);
    EXPECT_TRUE(TradeBubbleRendererTest::enabled(renderer));
    EXPECT_EQ(TradeBubbleRendererTest::threshold(renderer), 1000);
    EXPECT_EQ(TradeBubbleRendererTest::count(renderer), 1);
    renderer.setTimeframe(300000);
    EXPECT_EQ(TradeBubbleRendererTest::count(renderer), 1);
    t.product_id = "ETH-USD";
    renderer.onTradeReceived(t);
    EXPECT_EQ(TradeBubbleRendererTest::count(renderer), 1);
    renderer.setActiveSymbol("ETH-USD");
    EXPECT_EQ(TradeBubbleRendererTest::count(renderer), 0);
    renderer.onTradeReceived(t);
    EXPECT_EQ(TradeBubbleRendererTest::count(renderer), 1);
}

TEST(TradeBubbleControls, GearTogglePresetsAndApiSharePersistedChartSettings) {
    TempStore t;
    HeatmapSettingsModel model(t.store, "main", t.config);
    TopToolbar toolbar;
    HeatmapChartControls controls(&model);
    controls.setToolbar(&toolbar);
    auto* toggle = toolbar.chartMenu()->findChild<QAction*>("chartMenuTrades");
    ASSERT_NE(toggle, nullptr);
    EXPECT_FALSE(toggle->isChecked());
    toggle->trigger();
    EXPECT_TRUE(model.settings().showTrades);
    EXPECT_TRUE(t.reload().showTrades);
    auto* sizes = toolbar.chartMenu()->findChild<QMenu*>("chartMenuTradeSize");
    ASSERT_NE(sizes, nullptr);
    auto actions = sizes->actions();
    ASSERT_EQ(actions.size(), 5);
    actions[2]->trigger();
    EXPECT_EQ(model.settings().tradeMinNotional, 1000);
    EXPECT_EQ(t.reload().tradeMinNotional, 1000);
    AgentApi::Request request{"POST", "/api/v1/heatmap/settings", {},
        R"({"showTrades":false,"tradeMinNotional":10000,"persist":false})"};
    const auto validated = AgentApi::validateControl(request, std::nullopt);
    ASSERT_EQ(validated.status, 200);
    ASSERT_TRUE(model.apply(validated.body.heatmapSettings, validated.body.persistHeatmapSettings).isEmpty());
    controls.refreshChartMenu();
    EXPECT_FALSE(toggle->isChecked());
    EXPECT_TRUE(actions[3]->isChecked());
    EXPECT_TRUE(t.reload().showTrades);
    EXPECT_EQ(t.reload().tradeMinNotional, 1000);
    ASSERT_TRUE(model.apply({{"renderer", "legacy"}}, false).isEmpty());
    controls.refreshChartMenu();
    EXPECT_FALSE(toggle->isEnabled());
    EXPECT_FALSE(sizes->menuAction()->isEnabled());
    ASSERT_TRUE(model.apply({{"renderer", "gpu"}}, false).isEmpty());
    controls.refreshChartMenu();
    EXPECT_TRUE(sizes->menuAction()->isEnabled());
}

// ----------------------------------------------------------- telemetry dock
TEST(HeatmapTelemetryDockTest, PollsAt4HzOnlyWhileVisible) {
    HeatmapTelemetryDock dock;
    int calls = 0;
    dock.setProvider([&]() -> std::optional<QVariantMap> {
        ++calls;
        return QVariantMap{{"mode", "auto"}, {"tick", 5.0}, {"residentBytes", 3.0 * MiB}};
    });
    EXPECT_EQ(dock.timer()->interval(), 250);
    EXPECT_FALSE(dock.timer()->isActive()); // hidden: no polling
    calls = 0;
    dock.show();
    QElapsedTimer clock;
    clock.start();
    QTest::qWait(1100);
    const double expected = 1 + double(clock.elapsed()) / 250.0; // one on show, then the timer
    EXPECT_GE(calls, int(expected) - 1);
    EXPECT_LE(calls, int(expected) + 1);
    EXPECT_EQ(dock.valueText("tick"), "$5");
    EXPECT_EQ(dock.valueText("residentBytes").left(6), "3.0 MB");
    EXPECT_FALSE(dock.disabledShown());
    dock.hide();
    const int hidden = calls;
    QTest::qWait(600);
    EXPECT_EQ(calls, hidden);
    // nullopt (legacy renderer): the disabled state.
    dock.setProvider([] { return std::optional<QVariantMap>{}; });
    EXPECT_TRUE(dock.disabledShown());
}

TEST(HeatmapTelemetryDockTest, VisibilityFollowsShowTelemetry) {
    TempStore t;
    HeatmapSettingsModel model(t.store, "main", t.config);
    HeatmapChartControls controls(&model);
    HeatmapTelemetryDock dock;
    dock.hide();
    controls.setTelemetryDock(&dock);
    EXPECT_TRUE(dock.isHidden());
    ASSERT_TRUE(model.apply({{"showTelemetry", true}}).isEmpty()); // e.g. the API or the Debug tab
    EXPECT_FALSE(dock.isHidden());
    dock.close(); // the title-bar close button
    EXPECT_FALSE(model.settings().showTelemetry);
    EXPECT_FALSE(t.reload().showTelemetry);
    dock.toggleViewAction()->trigger(); // View menu
    EXPECT_FALSE(dock.isHidden());
    EXPECT_TRUE(model.settings().showTelemetry);
    EXPECT_TRUE(t.reload().showTelemetry);
}

// Item 3 of the review: a tabified dock polls only while its tab is current, and
// the API's telemetryVisible is that exposure, not the saved preference.
TEST(HeatmapTelemetryDockTest, TabifiedBehindAnotherTabItDoesNotPoll) {
    TempStore t;
    HeatmapSettingsModel model(t.store, "main", t.config);
    HeatmapChartControls controls(&model);
    QMainWindow window;
    window.setCentralWidget(new QWidget);
    auto *other = new QDockWidget("Other", &window);
    other->setObjectName("other");
    other->setWidget(new QLabel("other"));
    auto *dock = new HeatmapTelemetryDock(&window);
    window.addDockWidget(Qt::RightDockWidgetArea, other);
    window.addDockWidget(Qt::RightDockWidgetArea, dock);
    window.tabifyDockWidget(other, dock);
    controls.setTelemetryDock(dock);
    ASSERT_TRUE(model.apply({{"showTelemetry", true}}).isEmpty());
    window.resize(1200, 800);
    window.show();
    dock->raise();
    QTest::qWait(50);
    ASSERT_TRUE(dock->exposed());
    EXPECT_TRUE(dock->timer()->isActive());
    EXPECT_TRUE(controls.uiState()["telemetryVisible"].toBool());
    other->raise(); // the user selects the other tab
    QTest::qWait(50);
    EXPECT_FALSE(dock->exposed());
    EXPECT_FALSE(dock->timer()->isActive());
    const uint64_t polls = dock->refreshCount();
    QTest::qWait(600);
    EXPECT_EQ(dock->refreshCount(), polls);
    EXPECT_FALSE(controls.uiState()["telemetryVisible"].toBool());
    EXPECT_TRUE(controls.uiState()["telemetryPreferred"].toBool());
    EXPECT_TRUE(model.settings().showTelemetry); // a tab switch is not a preference change
    ASSERT_TRUE(model.apply({{"opacity", 0.5}}).isEmpty()); // an unrelated change does not raise it
    QTest::qWait(50);
    EXPECT_FALSE(dock->exposed());
    dock->raise();
    QTest::qWait(50);
    EXPECT_TRUE(dock->exposed());
    EXPECT_TRUE(dock->timer()->isActive());
}

// --------------------------------------------------------- GPU chart cases
QTemporaryDir *fixtureDir = nullptr;

class HeatmapChartUi : public testing::Test {
protected:
    std::unique_ptr<lab::OffscreenQuick> scene;
    UnifiedGridRenderer *ugr = nullptr;
    std::unique_ptr<TempStore> temp;
    std::unique_ptr<HeatmapSettingsModel> model;
    std::unique_ptr<HeatmapChartControls> controls;
    std::unique_ptr<TopToolbar> toolbar;
    QString error;
    QImage image;
    const int64_t viewLo = epoch + 2 * kHourMs, viewHi = epoch + 2 * kHourMs + 40 * minute;
    void SetUp() override {
        if (const QString why = lab::gpuUnavailableReason(); !why.isEmpty())
            GTEST_SKIP() << "GPU case skipped: " << why.toStdString();
        scene = std::make_unique<lab::OffscreenQuick>();
        ASSERT_TRUE(scene->create(QSize(640, 320), &error)) << error.toStdString();
        ugr = new UnifiedGridRenderer(scene->window()->contentItem());
        ugr->setSize(QSizeF(640, 320));
        ugr->setHeatmapService(&lab::LabData::instance().service());
        ugr->setActiveSymbol("BTC-USD");
        ugr->setTimeframe(int(minute));
        temp = std::make_unique<TempStore>();
        model = std::make_unique<HeatmapSettingsModel>(temp->store, "main", temp->config);
        // gpu is the default; these cases start from the legacy renderer and flip with gpuOn().
        ASSERT_TRUE(model->apply({{"renderer", "legacy"}}, false).isEmpty());
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
    void gpuOn() {
        ASSERT_TRUE(model->apply({{"renderer", "gpu"}}, false).isEmpty());
        ugr->setViewport(viewLo, viewHi, 99'900, 100'300);
    }
    template <class Done> bool pump(int ms, Done done) {
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < ms) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            ugr->update();
            image = scene->renderFrame(&error);
            if (image.isNull()) return false;
            if (done()) return true;
        }
        return false;
    }
    bool settle() {
        int streak = 0;
        return pump(30'000, [&] {
            const auto &st = layer().tileStats();
            streak = layer().settled() && !st.crossfading.load() && !st.holding.load() ? streak + 1 : 0;
            return streak >= 3;
        });
    }
    // Lets the controls' coalescing timers run (0 ms sync, 250 ms indicator).
    void syncUi() {
        pump(300, [] { return false; });
    }
    std::vector<int64_t> comboUnits() const {
        std::vector<int64_t> out;
        for (int i = 0; i < toolbar->tickPresetCombo()->count(); ++i)
            out.push_back(toolbar->tickPresetCombo()->itemData(i).toLongLong());
        return out;
    }
    void pick(int64_t units) {
        const int index = toolbar->tickPresetCombo()->findData(qlonglong(units));
        ASSERT_GE(index, 0) << units;
        toolbar->tickPresetCombo()->setCurrentIndex(index);
        emit toolbar->tickPresetCombo()->activated(index); // as a user's pick
    }
};

TEST_F(HeatmapChartUi, TickSelectorIsDisabledInLegacyWithAReason) {
    EXPECT_FALSE(ugr->gpuHeatmapActive());
    EXPECT_FALSE(toolbar->tickModeCombo()->isEnabled());
    EXPECT_FALSE(toolbar->tickPresetCombo()->isEnabled());
    EXPECT_TRUE(toolbar->tickModeCombo()->toolTip().contains("GPU heatmap renderer"));
    gpuOn();
    ASSERT_TRUE(settle()) << error.toStdString();
    syncUi();
    EXPECT_TRUE(toolbar->tickModeCombo()->isEnabled());
    EXPECT_TRUE(toolbar->tickPresetCombo()->isEnabled());
    ASSERT_TRUE(model->apply({{"renderer", "legacy"}}, false).isEmpty());
    syncUi();
    EXPECT_FALSE(toolbar->tickModeCombo()->isEnabled());
}

TEST_F(HeatmapChartUi, AutoShowsTheDrawnTickAndPresetsFollowTheLoadedData) {
    gpuOn();
    EXPECT_TRUE(layer().offeredTickUnits().empty()); // nothing loaded yet
    ASSERT_TRUE(settle()) << error.toStdString();
    syncUi();
    EXPECT_EQ(toolbar->tickModeCombo()->currentIndex(), 0);
    const int64_t drawn = layer().tickUnits();
    ASSERT_GT(drawn, 0);
    EXPECT_EQ(toolbar->tickPresetCombo()->currentData().toLongLong(), drawn);
    EXPECT_EQ(toolbar->tickPresetCombo()->currentText(), TopToolbar::tickText(drawn, layer().priceScale()));
    // Rule 4 option B: what some loaded data builds; the finest is the near $1 grid.
    const auto offered = layer().offeredTickUnits();
    ASSERT_FALSE(offered.empty());
    EXPECT_EQ(comboUnits(), offered);
    EXPECT_EQ(offered.front(), 100);
    for (const int64_t u : offered) EXPECT_EQ(u % 100, 0) << u;
    // Zooming out in price makes Auto coarser; the toolbar follows the drawn tick.
    ugr->setViewport(viewLo, viewHi, 95'000, 105'000);
    ASSERT_TRUE(settle()) << error.toStdString();
    syncUi();
    EXPECT_GT(layer().tickUnits(), drawn);
    EXPECT_EQ(toolbar->tickPresetCombo()->currentData().toLongLong(), layer().tickUnits());
}

TEST_F(HeatmapChartUi, ManualLockIsRememberedPerSymbolAndTimeframe) {
    gpuOn();
    ASSERT_TRUE(settle()) << error.toStdString();
    syncUi();
    pick(500);
    EXPECT_EQ(model->settings().tickMode, TickMode::Manual);
    EXPECT_EQ(layer().manualTickUnits(), 500);
    ASSERT_TRUE(settle()) << error.toStdString();
    EXPECT_EQ(layer().tickUnits(), 500);
    syncUi();
    EXPECT_EQ(toolbar->tickModeCombo()->currentIndex(), 1);
    EXPECT_EQ(toolbar->tickPresetCombo()->currentData().toLongLong(), 500);

    ugr->setTimeframe(int(5 * minute));
    syncUi();
    pick(1000);
    ugr->setTimeframe(int(minute)); // back: this timeframe's lock
    syncUi();
    EXPECT_EQ(layer().manualTickUnits(), 500);
    EXPECT_EQ(toolbar->tickPresetCombo()->currentData().toLongLong(), 500);
    ugr->setTimeframe(int(5 * minute));
    syncUi();
    EXPECT_EQ(toolbar->tickPresetCombo()->currentData().toLongLong(), 1000);
    const auto saved = temp->reloadTicks(); // shared by every chart (owner decision 5)
    EXPECT_EQ(saved.get("BTC-USD", minute).value_or(0), 500);
    EXPECT_EQ(saved.get("BTC-USD", 5 * minute).value_or(0), 1000);
}

// Rule 2: a locked $1 over rows only the deep $10 grid covers is veiled, and the
// toolbar says so (the resolution indicator as the veil label's tooltip).
TEST_F(HeatmapChartUi, TheToolbarNamesVeiledAvailability) {
    gpuOn();
    ASSERT_TRUE(settle()) << error.toStdString();
    syncUi();
    EXPECT_FALSE(toolbar->tickVeilLabel()->isVisibleTo(toolbar.get()));
    pick(100);
    ugr->setViewport(viewLo, viewHi, 100'400, 100'700); // above the near band, inside the deep book
    ASSERT_TRUE(settle()) << error.toStdString();
    syncUi();
    const QString indicator = layer().resolutionIndicator();
    ASSERT_FALSE(indicator.isEmpty());
    EXPECT_TRUE(toolbar->tickVeilLabel()->isVisibleTo(toolbar.get()));
    EXPECT_EQ(toolbar->tickVeilLabel()->toolTip(), indicator);
    EXPECT_TRUE(toolbar->tickPresetCombo()->toolTip().contains(indicator));
    EXPECT_EQ(controls->uiState()["toolbar"].toObject()["veiled"].toBool(), true);
}

// Item 1 of the review: the dialog's tick actions are the toolbar's. A preset
// picked in Auto locks Manual at once (no remembered tick replaces it); entering
// Manual restores the remembered tick, or locks the drawn one when none.
TEST_F(HeatmapChartUi, DialogTickActionsAreTheToolbars) {
    HeatmapSettingsDialog dialog(model.get(), ugr);
    controls->setDialog(&dialog);
    auto *mode = child<QComboBox>(dialog, "tickMode");
    auto *preset = child<QComboBox>(dialog, "manualTick");
    const auto pickInDialog = [&](int64_t units) {
        const int index = preset->findData(qlonglong(units));
        ASSERT_GE(index, 0) << units;
        preset->setCurrentIndex(index);
        emit preset->activated(index); // a user's pick (also of the shown tick)
    };
    gpuOn();
    ASSERT_TRUE(settle()) << error.toStdString();
    syncUi();
    pickInDialog(500); // remember $5 for BTC/1m
    EXPECT_EQ(layer().manualTickUnits(), 500);
    mode->setCurrentIndex(0); // back to Auto
    EXPECT_FALSE(layer().manualMode());
    syncUi();
    pickInDialog(2000); // $20 in Auto: Manual $20, not the remembered $5
    EXPECT_EQ(model->settings().tickMode, TickMode::Manual);
    EXPECT_EQ(layer().manualTickUnits(), 2000);
    EXPECT_EQ(mode->currentIndex(), 1);
    EXPECT_EQ(temp->reloadTicks().get("BTC-USD", minute).value_or(0), 2000);
    mode->setCurrentIndex(0);
    mode->setCurrentIndex(1); // Manual again: the remembered $20
    EXPECT_EQ(layer().manualTickUnits(), 2000);
    // 5m has nothing remembered: entering Manual from the dialog locks the drawn tick.
    mode->setCurrentIndex(0);
    ugr->setTimeframe(int(5 * minute));
    ASSERT_TRUE(settle()) << error.toStdString();
    const int64_t drawn = layer().tickUnits();
    ASSERT_GT(drawn, 0);
    ASSERT_NE(drawn, model->settings().manualTick); // the chart-wide value would differ
    mode->setCurrentIndex(1);
    EXPECT_EQ(layer().manualTickUnits(), drawn);
    EXPECT_EQ(temp->reloadTicks().get("BTC-USD", 5 * minute).value_or(0), drawn);
}

TEST_F(HeatmapChartUi, EnteringManualLocksTheDrawnTick) {
    gpuOn();
    ASSERT_TRUE(settle()) << error.toStdString();
    syncUi();
    const int64_t drawn = layer().tickUnits();
    ASSERT_GT(drawn, 0);
    toolbar->tickModeCombo()->setCurrentIndex(1);
    emit toolbar->tickModeCombo()->activated(1);
    EXPECT_EQ(model->settings().tickMode, TickMode::Manual);
    EXPECT_EQ(layer().manualTickUnits(), drawn); // no jump
    EXPECT_EQ(temp->reloadTicks().get("BTC-USD", minute).value_or(0), drawn);
    syncUi();
    EXPECT_EQ(toolbar->tickPresetCombo()->currentData().toLongLong(), drawn);
}

TEST_F(HeatmapChartUi, TelemetryDockShowsTheLayerAt4HzWithNoPerFrameWork) {
    HeatmapTelemetryDock dock;
    controls->setTelemetryDock(&dock);
    ASSERT_TRUE(model->apply({{"showTelemetry", true}}).isEmpty());
    ASSERT_TRUE(dock.isVisible());
    ASSERT_TRUE(dock.timer()->isActive());
    dock.refresh();
    EXPECT_TRUE(dock.disabledShown()); // legacy renderer
    gpuOn();
    ASSERT_TRUE(settle()) << error.toStdString();
    dock.refresh();
    EXPECT_FALSE(dock.disabledShown());
    EXPECT_EQ(dock.valueText("mode"), "auto");
    EXPECT_EQ(dock.valueText("tick"), TopToolbar::tickText(layer().tickUnits(), layer().priceScale()));
    EXPECT_NE(dock.valueText("residentBytes").left(6), "0.0 MB");
    EXPECT_EQ(dock.valueText("settled"), "yes");
    // Frames do not drive the dock: only its 4 Hz timer does.
    const uint64_t before = dock.refreshCount();
    QElapsedTimer clock;
    clock.start();
    int frames = 0;
    pump(60'000, [&] { return ++frames >= 120 && clock.elapsed() >= 1000; });
    const double elapsed = double(clock.elapsed());
    const uint64_t polls = dock.refreshCount() - before;
    EXPECT_GE(frames, 120);
    EXPECT_LE(double(polls), elapsed / 250.0 + 2) << "frames=" << frames << " ms=" << elapsed;
    EXPECT_GE(double(polls), elapsed / 250.0 - 2) << "frames=" << frames << " ms=" << elapsed;
}

TEST_F(HeatmapChartUi, LiveMinIntervalReachesTheController) {
    gpuOn();
    ASSERT_TRUE(model->apply({{"liveMinIntervalMs", 1500}}).isEmpty());
    auto *controller = layer().controller();
    ASSERT_NE(controller, nullptr);
    int seen = 0;
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < 5000 && seen != 1500) {
        QCoreApplication::processEvents();
        lab::LabData::instance().service().onData([&] { seen = controller->liveMinInterval(); });
    }
    EXPECT_EQ(seen, 1500);
}
} // namespace

int main(int argc, char **argv) {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    lab::selectQuickSceneGraph(); // before any QQuickWindow: Qt fixes the backend at the first one
    ::testing::InitGoogleTest(&argc, argv);
    std::cout << "[sentinel] " << lab::describeRhi().toStdString() << std::endl;
    fixtureDir = new QTemporaryDir;
    writeRecording(*fixtureDir, 4 * 60);
    lab::LabData::configure(fixtureDir->path().toStdString(), epoch + 4 * kHourMs);
    return RUN_ALL_TESTS();
}
