#include "HeatmapChartControls.hpp"
#include "PerformanceMonitor.hpp"
#include "SentinelLogging.hpp"
#include "UnifiedGridRenderer.h"
#include "render/heatmap/HeatmapGpuLayer.hpp"
#include "widgets/HeatmapSettingsDialog.hpp"
#include "widgets/HeatmapTelemetryDock.hpp"
#include <QAction>
#include <QActionGroup>
#include <QJsonArray>
#include <QMenu>
#include <QTimer>

HeatmapChartControls::HeatmapChartControls(heatmap::HeatmapSettingsModel *model, QObject *parent)
    : QObject(parent), m_model(model) {
    m_syncTimer = new QTimer(this);
    m_syncTimer->setSingleShot(true);
    m_syncTimer->setInterval(0);
    connect(m_syncTimer, &QTimer::timeout, this, &HeatmapChartControls::syncNow);
    m_indicatorTimer = new QTimer(this);
    m_indicatorTimer->setSingleShot(true);
    m_indicatorTimer->setInterval(kIndicatorMs);
    connect(m_indicatorTimer, &QTimer::timeout, this, &HeatmapChartControls::syncNow);
    connect(m_model, &heatmap::HeatmapSettingsModel::changed, this, &HeatmapChartControls::onModelChanged);
    // The Manual tick memory follows the chart's symbol and timeframe.
    m_model->setContextProvider([this] {
        heatmap::HeatmapSettingsModel::Context context;
        if (const auto *layer = m_renderer ? m_renderer->gpuHeatmapLayer() : nullptr) {
            context.symbol = layer->symbol();
            context.tfMs = layer->tfMs();
        }
        return context;
    });
}

void HeatmapChartControls::setRenderer(UnifiedGridRenderer *renderer) {
    if (m_renderer == renderer) return;
    if (m_renderer) disconnect(m_renderer, nullptr, this, nullptr);
    m_renderer = renderer;
    if (!renderer) {
        syncNow();
        return;
    }
    renderer->setHeatmapTickMemory(m_model->manualTicks());
    renderer->setHeatmapChartSettings(m_model->settings());
    renderer->setCandleAppearance(m_model->settings());
    connect(renderer, &UnifiedGridRenderer::timeframeChanged, this, &HeatmapChartControls::scheduleSync);
    connect(renderer, &UnifiedGridRenderer::viewportChanged, this, &HeatmapChartControls::scheduleIndicator);
    connect(renderer, &UnifiedGridRenderer::layerVisibilityChanged, this, &HeatmapChartControls::scheduleSync);
    connect(renderer, &UnifiedGridRenderer::tpoConfigChanged, this, &HeatmapChartControls::scheduleSync);
    connect(renderer, &UnifiedGridRenderer::tpoStyleChanged, this, &HeatmapChartControls::scheduleSync);
    connect(renderer, &UnifiedGridRenderer::candleStyleChanged, this, &HeatmapChartControls::scheduleSync);
    if (auto *layer = renderer->gpuHeatmapLayer()) {
        using Layer = heatmap::gpu::HeatmapGpuLayer;
        connect(layer, &Layer::tickChanged, this, &HeatmapChartControls::scheduleSync);
        connect(layer, &Layer::presetsChanged, this, &HeatmapChartControls::scheduleSync);
        connect(layer, &Layer::snapshotChanged, this, &HeatmapChartControls::scheduleIndicator);
        connect(layer, &Layer::liquidityRangeChanged, this, &HeatmapChartControls::scheduleIndicator);
    }
    if (m_dock) setTelemetryDock(m_dock); // the provider reads the new renderer
    syncNow();
}

void HeatmapChartControls::setToolbar(TopToolbar *toolbar) {
    if (m_toolbar) disconnect(m_toolbar, nullptr, this, nullptr);
    m_toolbar = toolbar;
    if (!toolbar) return;
    connect(toolbar, &TopToolbar::tickModeRequested, this, &HeatmapChartControls::requestTickMode);
    connect(toolbar, &TopToolbar::tickPresetRequested, this, [this](qint64 units) { requestTickPreset(units); });
    connect(toolbar, &TopToolbar::liquidityRangeEdited, this, &HeatmapChartControls::requestLiquidityRange);
    connect(toolbar, &TopToolbar::labelsToggled, this, &HeatmapChartControls::requestLabels);
    connect(toolbar, &TopToolbar::liquidityLabelModeChanged, this, [this](int mode) { requestLabelCurrency(mode == 1); });
    connect(toolbar, &TopToolbar::candlesToggled, this, &HeatmapChartControls::scheduleSync);
    connect(toolbar, &TopToolbar::tpoSessionSelected, this, [this](int session) {
        if (m_renderer) m_renderer->setTpoSessionType(session);
    });
    connect(toolbar, &TopToolbar::tpoLayoutSelected, this, [this](const QString &layout) {
        if (m_renderer) m_renderer->setTpoLayout(layout);
    });
    connect(toolbar, &TopToolbar::chartTypeSelected, this, [this, toolbar](const QString &style) {
        requestCandleStyle(toolbar->chartTypeCombo()->findText(style));
    });
    buildChartMenu();
    syncNow();
}

void HeatmapChartControls::setMenuHooks(MenuHooks hooks) {
    m_hooks = std::move(hooks);
    buildChartMenu();
}

TopToolbar::ModeState HeatmapChartControls::modeState() const {
    TopToolbar::ModeState mode;
    if (m_renderer) {
        mode.heatmap = m_renderer->heatmapLayerEnabled();
        mode.footprint = m_renderer->footprintLayerEnabled();
        mode.tpo = m_renderer->tpoLayerEnabled();
        mode.volumeProfile = m_renderer->volumeProfileLayerEnabled();
    }
    mode.candles = m_toolbar ? m_toolbar->candlesChecked() : true;
    return mode;
}

void HeatmapChartControls::requestLiquidityRange(double low, double high, bool persist) {
    // Process-only while dragging, persisted on release (one write per edit).
    if (const auto error = m_model->apply({{"sensitivityMin", low}, {"sensitivityMax", high}}, persist); !error.isEmpty())
        sLog_Warning("Liquidity range rejected: " << error);
}

void HeatmapChartControls::requestLabels(bool show) {
    if (const auto error = m_model->apply({{"showLabels", show}}); !error.isEmpty())
        sLog_Warning("Labels toggle rejected: " << error);
    syncNow();
}

void HeatmapChartControls::requestLabelCurrency(bool usd) {
    if (const auto error = m_model->apply({{"labelCurrency", usd ? "usd" : "asset"}}); !error.isEmpty())
        sLog_Warning("Label currency rejected: " << error);
    syncNow();
}

void HeatmapChartControls::requestLabelSize(double minPx, double maxPx) {
    if (const auto error = m_model->apply({{"labelMinPx", minPx}, {"labelMaxPx", maxPx}}); !error.isEmpty())
        sLog_Warning("Label size rejected: " << error);
    syncNow();
}

void HeatmapChartControls::requestCandleStyle(int style) {
    if (m_renderer) m_renderer->setCandleStyle(std::clamp(style, 0, 2));
    syncNow();
}

// The label size presets the menu offers (labelMinPx/labelMaxPx; the dialog's
// Chart tab edits any values).
namespace {
struct SizePreset {
    const char *name;
    double minPx, maxPx;
};
constexpr SizePreset kLabelSizes[] = {{"Small (12-15 px)", 12, 15}, {"Medium (14-18 px)", 14, 18},
                                      {"Large (16-20 px)", 16, 20}};
const char *kCandleStyles[] = {"Candle", "Hollow", "Line"};
} // namespace

void HeatmapChartControls::buildChartMenu() {
    QMenu *menu = m_toolbar ? m_toolbar->chartMenu() : nullptr;
    if (!menu) return;
    menu->clear();
    // clear() removes actions but retains submenu QObjects. Rebuilding for host
    // hooks must not leave findChild() pointing to an old, disabled layout menu.
    qDeleteAll(menu->findChildren<QMenu *>(QString(), Qt::FindDirectChildrenOnly));
    disconnect(menu, &QMenu::aboutToShow, this, nullptr);
    connect(menu, &QMenu::aboutToShow, this, &HeatmapChartControls::refreshChartMenu);
    auto add = [&](QMenu *parent, const QString &text, const char *name, std::function<void()> fn) {
        QAction *a = parent->addAction(text);
        a->setObjectName(name);
        a->setEnabled(bool(fn));
        if (fn) connect(a, &QAction::triggered, this, [fn] { fn(); });
        return a;
    };
    const auto hook = [](const std::function<void()> &f) { return f ? f : std::function<void()>{}; };
    add(menu, "Chart settings...", "chartMenuSettings",
        m_hooks.openSettings ? std::function<void()>([this] { m_hooks.openSettings("Chart"); }) : nullptr);
    add(menu, "Heatmap settings...", "chartMenuHeatmapSettings",
        m_hooks.openSettings ? std::function<void()>([this] { m_hooks.openSettings("Look"); }) : nullptr);
    QMenu *palettes = menu->addMenu("Heatmap palette");
    palettes->setObjectName("chartMenuPalettes");
    auto *paletteGroup = new QActionGroup(palettes);
    for (const QString &preset : {QStringLiteral("Electric"), QStringLiteral("Fire"), QStringLiteral("Ocean"),
                                  QStringLiteral("Monochrome"), QStringLiteral("Matrix")}) {
        QAction *a = add(palettes, preset, "chartMenuPalettePreset", [this, preset] {
            if (const auto error = m_model->apply({{"palettePreset", preset}}); !error.isEmpty())
                sLog_Warning("Palette preset rejected: " << error);
        });
        a->setCheckable(true);
        a->setData(preset);
        paletteGroup->addAction(a);
    }
    menu->addSeparator();
    QAction *trades = add(menu, "Trades", "chartMenuTrades", [this] {
        const auto error = m_model->apply({{"showTrades", !m_model->settings().showTrades}});
        if (!error.isEmpty()) sLog_Warning("Trades toggle rejected: " << error);
    });
    trades->setCheckable(true);
    trades->setToolTip("Live session trades; buy uses the bid colour, sell the ask colour. No backfill.");
    QMenu *tradeSize = menu->addMenu("Minimum trade notional (quote)");
    tradeSize->setObjectName("chartMenuTradeSize");
    auto *tradeGroup = new QActionGroup(tradeSize);
    for (double value : {0., 100., 1000., 10000., 100000.}) {
        QAction *a = add(tradeSize, value == 0 ? "All sizes" : QString::number(value, 'f', 0),
                         "chartMenuTradeSizePreset", [this, value] {
            const auto error = m_model->apply({{"tradeMinNotional", value}});
            if (!error.isEmpty()) sLog_Warning("Trade size rejected: " << error);
        });
        a->setCheckable(true);
        a->setData(value);
        tradeGroup->addAction(a);
    }
    QAction *labels = add(menu, "Liquidity labels", "chartMenuLabels", [this] {
        requestLabels(!m_model->settings().showLabels);
    });
    labels->setCheckable(true);
    QMenu *currency = menu->addMenu("Label currency");
    currency->setObjectName("chartMenuCurrency");
    auto *currencyGroup = new QActionGroup(currency);
    for (const auto &[text, usd] : {std::pair{"USD ($1.24M)", true}, std::pair{"Asset (12.5)", false}}) {
        QAction *a = add(currency, text, usd ? "chartMenuCurrencyUsd" : "chartMenuCurrencyAsset",
                         [this, usd = usd] { requestLabelCurrency(usd); });
        a->setCheckable(true);
        currencyGroup->addAction(a);
    }
    QMenu *size = menu->addMenu("Label size");
    size->setObjectName("chartMenuLabelSize");
    auto *sizeGroup = new QActionGroup(size);
    for (size_t i = 0; i < std::size(kLabelSizes); ++i) {
        const auto preset = kLabelSizes[i];
        QAction *a = add(size, preset.name, "chartMenuLabelSizePreset",
                         [this, preset] { requestLabelSize(preset.minPx, preset.maxPx); });
        a->setCheckable(true);
        a->setData(int(i));
        sizeGroup->addAction(a);
    }
    QMenu *candles = menu->addMenu("Candle style");
    candles->setObjectName("chartMenuCandleStyle");
    auto *candleGroup = new QActionGroup(candles);
    for (int i = 0; i < 3; ++i) {
        QAction *a = add(candles, kCandleStyles[i], "chartMenuCandleStylePreset", [this, i] { requestCandleStyle(i); });
        a->setCheckable(true);
        a->setData(i);
        candleGroup->addAction(a);
    }
    menu->addSeparator();
    add(menu, "Save chart screenshot", "chartMenuScreenshot", hook(m_hooks.screenshot));
    QMenu *layouts = menu->addMenu("Layouts");
    layouts->setObjectName("chartMenuLayouts");
    add(layouts, "Save current layout...", "chartMenuSaveLayout", hook(m_hooks.saveLayout));
    add(layouts, "Restore layout...", "chartMenuRestoreLayout", hook(m_hooks.restoreLayout));
    add(layouts, "Reset to default layout", "chartMenuResetLayout", hook(m_hooks.resetLayout));
    add(menu, "Font...", "chartMenuFont", hook(m_hooks.fontSettings));
    QMenu *debug = menu->addMenu("Diagnostics");
    debug->setObjectName("chartMenuDiagnostics");
    QAction *telemetry = add(debug, "Heatmap telemetry", "chartMenuTelemetry", [this] {
        requestTelemetryVisible(!m_model->settings().showTelemetry);
    });
    telemetry->setCheckable(true);
    for (const auto &[text, property] : {
             std::pair{"Frame and GPU statistics", "showGpuStatsOverlay"},
             std::pair{"Data pipeline", "showDataPipelineOverlay"},
             std::pair{"Render strategy", "showRenderStrategyOverlay"},
             std::pair{"Viewport math", "showViewportMathOverlay"},
             std::pair{"Memory and cache", "showMemoryCacheOverlay"},
             std::pair{"Mode flags", "showModeFlagsOverlay"}}) {
        QAction *overlay = add(debug, text, "chartMenuDebugOverlay", [this, property] {
            if (m_renderer) m_renderer->setProperty(property, !m_renderer->property(property).toBool());
        });
        overlay->setCheckable(true);
        overlay->setData(property);
    }
    add(debug, "Advanced settings...", "chartMenuDebugSettings",
        m_hooks.openSettings ? std::function<void()>([this] { m_hooks.openSettings("Debug"); }) : nullptr);
    refreshChartMenu();
}

void HeatmapChartControls::refreshChartMenu() {
    QMenu *menu = m_toolbar ? m_toolbar->chartMenu() : nullptr;
    if (!menu) return;
    const auto &s = m_model->settings();
    if (auto *palettes = menu->findChild<QMenu *>("chartMenuPalettes"))
        palettes->menuAction()->setEnabled(modeState().heatmap);
    for (QAction *a : menu->findChildren<QAction *>()) {
        const QString name = a->objectName();
        if (name == "chartMenuPalettePreset") {
            a->setChecked(a->data().toString() == QString::fromStdString(s.palettePreset));
        } else if (name == "chartMenuTelemetry") {
            a->setChecked(s.showTelemetry);
            a->setEnabled(bool(m_dock));
            a->setToolTip(m_dock ? "Show detailed heatmap diagnostics" : "Heatmap telemetry is unavailable without its dock");
        } else if (name == "chartMenuDebugOverlay") {
            a->setChecked(m_renderer && m_renderer->property(a->data().toString().toLatin1().constData()).toBool());
            a->setEnabled(bool(m_renderer));
            a->setToolTip(m_renderer ? "Show renderer diagnostics on the chart" : "No chart is attached");
        } else if (name == "chartMenuTrades") {
            a->setChecked(s.showTrades);
        } else if (name == "chartMenuTradeSizePreset") {
            a->setChecked(s.tradeMinNotional == a->data().toDouble());
        } else if (name == "chartMenuLabels") {
            a->setChecked(s.showLabels);
        }
        else if (name == "chartMenuCurrencyUsd") a->setChecked(s.labelCurrency == "usd");
        else if (name == "chartMenuCurrencyAsset") a->setChecked(s.labelCurrency == "asset");
        else if (name == "chartMenuLabelSizePreset") {
            const auto &p = kLabelSizes[a->data().toInt()];
            a->setChecked(p.minPx == s.labelMinPx && p.maxPx == s.labelMaxPx);
        } else if (name == "chartMenuCandleStylePreset") {
            a->setChecked(m_renderer && m_renderer->candleStyle() == a->data().toInt());
            a->setEnabled(bool(m_renderer));
        }
    }
}

void HeatmapChartControls::setDialog(HeatmapSettingsDialog *dialog) {
    if (m_dialog) disconnect(m_dialog, nullptr, this, nullptr);
    m_dialog = dialog;
    if (!dialog) return;
    // One path for tick actions: the dialog's are the toolbar's.
    connect(dialog, &HeatmapSettingsDialog::tickModeRequested, this, &HeatmapChartControls::requestTickMode);
    connect(dialog, &HeatmapSettingsDialog::tickPresetRequested, this, [this](qint64 units) { requestTickPreset(units); });
    syncNow();
}

void HeatmapChartControls::setTelemetryDock(HeatmapTelemetryDock *dock) {
    if (m_dock && m_dock != dock) disconnect(m_dock, nullptr, this, nullptr);
    m_dock = dock;
    if (!dock) return;
    disconnect(dock, nullptr, this, nullptr);
    disconnect(dock->toggleViewAction(), nullptr, this, nullptr);
    QPointer<UnifiedGridRenderer> renderer = m_renderer;
    dock->setProvider([renderer]() -> std::optional<QVariantMap> {
        if (!renderer || !renderer->gpuHeatmapLayer()) return std::nullopt;
        auto m = renderer->gpuHeatmapLayer()->metrics();
        const auto frames = PerformanceMonitor::instance().frameStats().window;
        m["fps"] = frames.renderRateHz;
        m["frameMs"] = frames.p50Ms;
        m["frameP95Ms"] = frames.p95Ms;
        return m;
    });
    // The user's choice (View menu or the close button) is the persisted setting;
    // layout changes that hide a tabbed dock are not.
    connect(dock, &HeatmapTelemetryDock::closedByUser, this, [this] { requestTelemetryVisible(false); });
    connect(dock->toggleViewAction(), &QAction::triggered, this, [this](bool on) { requestTelemetryVisible(on); });
    syncTelemetryVisibility();
}

void HeatmapChartControls::requestTelemetryVisible(bool visible) {
    if (m_model->settings().showTelemetry != visible) {
        if (const auto error = m_model->apply({{"showTelemetry", visible}}); !error.isEmpty())
            sLog_Warning("Telemetry visibility rejected: " << error);
    }
    syncTelemetryVisibility();
}

void HeatmapChartControls::syncTelemetryVisibility() {
    if (!m_dock) return;
    const bool want = m_model->settings().showTelemetry;
    if (m_dock->isHidden() != want) return; // already shown (or hidden) as the setting says
    m_dock->setVisible(want);
    if (want) m_dock->raise();
}

void HeatmapChartControls::onModelChanged(bool explicitManualTick) {
    const auto &s = m_model->settings();
    if (m_renderer) {
        m_renderer->setHeatmapChartSettings(s, explicitManualTick);
        m_renderer->setCandleAppearance(s);
    }
    syncTelemetryVisibility();
    syncNow(); // the toolbar and dialog follow every change at once (API, dialog, toolbar)
}

void HeatmapChartControls::scheduleSync() {
    if (!m_syncTimer->isActive()) m_syncTimer->start();
}

// Throttled, not debounced: a continuous pan still refreshes every kIndicatorMs.
void HeatmapChartControls::scheduleIndicator() {
    if (!m_indicatorTimer->isActive()) m_indicatorTimer->start();
}

TopToolbar::TickSelectorState HeatmapChartControls::tickSelectorState() const {
    TopToolbar::TickSelectorState st;
    const auto &s = m_model->settings();
    const auto *layer = m_renderer ? m_renderer->gpuHeatmapLayer() : nullptr;
    st.enabled = layer != nullptr;
    st.manual = layer ? layer->manualMode() : s.tickMode == heatmap::TickMode::Manual;
    st.manualUnits = layer ? layer->manualTickUnits() : s.manualTick;
    if (!layer) {
        st.disabledReason = QStringLiteral("No chart is attached");
        return st;
    }
    st.drawnUnits = layer->tickUnits();
    st.offeredUnits = layer->offeredTickUnits();
    st.priceScale = layer->priceScale();
    st.indicator = layer->resolutionIndicator();
    return st;
}

void HeatmapChartControls::syncNow() {
    m_syncTimer->stop();
    m_indicatorTimer->stop();
    const auto st = tickSelectorState();
    if (m_toolbar) {
        m_toolbar->setTickSelectorState(st);
        m_toolbar->setModeState(modeState());
        const auto &s = m_model->settings();
        const auto *layer = m_renderer ? m_renderer->gpuHeatmapLayer() : nullptr;
        const auto range = layer ? layer->liquidityRange() : heatmap::gpu::HeatmapGpuLayer::LiquidityRange{};
        m_toolbar->setLiquidityRange(range.valid ? range.lo : 0, range.valid ? range.hi : 0, s.sensitivityMin,
                                     s.sensitivityMax);
        m_toolbar->setLabelOptions(s.showLabels, s.labelCurrency == "usd");
        m_toolbar->setColorPreset(QString::fromStdString(s.palettePreset));
        m_toolbar->chartTypeCombo()->setEnabled(bool(m_renderer));
        m_toolbar->tpoSessionCombo()->setEnabled(bool(m_renderer));
        m_toolbar->tpoLayoutCombo()->setEnabled(bool(m_renderer));
        if (m_renderer) {
            m_toolbar->setTpoState(m_renderer->tpoSessionType(), m_renderer->tpoLayout());
            if (auto *combo = m_toolbar->chartTypeCombo(); combo && combo->currentIndex() != m_renderer->candleStyle()) {
                const QSignalBlocker block(combo);
                combo->setCurrentIndex(m_renderer->candleStyle());
            }
        }
    }
    if (m_dialog) m_dialog->setTickSelectorState(st);
}

void HeatmapChartControls::requestTickMode(bool manual) {
    QJsonObject patch{{"tickMode", manual ? "manual" : "auto"}};
    // Entering Manual with nothing remembered for this symbol and timeframe locks
    // the tick drawn now (no jump); a remembered choice wins (spec rule 2).
    const auto *layer = m_renderer ? m_renderer->gpuHeatmapLayer() : nullptr;
    if (manual && layer && !layer->manualMode() && layer->tickUnits() > 0 &&
        !layer->tickMemory().get(layer->symbol(), layer->tfMs()))
        patch["manualTick"] = qint64(layer->tickUnits());
    if (const auto error = m_model->apply(patch); !error.isEmpty()) sLog_Warning("Tick mode rejected: " << error);
    syncNow();
}

void HeatmapChartControls::requestTickPreset(int64_t units) {
    if (const auto error = m_model->apply({{"tickMode", "manual"}, {"manualTick", qint64(units)}}); !error.isEmpty())
        sLog_Warning("Tick preset rejected: " << error);
    syncNow();
}

QJsonObject HeatmapChartControls::uiState() const {
    const auto st = tickSelectorState();
    QJsonArray presets;
    for (const int64_t u : st.offeredUnits) presets.append(qint64(u));
    QJsonObject toolbar{{"enabled", st.enabled}, {"mode", st.manual ? "manual" : "auto"},
                        {"shownTickUnits", qint64(st.manual ? st.manualUnits : st.drawnUnits)},
                        {"presets", presets}, {"veiled", !st.indicator.isEmpty()}};
    if (m_toolbar) {
        const auto shown = m_toolbar->shownControls();
        toolbar["shown"] = QJsonObject{{"tickSelector", shown.tickSelector}, {"palette", shown.palette},
                                       {"liquidity", shown.liquidity}, {"rangeSlider", shown.rangeSlider},
                                       {"candleStyle", shown.candleStyle},
                                       {"tpoSession", shown.tpoSession}, {"tpoLayout", shown.tpoLayout},
                                       {"labelsToggle", shown.labelsToggle}};
        toolbar["liquidityRange"] = QJsonObject{{"low", m_toolbar->rangeSlider()->low()},
                                                {"high", m_toolbar->rangeSlider()->high()},
                                                {"endLo", m_toolbar->rangeSlider()->endLo()},
                                                {"endHi", m_toolbar->rangeSlider()->endHi()}};
    }
    if (m_toolbar && m_toolbar->tickPresetCombo())
        toolbar["presetText"] = m_toolbar->tickPresetCombo()->currentText();
    const auto b = m_model->budgets();
    return {{"toolbar", toolbar},
            {"telemetryVisible", m_dock && m_dock->exposed()}, // on screen, not the preference
            {"telemetryPreferred", m_model->settings().showTelemetry},
            {"settingsDialogOpen", m_dialog && m_dialog->isVisible()},
            {"budgets", QJsonObject{{"decodedChunkBytes", qint64(b.decodedChunks)},
                                    {"spanSourceBytes", qint64(b.spanSources)},
                                    {"cpuCeilingBytes", qint64(b.cpuCeiling)}}},
            {"savedRenderer", "gpu"}}; // the constant "gpu" since S8a (goes in S8b)
}
