#include "HeatmapChartControls.hpp"
#include "PerformanceMonitor.hpp"
#include "SentinelLogging.hpp"
#include "UnifiedGridRenderer.h"
#include "render/heatmap/HeatmapGpuLayer.hpp"
#include "widgets/HeatmapSettingsDialog.hpp"
#include "widgets/HeatmapTelemetryDock.hpp"
#include <QAction>
#include <QJsonArray>
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
    if (!renderer) return;
    renderer->setHeatmapTickMemory(m_model->manualTicks());
    renderer->setHeatmapChartSettings(m_model->settings());
    renderer->setHeatmapRenderer(QString::fromStdString(m_model->settings().renderer));
    connect(renderer, &UnifiedGridRenderer::timeframeChanged, this, &HeatmapChartControls::scheduleSync);
    connect(renderer, &UnifiedGridRenderer::viewportChanged, this, &HeatmapChartControls::scheduleIndicator);
    if (auto *layer = renderer->gpuHeatmapLayer()) {
        using Layer = heatmap::gpu::HeatmapGpuLayer;
        connect(layer, &Layer::tickChanged, this, &HeatmapChartControls::scheduleSync);
        connect(layer, &Layer::presetsChanged, this, &HeatmapChartControls::scheduleSync);
        connect(layer, &Layer::snapshotChanged, this, &HeatmapChartControls::scheduleIndicator);
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
    syncNow();
}

void HeatmapChartControls::setDialog(HeatmapSettingsDialog *dialog) {
    m_dialog = dialog;
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
        if (!renderer || !renderer->gpuHeatmapActive() || !renderer->gpuHeatmapLayer()) return std::nullopt;
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
        m_renderer->setHeatmapRenderer(QString::fromStdString(s.renderer));
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
    const bool gpu = m_renderer && m_renderer->gpuHeatmapActive() && layer;
    st.enabled = gpu;
    st.manual = gpu ? layer->manualMode() : s.tickMode == heatmap::TickMode::Manual;
    st.manualUnits = gpu ? layer->manualTickUnits() : s.manualTick;
    if (!gpu) {
        st.disabledReason = QStringLiteral("The tick selector drives the GPU heatmap renderer. This chart draws with "
                                           "the legacy renderer (Settings > Debug > Renderer).");
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
    if (m_toolbar) m_toolbar->setTickSelectorState(st);
    if (m_dialog) m_dialog->setTickPresets(st.offeredUnits, st.priceScale, st.manual ? st.manualUnits : 0);
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
    if (m_toolbar && m_toolbar->tickPresetCombo())
        toolbar["presetText"] = m_toolbar->tickPresetCombo()->currentText();
    const auto b = m_model->budgets();
    return {{"toolbar", toolbar},
            {"telemetryVisible", m_dock && m_dock->isVisible()},
            {"settingsDialogOpen", m_dialog && m_dialog->isVisible()},
            {"budgets", QJsonObject{{"decodedChunkBytes", qint64(b.decodedChunks)},
                                    {"spanSourceBytes", qint64(b.spanSources)},
                                    {"cpuCeilingBytes", qint64(b.cpuCeiling)}}},
            {"savedRenderer", QString::fromStdString(m_model->savedRenderer())}};
}
