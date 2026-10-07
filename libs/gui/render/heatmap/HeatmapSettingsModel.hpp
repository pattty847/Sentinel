#pragma once
// One chart's heatmap settings: the single source of truth (S6c). The settings
// dialog, the toolbar tick selector, the telemetry dock and the Agent API all
// change settings through apply(); every listener (the chart, the widgets)
// follows changed(). GUI thread only; no per-frame use.
//
// Persistence goes through HeatmapSettingsStore: heatmap/<chartId>/... per chart,
// layouts/<name>/heatmap/<chartId>/ for named workspaces only (never
// _last_session, INV-088), heatmap/manualTick/<symbol>/<tf> for the Manual tick
// memory (shared by every chart) and heatmap/budgets/... for the process budgets.
#include "HeatmapSettingsStore.hpp"
#include "HeatmapSourceController.hpp"
#include <QObject>
#include <QStringList>
#include <functional>

namespace heatmap {
class HeatmapSettingsModel final : public QObject {
    Q_OBJECT
public:
    // The symbol and timeframe a Manual tick choice is remembered for.
    struct Context {
        std::string symbol;
        int64_t tfMs = 0;
    };
    HeatmapSettingsModel(HeatmapSettingsStore &store, QString chartId, ClientHeatmapConfig defaults,
                         QObject *parent = nullptr);

    const HeatmapChartSettings &settings() const { return settings_; }
    HeatmapChartSettings defaultSettings() const { return chartDefaults(defaults_); }
    const ClientHeatmapConfig &configDefaults() const { return defaults_; }
    const QString &chartId() const { return chartId_; }
    HeatmapSettingsStore &store() { return store_; }

    void setContextProvider(std::function<Context()> provider) { context_ = std::move(provider); }

    // A partial settings patch (the Agent API schema). persist=false changes this
    // process only. Returns the S6a validation error (nothing changes) or empty.
    QString apply(const QJsonObject &patch, bool persist = true);
    // Restores the listed keys to their configured defaults (persisted).
    QString resetKeys(const QStringList &keys);

    // Named workspaces (the LayoutOrchestrator hooks); _last_session is ignored.
    void saveLayout(const QString &name);
    void restoreLayout(const QString &name);

    // Process-wide CPU budgets (every chart). The sink applies them to the live
    // service; they are saved only when the sink (if any) accepts them.
    HeatmapBudgets budgets() const { return budgets_; }
    HeatmapBudgets defaultBudgets() const;
    void setBudgetSink(std::function<bool(const HeatmapBudgets &)> sink) { budgetSink_ = std::move(sink); }
    // Empty on success, else why the budgets were refused (nothing changes).
    QString setBudgets(const HeatmapBudgets &budgets);

    // The Manual tick memory as persisted (shared by every chart).
    ManualTickMemory manualTicks() const { return store_.loadManualTicks(); }

signals:
    // settings() changed. explicitManualTick: the change is an explicit Manual
    // tick choice (HeatmapGpuLayer::setSettings remembers it for the context).
    void changed(bool explicitManualTick);
    void budgetsChanged();

private:
    HeatmapSettingsStore &store_;
    QString chartId_;
    ClientHeatmapConfig defaults_;
    HeatmapChartSettings settings_;
    HeatmapBudgets budgets_;
    std::function<Context()> context_;
    std::function<bool(const HeatmapBudgets &)> budgetSink_;
};

// Bounds the Budgets tab and setBudgets enforce (HeatmapSettingsStore::loadBudgets).
constexpr uint64_t kMinBudgetBytes = 1ull << 20, kMaxBudgetBytes = 4096ull << 20;
QString validateBudgets(const HeatmapBudgets &budgets);
} // namespace heatmap
