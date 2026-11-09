#include "HeatmapSettingsModel.hpp"
#include "SentinelLogging.hpp"
#include <algorithm>
#include <cmath>

namespace heatmap {
namespace {
constexpr double kBtcReferenceUsd = 100'000.0;
constexpr double kAutoLowUsd = 0.05 * kBtcReferenceUsd;
constexpr double kAutoHighUsd = 50.0 * kBtcReferenceUsd;
}
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
    store_.removeLegacySensitivity(chartId_);
    settings_.sensitivityReady = false;
    budgets_ = store_.loadBudgets(defaults_);
}

void HeatmapSettingsModel::selectSymbol(const std::string &symbol) {
    if (activeSymbol_ == symbol) return;
    activeSymbol_ = symbol;
    currentPrice_ = 0;
    settings_.sensitivitySymbol = symbol;
    if (const auto saved = store_.loadSensitivity(symbol)) {
        settings_.sensitivityMin = saved->min;
        settings_.sensitivityMax = saved->max;
        settings_.sensitivityReady = true;
    } else {
        settings_.sensitivityMin = 0.05;
        settings_.sensitivityMax = 50;
        settings_.sensitivityReady = false;
    }
    emit changed(false);
}

void HeatmapSettingsModel::notePrice(const std::string &symbol, double price) {
    if (activeSymbol_ != symbol || !std::isfinite(price) || price <= 0) return;
    currentPrice_ = price;
    if (settings_.sensitivityReady) return;
    settings_.sensitivityMin = kAutoLowUsd / price;
    settings_.sensitivityMax = kAutoHighUsd / price;
    clampSettings(settings_);
    if (!store_.saveSensitivity(symbol, {settings_.sensitivityMin, settings_.sensitivityMax})) return;
    settings_.sensitivityReady = true;
    emit changed(false);
}

QString HeatmapSettingsModel::resetSensitivityToAuto() {
    if (currentPriceProvider_) {
        const double price = currentPriceProvider_();
        if (!std::isfinite(price) || price <= 0) return QStringLiteral("Waiting for a price for this symbol");
        notePrice(activeSymbol_, price);
    }
    if (activeSymbol_.empty() || !(currentPrice_ > 0)) return QStringLiteral("Waiting for a price for this symbol");
    store_.removeSensitivity(activeSymbol_);
    settings_.sensitivityReady = false;
    notePrice(activeSymbol_, currentPrice_);
    return {};
}

QString HeatmapSettingsModel::apply(const QJsonObject &patch, bool persist) {
    const Context context = context_ ? context_() : Context{};
    auto next = settings_;
    const auto error = store_.applyChartPatch(chartId_, next, patch, persist, defaults_, context.symbol, context.tfMs);
    if (!error.isEmpty()) return error;
    if (patch.contains("sensitivityMin") || patch.contains("sensitivityMax")) next.sensitivityReady = true;
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
    next.sensitivitySymbol = activeSymbol_;
    next.sensitivityReady = settings_.sensitivityReady;
    next.sensitivityMin = settings_.sensitivityMin;
    next.sensitivityMax = settings_.sensitivityMax;
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
