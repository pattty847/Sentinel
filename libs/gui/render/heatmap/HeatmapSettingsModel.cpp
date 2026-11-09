#include "HeatmapSettingsModel.hpp"
#include "SentinelLogging.hpp"
#include <QDateTime>
#include <QTimer>
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
    const bool inheritPendingEdit = activeSymbol_.empty() && userEditedRange_;
    const double pendingMin = settings_.sensitivityMin, pendingMax = settings_.sensitivityMax;
    activeSymbol_ = symbol;
    currentPrice_ = 0;
    userEditedRange_ = false;
    settings_.sensitivitySymbol = symbol;
    if (const auto saved = store_.loadSensitivity(symbol)) {
        activeRangeSaved_ = true;
        settings_.sensitivityMin = saved->min;
        settings_.sensitivityMax = saved->max;
        settings_.sensitivityReady = true;
    } else {
        activeRangeSaved_ = false;
        settings_.sensitivityMin = inheritPendingEdit ? pendingMin : 0.05;
        settings_.sensitivityMax = inheritPendingEdit ? pendingMax : 50;
        settings_.sensitivityReady = inheritPendingEdit;
        userEditedRange_ = inheritPendingEdit;
    }
    emit changed(false);
    if (!symbol.empty()) QTimer::singleShot(10'000, this, [this, symbol] {
        if (activeSymbol_ != symbol || activeRangeSaved_) return;
        sLog_Warning("Heatmap range unsaved symbol=" << symbol
                      << " reason=" << (userEditedRange_ ? "pending_user_edit" :
                                         currentPrice_ > 0 ? "waiting_for_visible_liquidity" : "waiting_for_price_or_liquidity"));
    });
}

void HeatmapSettingsModel::notePrice(const std::string &symbol, double price) {
    if (activeSymbol_ != symbol || !std::isfinite(price) || price <= 0) return;
    currentPrice_ = price;
    if (settings_.sensitivityReady) return;
    settings_.sensitivityMin = kAutoLowUsd / price;
    settings_.sensitivityMax = kAutoHighUsd / price;
    clampSettings(settings_);
    settings_.sensitivityReady = true;
    emit changed(false);
}

void HeatmapSettingsModel::refreshCurrentPrice() {
    if (!currentPriceProvider_) return;
    const double price = currentPriceProvider_();
    if (std::isfinite(price) && price > 0) currentPrice_ = price;
}

void HeatmapSettingsModel::noteVisibleLiquidity(const std::string &symbol, double lo, double hi) {
    if (symbol != activeSymbol_ || symbol.empty() || activeRangeSaved_ || userEditedRange_ ||
        !std::isfinite(lo) || !std::isfinite(hi) || lo <= 0 || hi <= lo) return;
    refreshCurrentPrice();
    HeatmapSettingsStore::SensitivityRange range{lo, hi, HeatmapSettingsStore::RangeOrigin::Auto,
                                                  QDateTime::currentMSecsSinceEpoch(), currentPrice_};
    if (!store_.saveSensitivity(symbol, range)) return;
    activeRangeSaved_ = true;
    settings_.sensitivityMin = lo;
    settings_.sensitivityMax = hi;
    settings_.sensitivityReady = true;
    emit changed(false);
}

QString HeatmapSettingsModel::resetSensitivityToAuto() {
    refreshCurrentPrice();
    if (activeSymbol_.empty()) return QStringLiteral("Waiting for a symbol");
    store_.removeSensitivity(activeSymbol_);
    activeRangeSaved_ = false;
    userEditedRange_ = false;
    settings_.sensitivityReady = false;
    if (visibleLiquidity_) {
        const auto visible = visibleLiquidity_(activeSymbol_);
        if (visible) {
            noteVisibleLiquidity(activeSymbol_, visible->lo, visible->hi);
            if (activeRangeSaved_) return {};
        }
    }
    settings_.sensitivityMin = 0.05;
    settings_.sensitivityMax = 50;
    if (currentPrice_ > 0) notePrice(activeSymbol_, currentPrice_);
    else emit changed(false);
    return {};
}

QString HeatmapSettingsModel::apply(const QJsonObject &patch, bool persist) {
    const Context context = context_ ? context_() : Context{};
    const bool rangeEdited = patch.contains("sensitivityMin") || patch.contains("sensitivityMax");
    if (rangeEdited && persist) refreshCurrentPrice();
    std::optional<HeatmapSettingsStore::SensitivityRange> sensitivity;
    if (rangeEdited && persist)
        sensitivity = HeatmapSettingsStore::SensitivityRange{0, 0, HeatmapSettingsStore::RangeOrigin::User,
                                                               QDateTime::currentMSecsSinceEpoch(), currentPrice_};
    auto next = settings_;
    const std::string symbol = rangeEdited ? activeSymbol_ : context.symbol;
    const auto error = store_.applyChartPatch(chartId_, next, patch, persist, defaults_, symbol, context.tfMs, sensitivity);
    if (!error.isEmpty()) return error;
    if (rangeEdited) {
        next.sensitivityReady = true;
        userEditedRange_ = true;
        if (persist && symbol == activeSymbol_ && !symbol.empty()) activeRangeSaved_ = true;
    }
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
