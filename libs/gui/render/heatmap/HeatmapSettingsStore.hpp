#pragma once
#include "heatmap/HeatmapChartSettings.hpp"
#include "config/ConfigTypes.hpp"
#include <QJsonObject>
#include <QSettings>
#include <optional>

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
    struct SensitivityRange { double min = 0, max = 0; };
    HeatmapSettingsStore(); // QSettings("Sentinel", "SentinelTerminal")
    explicit HeatmapSettingsStore(QSettings &settings); // isolated store for tests
    HeatmapChartSettings load(const QString &chartId, const ClientHeatmapConfig &defaults) const;
    void save(const QString &chartId, HeatmapChartSettings value);
    void removeLegacySensitivity(const QString &chartId);
    // Patches persist only their own fields, never earlier process-only overrides.
    QString applyChartPatch(const QString &chartId, HeatmapChartSettings &current, const QJsonObject &patch,
                            bool persist, const ClientHeatmapConfig &defaults, const std::string &symbol, int64_t tfMs);
    void saveLayout(const QString &name, const QString &chartId, const ClientHeatmapConfig &defaults);
    void restoreLayoutInto(const QString &name, const QString &chartId, HeatmapChartSettings &current,
                           const ClientHeatmapConfig &defaults);
    HeatmapChartSettings restoreLayout(const QString &name, const QString &chartId,
                                      const ClientHeatmapConfig &defaults);
    ManualTickMemory loadManualTicks() const;
    bool saveManualTick(const std::string &symbol, int64_t tfMs, int64_t units);
    std::optional<SensitivityRange> loadSensitivity(const std::string &symbol) const;
    bool saveSensitivity(const std::string &symbol, SensitivityRange range);
    void removeSensitivity(const std::string &symbol);
    HeatmapBudgets loadBudgets(const ClientHeatmapConfig &defaults) const;
    void saveBudgets(const HeatmapBudgets &budgets);
private:
    HeatmapChartSettings loadAt(const QString &prefix, HeatmapChartSettings defaults) const;
    void saveAt(const QString &prefix, HeatmapChartSettings value);
    QSettings owned_{QSettings::defaultFormat(), QSettings::UserScope, "Sentinel", "SentinelTerminal"}; // process default format: INI under --agent-host
    QSettings *settings_ = &owned_;
};
} // namespace heatmap
