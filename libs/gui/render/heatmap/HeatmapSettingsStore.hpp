#pragma once
#include "heatmap/HeatmapChartSettings.hpp"
#include "config/ConfigTypes.hpp"
#include <QJsonObject>
#include <QSettings>

namespace heatmap {
struct HeatmapBudgets;
HeatmapChartSettings chartDefaults(const ClientHeatmapConfig &config);
void clampSettings(HeatmapChartSettings &settings);
QJsonObject settingsJson(const HeatmapChartSettings &settings);
// Strict types and keys; finite numeric values are clamped to documented limits.
// A bad patch changes nothing. Error is empty on success.
QString applySettingsPatch(HeatmapChartSettings &settings, const QJsonObject &patch);

class HeatmapSettingsStore {
public:
    HeatmapSettingsStore(); // QSettings("Sentinel", "SentinelTerminal")
    explicit HeatmapSettingsStore(QSettings &settings); // isolated store for tests
    HeatmapChartSettings load(const QString &chartId, const ClientHeatmapConfig &defaults) const;
    void save(const QString &chartId, HeatmapChartSettings value);
    void saveLayout(const QString &name, const QString &chartId, HeatmapChartSettings value);
    HeatmapChartSettings restoreLayout(const QString &name, const QString &chartId,
                                      const ClientHeatmapConfig &defaults);
    ManualTickMemory loadManualTicks() const;
    bool saveManualTick(const std::string &symbol, int64_t tfMs, int64_t units);
    HeatmapBudgets loadBudgets(const ClientHeatmapConfig &defaults) const;
    void saveBudgets(const HeatmapBudgets &budgets);
private:
    HeatmapChartSettings loadAt(const QString &prefix, HeatmapChartSettings defaults) const;
    void saveAt(const QString &prefix, HeatmapChartSettings value);
    QSettings owned_{"Sentinel", "SentinelTerminal"};
    QSettings *settings_ = &owned_;
};
} // namespace heatmap
