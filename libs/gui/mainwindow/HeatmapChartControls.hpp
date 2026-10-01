#pragma once
// Binds one chart's HeatmapSettingsModel (the single source of truth) to its
// chart and UI (S6c, GUI thread):
// - model changed -> UnifiedGridRenderer (settings, renderer flip), the toolbar
//   tick selector, the settings dialog and the telemetry dock visibility;
// - toolbar tick selector -> model (Auto/Manual, a preset locks Manual and is
//   remembered per symbol and timeframe);
// - chart tick/preset/renderer changes -> toolbar and dialog at once; view and
//   data changes refresh the veil indicator at most every kIndicatorMs;
// - the telemetry dock's 4 Hz provider (the layer's metrics + frame stats) and
//   its showTelemetry visibility (View menu, close button, setting).
// Nothing here runs per frame.
#include "render/heatmap/HeatmapSettingsModel.hpp"
#include "widgets/TopToolbar.hpp"
#include <QJsonObject>
#include <QObject>
#include <QPointer>

class UnifiedGridRenderer;
class HeatmapSettingsDialog;
class HeatmapTelemetryDock;
class QTimer;

class HeatmapChartControls final : public QObject {
    Q_OBJECT
public:
    static constexpr int kIndicatorMs = 250;
    explicit HeatmapChartControls(heatmap::HeatmapSettingsModel *model, QObject *parent = nullptr);

    heatmap::HeatmapSettingsModel *model() const { return m_model; }
    void setRenderer(UnifiedGridRenderer *renderer);
    void setToolbar(TopToolbar *toolbar);
    void setDialog(HeatmapSettingsDialog *dialog);
    void setTelemetryDock(HeatmapTelemetryDock *dock);

    // The toolbar state the chart implies now (also what the API reports).
    TopToolbar::TickSelectorState tickSelectorState() const;
    // Toolbar actions (the toolbar's signals call these).
    void requestTickMode(bool manual);
    void requestTickPreset(int64_t units);
    // Shows/hides the telemetry dock as the user asked (persisted showTelemetry).
    void requestTelemetryVisible(bool visible);
    // The UI fields of GET /api/v1/heatmap/state.
    QJsonObject uiState() const;
    // Applies the model to every bound piece now (also the coalesced path).
    void syncNow();

private:
    void onModelChanged(bool explicitManualTick);
    void scheduleSync();
    void scheduleIndicator();
    void syncTelemetryVisibility();

    heatmap::HeatmapSettingsModel *m_model = nullptr;
    QPointer<UnifiedGridRenderer> m_renderer;
    QPointer<TopToolbar> m_toolbar;
    QPointer<HeatmapSettingsDialog> m_dialog;
    QPointer<HeatmapTelemetryDock> m_dock;
    QTimer *m_syncTimer = nullptr;      // 0 ms: coalesces bursts of tick/preset signals
    QTimer *m_indicatorTimer = nullptr; // kIndicatorMs: view/data-driven refresh, throttled
};
