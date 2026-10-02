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
// - the toolbar's mode (which controls the active layers show, TopToolbar::
//   controlVisibility), the liquidity range slider and label options (S7b:
//   sensitivityMin/Max, showLabels, labelCurrency) and the TPO controls;
// - the chart settings menu (the toolbar gear): one entry point for chart-level
//   settings that exist (labels, label size, candle style, the settings dialog,
//   screenshot, layouts, font). The host supplies the actions it owns (hooks).
// Nothing here runs per frame.
#include "render/heatmap/HeatmapSettingsModel.hpp"
#include "widgets/TopToolbar.hpp"
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <functional>

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

    // Actions of the chart menu the host owns (empty: the entry is disabled).
    struct MenuHooks {
        std::function<void(const QString &tab)> openSettings; // the settings dialog on a tab
        std::function<void()> screenshot;                     // save a chart screenshot
        std::function<void()> saveLayout, restoreLayout, resetLayout;
        std::function<void()> fontSettings;
    };
    void setMenuHooks(MenuHooks hooks);
    // Rebuilds the chart menu's check states from the model and renderer (also on
    // every aboutToShow).
    void refreshChartMenu();
    // The toolbar mode the chart implies now.
    TopToolbar::ModeState modeState() const;
    // Toolbar actions (the toolbar's signals call these).
    void requestLiquidityRange(double low, double high, bool persist);
    void requestLabels(bool show);
    void requestLabelCurrency(bool usd);
    void requestLabelSize(double minPx, double maxPx);
    void requestCandleStyle(int style);

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
    void buildChartMenu();

    heatmap::HeatmapSettingsModel *m_model = nullptr;
    QPointer<UnifiedGridRenderer> m_renderer;
    QPointer<TopToolbar> m_toolbar;
    QPointer<HeatmapSettingsDialog> m_dialog;
    QPointer<HeatmapTelemetryDock> m_dock;
    QTimer *m_syncTimer = nullptr;      // 0 ms: coalesces bursts of tick/preset signals
    QTimer *m_indicatorTimer = nullptr; // kIndicatorMs: view/data-driven refresh, throttled
    MenuHooks m_hooks;
};
