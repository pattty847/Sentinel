#include "HeatmapSettingsModel.hpp"
#include "SentinelLogging.hpp"
#include <algorithm>

namespace heatmap {
QString validateBudgets(const HeatmapBudgets &b) {
    for (const size_t v : {b.decodedChunks, b.spanSources, b.cpuCeiling})
        if (v < kMinBudgetBytes || v > kMaxBudgetBytes) return QStringLiteral("Each budget must be 1..4096 MiB");
    if (b.decodedChunks + b.spanSources > b.cpuCeiling)
        return QStringLiteral("The CPU ceiling must be at least decoded chunks + span sources");
    return {};
}

HeatmapSettingsModel::HeatmapSettingsModel(HeatmapSettingsStore &store, QString chartId, ClientHeatmapConfig defaults,
                                           QObject *parent)
    : QObject(parent), store_(store), chartId_(std::move(chartId)), defaults_(std::move(defaults)) {
    settings_ = store_.load(chartId_, defaults_);
    budgets_ = store_.loadBudgets(defaults_);
}

QString HeatmapSettingsModel::apply(const QJsonObject &patch, bool persist) {
    const Context context = context_ ? context_() : Context{};
    auto next = settings_;
    const auto error = store_.applyChartPatch(chartId_, next, patch, persist, defaults_, context.symbol, context.tfMs);
    if (!error.isEmpty()) return error;
    const bool explicitTick = patch.contains("manualTick") && next.tickMode == TickMode::Manual;
    if (next == settings_ && !explicitTick) return {};
    settings_ = std::move(next);
    sLog_App("Heatmap settings changed chart=" << chartId_ << " keys=" << patch.keys().join(',')
             << " persist=" << persist);
    emit changed(explicitTick);
    return {};
}

QString HeatmapSettingsModel::resetKeys(const QStringList &keys) {
    const auto defaults = settingsJson(defaultSettings());
    QJsonObject patch;
    for (const auto &key : keys)
        if (defaults.contains(key)) patch[key] = defaults[key];
    if (patch.isEmpty()) return {};
    return apply(patch, true);
}

void HeatmapSettingsModel::saveLayout(const QString &name) { store_.saveLayout(name, chartId_, defaults_); }

void HeatmapSettingsModel::restoreLayout(const QString &name) {
    if (name == QLatin1String("_last_session")) return; // live per-chart settings win (INV-088)
    auto next = settings_;
    store_.restoreLayoutInto(name, chartId_, next, defaults_);
    if (next == settings_) return;
    settings_ = std::move(next);
    sLog_App("Heatmap settings restored from workspace=" << name << " chart=" << chartId_);
    emit changed(false);
}

HeatmapBudgets HeatmapSettingsModel::defaultBudgets() const {
    const auto clamp = [](uint64_t v) { return size_t(std::clamp<uint64_t>(v, kMinBudgetBytes, kMaxBudgetBytes)); };
    HeatmapBudgets b;
    b.decodedChunks = clamp(defaults_.decodedChunkBytes);
    b.spanSources = clamp(defaults_.spanSourceBytes);
    b.cpuCeiling = std::max(clamp(defaults_.cpuCeilingBytes), b.decodedChunks + b.spanSources);
    b.gpuPerChart = defaultSettings().gpuCapBytes;
    return b;
}

QString HeatmapSettingsModel::setBudgets(const HeatmapBudgets &requested) {
    auto b = requested;
    b.gpuPerChart = budgets_.gpuPerChart; // per chart: gpuCapBytes in the chart settings
    if (const auto error = validateBudgets(b); !error.isEmpty()) return error;
    if (b.decodedChunks == budgets_.decodedChunks && b.spanSources == budgets_.spanSources &&
        b.cpuCeiling == budgets_.cpuCeiling)
        return {};
    if (budgetSink_ && !budgetSink_(b)) return QStringLiteral("The data service refused the budgets");
    store_.saveBudgets(b);
    budgets_ = b;
    sLog_App("Heatmap process budgets decoded=" << b.decodedChunks << " spans=" << b.spanSources
             << " ceiling=" << b.cpuCeiling);
    emit budgetsChanged();
    return {};
}
} // namespace heatmap
