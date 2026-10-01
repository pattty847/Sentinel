#include "HeatmapSettingsStore.hpp"
#include "HeatmapSourceController.hpp"
#include <QJsonArray>
#include <QRegularExpression>
#include <algorithm>
#include <cmath>

namespace heatmap {
namespace {
constexpr uint64_t MiB = 1ull << 20;
const QStringList palettes{"Electric", "Fire", "Ocean", "Monochrome", "Matrix", "Custom"};
double finiteClamp(double v, double lo, double hi, double fallback) {
    return std::isfinite(v) ? std::clamp(v, lo, hi) : fallback;
}
bool validGradient(const QJsonValue &value) {
    if (!value.isArray()) return false;
    const auto a = value.toArray();
    if (a.size() < 2 || a.size() > 16) return false;
    double previous = -1;
    static const QRegularExpression color("^#[0-9a-fA-F]{6}([0-9a-fA-F]{2})?$");
    for (const auto &v : a) {
        if (!v.isObject()) return false;
        const auto o = v.toObject();
        const auto p = o["position"];
        if (o.size() != 2 || !p.isDouble() || !std::isfinite(p.toDouble()) ||
            p.toDouble() < 0 || p.toDouble() > 1 || p.toDouble() <= previous ||
            !o["color"].isString() || !color.match(o["color"].toString()).hasMatch()) return false;
        previous = p.toDouble();
    }
    return a.first().toObject()["position"].toDouble() == 0 && previous == 1;
}
QJsonArray gradientJson(const std::vector<GradientStop> &stops) {
    QJsonArray out;
    for (const auto &s : stops) out.append(QJsonObject{{"position", s.position}, {"color", QString::fromStdString(s.color)}});
    return out;
}
std::vector<GradientStop> gradient(const QJsonValue &v) {
    std::vector<GradientStop> out;
    for (const auto &s : v.toArray()) out.push_back({s.toObject()["position"].toDouble(), s.toObject()["color"].toString().toStdString()});
    return out;
}
// Escape path segments, including '/' and '%', so chart/layout/symbol names can
// never address a sibling group. The usual main/BTC-USD names stay readable.
QString segment(const QString &value) {
    QString out = value;
    return out.replace('%', "%25").replace('/', "%2F").replace('\\', "%5C");
}
QString symbolFromSegment(QString value) {
    return value.replace("%5C", "\\").replace("%2F", "/").replace("%25", "%");
}
}

void clampSettings(HeatmapChartSettings &s) {
    const HeatmapChartSettings d;
    if (s.renderer != "gpu" && s.renderer != "legacy") s.renderer = d.renderer;
    if (s.tickMode != TickMode::Auto && s.tickMode != TickMode::Manual) s.tickMode = TickMode::Auto;
    s.manualTick = presetAtLeast(double(std::clamp<int64_t>(s.manualTick, 1, 1'000'000'000'000LL)), 1);
    s.minRowPx = finiteClamp(s.minRowPx, 0.5, 32, d.minRowPx);
    s.hysteresis = finiteClamp(s.hysteresis, 0, 0.9, d.hysteresis);
    s.crossfadeMs = std::clamp(s.crossfadeMs, 0, 2000);
    if (!palettes.contains(QString::fromStdString(s.palettePreset))) s.palettePreset = d.palettePreset;
    if (!validGradient(gradientJson(s.bidGradient))) s.bidGradient = d.bidGradient;
    if (!validGradient(gradientJson(s.askGradient))) s.askGradient = d.askGradient;
    s.sensitivityMin = finiteClamp(s.sensitivityMin, 1e-9, 1e12, d.sensitivityMin);
    s.sensitivityMax = finiteClamp(s.sensitivityMax, 1e-9, 1e15, d.sensitivityMax);
    if (s.sensitivityMax <= s.sensitivityMin) s.sensitivityMax = s.sensitivityMin * 2;
    s.opacity = finiteClamp(s.opacity, 0, 1, d.opacity);
    s.gpuCapBytes = std::clamp<uint64_t>(s.gpuCapBytes, MiB, 4096 * MiB);
    s.uploadBudgetBytes = std::clamp<uint64_t>(s.uploadBudgetBytes, 1, std::min<uint64_t>(128 * MiB, s.gpuCapBytes));
    s.prefetchTiles = std::clamp(s.prefetchTiles, 0, 16);
    s.liveMinIntervalMs = std::clamp(s.liveMinIntervalMs, 100, 5000);
}
HeatmapChartSettings chartDefaults(const ClientHeatmapConfig &c) {
    HeatmapChartSettings s;
    s.renderer = c.renderer;
    s.tickMode = c.tickMode == "manual" ? TickMode::Manual : TickMode::Auto;
    s.manualTick = c.manualTick;
    s.minRowPx = c.minRowPx;
    s.hysteresis = c.hysteresis;
    s.crossfadeMs = c.crossfadeMs;
    s.showBandEdges = c.showBandEdges;
    s.palettePreset = c.palettePreset;
    s.bidGradient.clear(); s.askGradient.clear();
    for (const auto &[position, color] : c.bidGradient) s.bidGradient.push_back({position, color});
    for (const auto &[position, color] : c.askGradient) s.askGradient.push_back({position, color});
    s.sensitivityMin = c.sensitivityMin;
    s.sensitivityMax = c.sensitivityMax;
    s.opacity = c.opacity;
    s.gpuCapBytes = c.gpuCapBytes;
    s.uploadBudgetBytes = c.uploadBudgetBytes;
    s.prefetchTiles = c.prefetchTiles;
    s.liveMinIntervalMs = c.liveMinIntervalMs;
    s.showTelemetry = c.showTelemetry;
    clampSettings(s);
    return s;
}
QJsonObject settingsJson(const HeatmapChartSettings &s) {
    return {
        {"renderer", QString::fromStdString(s.renderer)},
        {"manualTick", qint64(s.manualTick)},
        {"minRowPx", s.minRowPx},
        {"hysteresis", s.hysteresis},
        {"crossfadeMs", s.crossfadeMs},
        {"showBandEdges", s.showBandEdges},
        {"palettePreset", QString::fromStdString(s.palettePreset)},
        {"sensitivityMin", s.sensitivityMin},
        {"sensitivityMax", s.sensitivityMax},
        {"opacity", s.opacity},
        {"gpuCapBytes", qint64(s.gpuCapBytes)},
        {"uploadBudgetBytes", qint64(s.uploadBudgetBytes)},
        {"prefetchTiles", s.prefetchTiles},
        {"liveMinIntervalMs", s.liveMinIntervalMs},
        {"showTelemetry", s.showTelemetry},
        {"tickMode", s.tickMode == TickMode::Auto ? "auto" : "manual"},
        {"bidGradient", gradientJson(s.bidGradient)}, {"askGradient", gradientJson(s.askGradient)}
    };
}
QString applySettingsPatch(HeatmapChartSettings &s, const QJsonObject &patch) {
    const auto schema = settingsJson(HeatmapChartSettings{});
    auto merged = settingsJson(s);
    const QStringList integers{"manualTick", "crossfadeMs", "gpuCapBytes", "uploadBudgetBytes", "prefetchTiles", "liveMinIntervalMs"};
    for (auto it = patch.begin(); it != patch.end(); ++it) {
        const auto &key = it.key();
        const auto v = it.value();
        if (!schema.contains(key)) return "Unknown heatmap setting: " + key;
        if (v.type() != schema[key].type()) return "Invalid type for heatmap setting: " + key;
        if (v.isDouble() && (!std::isfinite(v.toDouble()) || std::abs(v.toDouble()) > 9007199254740991.0))
            return "Expected a finite safe number: " + key;
        if (integers.contains(key) && std::floor(v.toDouble()) != v.toDouble()) return "Expected an integer: " + key;
        if (key == "renderer" && v != "legacy" && v != "gpu") return "renderer must be legacy or gpu";
        if (key == "tickMode" && v != "auto" && v != "manual") return "tickMode must be auto or manual";
        if (key == "palettePreset" && !palettes.contains(v.toString())) return "Unknown palettePreset";
        if ((key == "bidGradient" || key == "askGradient") && !validGradient(v)) return "Invalid gradient (2..16 ordered stops, endpoints 0 and 1, hex colors)";
        merged[key] = v;
    }
    HeatmapChartSettings out;
    out.renderer = merged["renderer"].toString().toStdString();
    out.manualTick = int64_t(merged["manualTick"].toDouble());
    out.minRowPx = merged["minRowPx"].toDouble();
    out.hysteresis = merged["hysteresis"].toDouble();
    out.crossfadeMs = int(std::clamp(merged["crossfadeMs"].toDouble(), -2147483647.0, 2147483647.0));
    out.showBandEdges = merged["showBandEdges"].toBool();
    out.palettePreset = merged["palettePreset"].toString().toStdString();
    out.sensitivityMin = merged["sensitivityMin"].toDouble();
    out.sensitivityMax = merged["sensitivityMax"].toDouble();
    out.opacity = merged["opacity"].toDouble();
    out.gpuCapBytes = uint64_t(std::max(0.0, merged["gpuCapBytes"].toDouble()));
    out.uploadBudgetBytes = uint64_t(std::max(0.0, merged["uploadBudgetBytes"].toDouble()));
    out.prefetchTiles = int(std::clamp(merged["prefetchTiles"].toDouble(), -2147483647.0, 2147483647.0));
    out.liveMinIntervalMs = int(std::clamp(merged["liveMinIntervalMs"].toDouble(), -2147483647.0, 2147483647.0));
    out.showTelemetry = merged["showTelemetry"].toBool();
    out.tickMode = merged["tickMode"] == "manual" ? TickMode::Manual : TickMode::Auto;
    out.bidGradient = gradient(merged["bidGradient"]);
    out.askGradient = gradient(merged["askGradient"]);
    clampSettings(out);
    s = std::move(out);
    return {};
}
HeatmapSettingsStore::HeatmapSettingsStore() = default;
HeatmapSettingsStore::HeatmapSettingsStore(QSettings &settings) : settings_(&settings) {}
HeatmapChartSettings HeatmapSettingsStore::loadAt(const QString &prefix, HeatmapChartSettings defaults) const {
    const auto schema = settingsJson(defaults);
    QJsonObject patch;
    for (auto it = schema.begin(); it != schema.end(); ++it) {
        if (!settings_->contains(prefix + it.key())) continue;
        const QJsonValue value = QJsonValue::fromVariant(settings_->value(prefix + it.key()));
        // QSettings INI can return scalar strings. Decode against the schema;
        // malformed persisted fields fall back independently, never to zero.
        QJsonValue decoded = value;
        if (value.isString() && it->isDouble()) {
            bool ok = false;
            const double n = value.toString().toDouble(&ok);
            if (!ok) continue;
            decoded = n;
        } else if (value.isString() && it->isBool()) {
            if (value != "true" && value != "false") continue;
            decoded = value == "true";
        }
        auto check = defaults;
        if (applySettingsPatch(check, {{it.key(), decoded}}).isEmpty()) patch[it.key()] = decoded;
    }
    applySettingsPatch(defaults, patch);
    return defaults;
}
void HeatmapSettingsStore::saveAt(const QString &prefix, HeatmapChartSettings value) {
    clampSettings(value);
    const auto json = settingsJson(value);
    for (auto it = json.begin(); it != json.end(); ++it) settings_->setValue(prefix + it.key(), it.value().toVariant());
}
HeatmapChartSettings HeatmapSettingsStore::load(const QString &id, const ClientHeatmapConfig &defaults) const {
    return loadAt("heatmap/" + segment(id) + "/", chartDefaults(defaults));
}
void HeatmapSettingsStore::save(const QString &id, HeatmapChartSettings value) {
    saveAt("heatmap/" + segment(id) + "/", std::move(value));
}
void HeatmapSettingsStore::saveLayout(const QString &name, const QString &id, HeatmapChartSettings value) {
    saveAt("layouts/" + segment(name) + "/heatmap/" + segment(id) + "/", std::move(value));
}
HeatmapChartSettings HeatmapSettingsStore::restoreLayout(const QString &name, const QString &id, const ClientHeatmapConfig &defaults) {
    auto out = loadAt("layouts/" + segment(name) + "/heatmap/" + segment(id) + "/", load(id, defaults));
    save(id, out);
    return out;
}
ManualTickMemory HeatmapSettingsStore::loadManualTicks() const {
    ManualTickMemory out;
    settings_->beginGroup("heatmap/manualTick");
    for (const auto &key : settings_->allKeys()) {
        const auto parts = key.split('/');
        if (parts.size() != 2) continue;
        bool tfOk = false, unitsOk = false;
        const auto tf = parts[1].toLongLong(&tfOk);
        const auto units = settings_->value(key).toLongLong(&unitsOk);
        if (tfOk && unitsOk) out.set(symbolFromSegment(parts[0]).toStdString(), tf, units);
    }
    settings_->endGroup();
    return out;
}
bool HeatmapSettingsStore::saveManualTick(const std::string &symbol, int64_t tf, int64_t units) {
    ManualTickMemory check;
    if (!check.set(symbol, tf, units)) return false;
    settings_->setValue("heatmap/manualTick/" + segment(QString::fromStdString(symbol)) + "/" + QString::number(tf), qint64(units));
    return true;
}
HeatmapBudgets HeatmapSettingsStore::loadBudgets(const ClientHeatmapConfig &c) const {
    const auto read = [&](const char *key, uint64_t fallback) {
        bool ok = false;
        const auto value = settings_->value(QString("heatmap/budgets/") + key, qulonglong(fallback)).toULongLong(&ok);
        return size_t(std::clamp<uint64_t>(ok ? value : fallback, MiB, 4096 * MiB));
    };
    HeatmapBudgets b;
    b.decodedChunks = read("decodedChunks", c.decodedChunkBytes);
    b.spanSources = read("spanSources", c.spanSourceBytes);
    b.cpuCeiling = std::max(read("cpuCeiling", c.cpuCeilingBytes), b.decodedChunks + b.spanSources);
    b.gpuPerChart = chartDefaults(c).gpuCapBytes;
    return b;
}
void HeatmapSettingsStore::saveBudgets(const HeatmapBudgets &b) {
    if (b.decodedChunks == 0 || b.spanSources == 0 || b.decodedChunks > b.cpuCeiling ||
        b.spanSources > b.cpuCeiling - b.decodedChunks) return;
    settings_->setValue("heatmap/budgets/decodedChunks", qulonglong(b.decodedChunks));
    settings_->setValue("heatmap/budgets/spanSources", qulonglong(b.spanSources));
    settings_->setValue("heatmap/budgets/cpuCeiling", qulonglong(b.cpuCeiling));
}
} // namespace heatmap
