#include "HeatmapSettingsStore.hpp"
#include <QDateTime>
#include "HeatmapSourceController.hpp"
#include <QJsonArray>
#include <QRegularExpression>
#include <algorithm>
#include <cmath>
#include <limits>
#include <variant>

namespace heatmap {
namespace {
constexpr uint64_t MiB = 1ull << 20;
const QStringList palettes{"Electric", "Fire", "Ocean", "Monochrome", "Matrix", "Custom"};
const QRegularExpression candleColor("^#[0-9a-fA-F]{6}$");
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

// One definition per accepted settings key. applySettingsPatch (validation),
// clampSettings, settingsJson and settingsSchema (GET /api/v1/schema) all read this
// table, so the API reference cannot drift from what the store accepts.
using S = HeatmapChartSettings;
using Field = std::variant<bool S::*, int S::*, int64_t S::*, uint64_t S::*, double S::*, std::string S::*,
                           std::vector<GradientStop> S::*, TickMode S::*>;
enum class Kind { Boolean, Integer, Number, Choice, Color, Gradient };
constexpr double kInf = std::numeric_limits<double>::infinity();
struct Def {
    const char *key;
    Field field;
    Kind kind;
    double min = -kInf, max = kInf; // Integer/Number: clamp range, inclusive
    bool reject = false;             // a patch outside [min, max] is rejected instead of clamped
    QStringList values;              // Choice: allowed values; Color: accepted words besides #RRGGBB
    const char *description = "";
    const char *error = nullptr;     // rejection message when the generated one does not fit
};
Def flag(const char *k, bool S::*f, const char *d) { return {k, f, Kind::Boolean, -kInf, kInf, false, {}, d}; }
template <typename T> Def integer(const char *k, T S::*f, double lo, double hi, const char *d, const char *rejectError = nullptr) {
    return {k, f, Kind::Integer, lo, hi, rejectError != nullptr, {}, d, rejectError};
}
Def number(const char *k, double S::*f, double lo, double hi, const char *d, const char *rejectError = nullptr) {
    return {k, f, Kind::Number, lo, hi, rejectError != nullptr, {}, d, rejectError};
}
template <typename T> Def choice(const char *k, T S::*f, QStringList v, const char *d, const char *error = nullptr) {
    return {k, f, Kind::Choice, -kInf, kInf, false, std::move(v), d, error};
}
Def color(const char *k, std::string S::*f, const char *d, QStringList words = {}) {
    return {k, f, Kind::Color, -kInf, kInf, false, std::move(words), d, "Candle colors must be #RRGGBB (wick also accepts auto)"};
}
Def stops(const char *k, std::vector<GradientStop> S::*f, const char *d) {
    return {k, f, Kind::Gradient, -kInf, kInf, false, {}, d, "Invalid gradient (2..16 ordered stops, endpoints 0 and 1, hex colors)"};
}
const std::vector<Def> &defs() {
    static const std::vector<Def> table{
        choice("tickMode", &S::tickMode, {"auto", "manual"}, "Auto picks the heatmap tick from zoom; manual locks manualTick."),
        integer("manualTick", &S::manualTick, 1, 1e12, "Manual heatmap tick in integer price units (price x priceScale), raised to a 1/2/2.5/5 x 10^k preset."),
        number("minRowPx", &S::minRowPx, 0.5, 32, "Auto tick: target minimum heatmap row height in physical pixels."),
        number("hysteresis", &S::hysteresis, 0, 0.9, "Auto tick: margin around the row-height threshold that reduces tick switching."),
        integer("crossfadeMs", &S::crossfadeMs, 0, 2000, "Blend old and new heatmap rows over this many milliseconds on a tick change; 0 switches at once."),
        flag("showBandEdges", &S::showBandEdges, "Draw the recorded book band edges on the heatmap."),
        choice("palettePreset", &S::palettePreset, palettes, "Heatmap colour scale; Custom uses bidGradient and askGradient.", "Unknown palettePreset"),
        stops("bidGradient", &S::bidGradient, "Bid colours from low to high liquidity: 2..16 {position, color} stops, positions 0..1 increasing, #RRGGBB[AA]."),
        stops("askGradient", &S::askGradient, "Ask colours from low to high liquidity: 2..16 {position, color} stops, positions 0..1 increasing, #RRGGBB[AA]."),
        number("sensitivityMin", &S::sensitivityMin, 1e-9, 1e12, "Liquidity range low in base units; cells below it stay hidden. Saved per symbol, not per chart."),
        number("sensitivityMax", &S::sensitivityMax, 1e-9, 1e15, "Liquidity range high in base units (raised to 2 x sensitivityMin when not above it). Saved per symbol."),
        number("opacity", &S::opacity, 0, 1, "Heatmap opacity: 0 transparent, 1 solid."),
        flag("smoothZoomOut", &S::smoothZoomOut, "Average covered heatmap cells when a cell is smaller than a screen pixel."),
        integer("gpuCapBytes", &S::gpuCapBytes, double(MiB), double(4096 * MiB), "GPU heatmap cache budget for this chart, in bytes."),
        integer("uploadBudgetBytes", &S::uploadBudgetBytes, 1, double(128 * MiB), "GPU heatmap upload budget per frame, in bytes; also capped at gpuCapBytes."),
        integer("prefetchTiles", &S::prefetchTiles, 0, 16, "Heatmap tiles prefetched beyond each side of the view."),
        integer("liveMinIntervalMs", &S::liveMinIntervalMs, 100, 5000, "Minimum spacing of GPU refreshes of the newest heatmap columns, in milliseconds."),
        flag("showTelemetry", &S::showTelemetry, "Show the heatmap telemetry dock."),
        flag("showTrades", &S::showTrades, "Draw trade bubbles."),
        flag("tradesAboveCandles", &S::tradesAboveCandles, "Draw trade bubbles above candles; false draws them below."),
        number("tradeMinNotional", &S::tradeMinNotional, -1, 1e12, "Trade bubble minimum in quote currency: 0 all trades, -1 the visible median, >0 a fixed minimum."),
        choice("bubblePlacement", &S::bubblePlacement, {"individual", "auto", "slot"}, "Trade bubbles at exchange time, per bar side at VWAP (slot), or chosen by column width (auto)."),
        choice("bubblesOverFootprint", &S::bubblesOverFootprint, {"over", "dim"}, "Keep trade bubbles at full opacity over footprint numbers, or dim them to 35%."),
        choice("paperPositionStyle", &S::paperPositionStyle, {"tag", "band"}, "Paper position as a compact PnL tag at the entry line, or a full-width entry-to-mark band."),
        flag("showLabels", &S::showLabels, "Show liquidity sizes on heatmap cells where the text fits."),
        choice("labelCurrency", &S::labelCurrency, {"usd", "asset"}, "Liquidity labels as USD notional or base-asset size."),
        flag("labelCurrencySymbol", &S::labelCurrencySymbol, "Prefix USD liquidity labels with '$'."),
        number("labelMinPx", &S::labelMinPx, 8, 24, "Smallest liquidity label text in pixels; labels that cannot fit are hidden."),
        number("labelMaxPx", &S::labelMaxPx, 8, 32, "Largest liquidity label text in pixels; never below labelMinPx."),
        color("candleUpColor", &S::candleUpColor, "Colour of candles that close at or above their open."),
        color("candleDownColor", &S::candleDownColor, "Colour of candles that close below their open."),
        color("candleWickColor", &S::candleWickColor, "Wick colour; auto follows each candle's body colour.", {"auto"}),
        number("candleBodyOpacity", &S::candleBodyOpacity, 0, 1, "Candle body opacity: 0 transparent, 1 solid."),
        integer("candleWickWidth", &S::candleWickWidth, 1, 3, "Wick thickness in device pixels.", "candleWickWidth must be 1..3 device pixels"),
        number("candleBodyFraction", &S::candleBodyFraction, 0.3, 1.0, "Candle body width as a fraction of its time column."),
        integer("candleMaxBodyPx", &S::candleMaxBodyPx, 5, 120, "Maximum candle body width in logical pixels."),
        flag("showVolumeBars", &S::showVolumeBars, "Show candle volume at the bottom of the price pane."),
        number("volumeBandPercent", &S::volumeBandPercent, 10, 40, "Share of the price pane used by volume bars, in percent."),
        number("volumeOpacity", &S::volumeOpacity, 0.1, 1, "Volume bar opacity."),
        flag("flowFooter", &S::flowFooter, "Show the per-bar DELTA / CUM / VOL footer."),
        integer("cvdStripPx", &S::cvdStripPx, 0, 400, "Cumulative volume delta strip height in logical pixels; 0 hides it."),
        choice("cvdReset", &S::cvdReset, {"visible", "utcDay", "tpoSession"}, "Where CVD starts from zero: the view's left edge, the UTC day, or the TPO session open."),
        choice("cvdStyle", &S::cvdStyle, {"line", "area"}, "CVD drawn as a line or a filled area."),
        choice("cvdFillAnchor", &S::cvdFillAnchor, {"auto", "zero", "edge", "none"}, "What the CVD area fills to: auto, zero, the plot-band edge, or nothing (line only)."),
        choice("footprintIntensity", &S::footprintIntensity, {"bar", "window"}, "Footprint fill length scaled to the bar's or the visible window's largest cell."),
        choice("footprintCandle", &S::footprintCandle, {"spine", "full"}, "Footprint bars draw a thin spine candle or the full candle style."),
        number("footprintDustNotional", &S::footprintDustNotional, 0, kInf, "Footprint sides below this quote amount get faint text and no fill.", "footprintDustNotional must be nonnegative"),
        flag("footprintNumbers", &S::footprintNumbers, "Show footprint bid and ask amounts when they fit."),
        flag("showLadder", &S::showLadder, "Show the chart-edge book ladder strip."),
        integer("ladderWidthPx", &S::ladderWidthPx, 60, 240, "Book ladder strip width in logical pixels."),
        flag("showWallRadar", &S::showWallRadar, "Mark walls on the book ladder (needs showLadder)."),
        number("wallMultiple", &S::wallMultiple, 1.5, 20, "Wall threshold as a multiple of the side's median nonempty bin near the touch."),
    };
    return table;
}
const Def *findDef(const QString &key) {
    for (const auto &def : defs()) if (key == QLatin1String(def.key)) return &def;
    return nullptr;
}
QJsonValue::Type jsonType(Kind kind) {
    switch (kind) {
    case Kind::Boolean: return QJsonValue::Bool;
    case Kind::Integer: case Kind::Number: return QJsonValue::Double;
    case Kind::Gradient: return QJsonValue::Array;
    default: return QJsonValue::String;
    }
}
template <typename... F> struct Visit : F... { using F::operator()...; };
template <typename... F> Visit(F...) -> Visit<F...>;
QJsonValue read(const Def &def, const S &s) {
    return std::visit(Visit{
        [&](bool S::*f) { return QJsonValue(s.*f); },
        [&](int S::*f) { return QJsonValue(s.*f); },
        [&](int64_t S::*f) { return QJsonValue(qint64(s.*f)); },
        [&](uint64_t S::*f) { return QJsonValue(qint64(s.*f)); },
        [&](double S::*f) { return QJsonValue(s.*f); },
        [&](std::string S::*f) { return QJsonValue(QString::fromStdString(s.*f)); },
        [&](std::vector<GradientStop> S::*f) { return QJsonValue(gradientJson(s.*f)); },
        [&](TickMode S::*f) { return QJsonValue(s.*f == TickMode::Auto ? "auto" : "manual"); },
    }, def.field);
}
// v has passed checkValue for def; numeric limits are applied afterwards by clampSettings.
void write(const Def &def, S &s, const QJsonValue &v) {
    std::visit(Visit{
        [&](bool S::*f) { s.*f = v.toBool(); },
        [&](int S::*f) { s.*f = int(std::clamp(v.toDouble(), -2147483647.0, 2147483647.0)); },
        [&](int64_t S::*f) { s.*f = int64_t(v.toDouble()); },
        [&](uint64_t S::*f) { s.*f = uint64_t(std::max(0.0, v.toDouble())); },
        [&](double S::*f) { s.*f = v.toDouble(); },
        [&](std::string S::*f) { s.*f = v.toString().toStdString(); },
        [&](std::vector<GradientStop> S::*f) { s.*f = gradient(v); },
        [&](TickMode S::*f) { s.*f = v == "manual" ? TickMode::Manual : TickMode::Auto; },
    }, def.field);
}
bool colorOk(const Def &def, const QString &v) { return def.values.contains(v) || candleColor.match(v).hasMatch(); }
template <typename T> void clampInteger(const Def &def, T &v) { v = std::clamp<T>(v, T(def.min), T(def.max)); }
void clampField(const Def &def, S &s, const S &d) {
    std::visit(Visit{
        [&](bool S::*) {},
        [&](int S::*f) { clampInteger(def, s.*f); },
        [&](int64_t S::*f) { clampInteger(def, s.*f); },
        [&](uint64_t S::*f) { clampInteger(def, s.*f); },
        [&](double S::*f) { s.*f = finiteClamp(s.*f, def.min, def.max, d.*f); },
        [&](std::string S::*f) {
            const auto v = QString::fromStdString(s.*f);
            if (def.kind == Kind::Choice ? !def.values.contains(v) : !colorOk(def, v)) s.*f = d.*f;
        },
        [&](std::vector<GradientStop> S::*f) { if (!validGradient(gradientJson(s.*f))) s.*f = d.*f; },
        [&](TickMode S::*f) { if (s.*f != TickMode::Auto && s.*f != TickMode::Manual) s.*f = d.*f; },
    }, def.field);
}
QString joinOr(const QStringList &values) {
    return values.size() < 2 ? values.join("") : values.mid(0, values.size() - 1).join(", ") + " or " + values.last();
}
QString checkValue(const Def &def, const QString &key, const QJsonValue &v) {
    if (v.type() != jsonType(def.kind)) return "Invalid type for heatmap setting: " + key;
    if (v.isDouble() && (!std::isfinite(v.toDouble()) || std::abs(v.toDouble()) > 9007199254740991.0))
        return "Expected a finite safe number: " + key;
    if (def.kind == Kind::Integer && std::floor(v.toDouble()) != v.toDouble()) return "Expected an integer: " + key;
    if (def.reject && (v.toDouble() < def.min || v.toDouble() > def.max)) return def.error;
    if (def.kind == Kind::Choice && !def.values.contains(v.toString()))
        return def.error ? QString(def.error) : key + " must be " + joinOr(def.values);
    if (def.kind == Kind::Color && !colorOk(def, v.toString())) return def.error;
    if (def.kind == Kind::Gradient && !validGradient(v)) return def.error;
    return {};
}
}

void clampSettings(HeatmapChartSettings &s) {
    const HeatmapChartSettings d;
    for (const auto &def : defs()) clampField(def, s, d);
    // Limits that depend on another key, applied after both are clamped.
    s.manualTick = presetAtLeast(double(s.manualTick), 1);
    if (s.sensitivityMax <= s.sensitivityMin) s.sensitivityMax = s.sensitivityMin * 2;
    s.uploadBudgetBytes = std::min(s.uploadBudgetBytes, s.gpuCapBytes);
    s.labelMaxPx = std::max(s.labelMaxPx, s.labelMinPx);
}
HeatmapChartSettings chartDefaults(const ClientHeatmapConfig &c) {
    HeatmapChartSettings s;
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
    s.candleBodyFraction = c.candleBodyFraction;
    s.candleMaxBodyPx = c.candleMaxBodyPx;
    s.showVolumeBars = c.showVolumeBars;
    s.volumeBandPercent = c.volumeBandPercent;
    s.volumeOpacity = c.volumeOpacity;
    clampSettings(s);
    return s;
}
QJsonObject settingsJson(const HeatmapChartSettings &s) {
    QJsonObject out;
    for (const auto &def : defs()) out.insert(QLatin1String(def.key), read(def, s));
    return out;
}
QString applySettingsPatch(HeatmapChartSettings &s, const QJsonObject &patch) {
    HeatmapChartSettings out = s;
    for (auto it = patch.begin(); it != patch.end(); ++it) {
        const Def *def = findDef(it.key());
        if (!def) return "Unknown heatmap setting: " + it.key();
        if (const auto error = checkValue(*def, it.key(), it.value()); !error.isEmpty()) return error;
        write(*def, out, it.value());
    }
    clampSettings(out);
    s = std::move(out);
    return {};
}
QJsonObject settingsSchema() {
    static const char *const kinds[] = {"boolean", "integer", "number", "enum", "color", "gradient"};
    const HeatmapChartSettings defaults;
    const auto bound = [](double v) { return std::isfinite(v) ? QJsonValue(v) : QJsonValue(QJsonValue::Null); };
    QJsonObject out;
    for (const auto &def : defs()) {
        QJsonObject entry{{"type", kinds[int(def.kind)]}, {"default", read(def, defaults)}, {"persists", true},
                          {"description", def.description}};
        if (def.kind == Kind::Integer || def.kind == Kind::Number) {
            entry["min"] = bound(def.min);
            entry["max"] = bound(def.max);
            entry["outOfRange"] = def.reject ? "rejected" : "clamped";
        }
        if (def.kind == Kind::Color) entry["format"] = "#RRGGBB";
        if (!def.values.isEmpty()) entry["values"] = QJsonArray::fromStringList(def.values);
        out.insert(QLatin1String(def.key), entry);
    }
    return out;
}
HeatmapSettingsStore::HeatmapSettingsStore() = default;
HeatmapSettingsStore::HeatmapSettingsStore(QSettings &settings) : settings_(&settings) {}
HeatmapChartSettings HeatmapSettingsStore::loadAt(const QString &prefix, HeatmapChartSettings defaults) const {
    const auto schema = settingsJson(defaults);
    QJsonObject patch;
    for (auto it = schema.begin(); it != schema.end(); ++it) {
        if (it.key() == "sensitivityMin" || it.key() == "sensitivityMax") continue;
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
        if (it.key() == "cvdReset" && decoded == "none") decoded = QStringLiteral("visible");
        auto check = defaults;
        if (applySettingsPatch(check, {{it.key(), decoded}}).isEmpty()) patch[it.key()] = decoded;
    }
    applySettingsPatch(defaults, patch);
    return defaults;
}
void HeatmapSettingsStore::saveAt(const QString &prefix, HeatmapChartSettings value) {
    clampSettings(value);
    const auto json = settingsJson(value);
    for (auto it = json.begin(); it != json.end(); ++it) {
        if (it.key() == "sensitivityMin" || it.key() == "sensitivityMax") {
            settings_->remove(prefix + it.key());
            continue;
        }
        settings_->setValue(prefix + it.key(), it.value().toVariant());
    }
}
HeatmapChartSettings HeatmapSettingsStore::load(const QString &id, const ClientHeatmapConfig &defaults) const {
    return loadAt("heatmap/" + segment(id) + "/", chartDefaults(defaults));
}
void HeatmapSettingsStore::save(const QString &id, HeatmapChartSettings value) {
    saveAt("heatmap/" + segment(id) + "/", std::move(value));
}
void HeatmapSettingsStore::removeLegacySensitivity(const QString &id) {
    const auto prefix = "heatmap/" + segment(id) + "/";
    settings_->remove(prefix + "sensitivityMin");
    settings_->remove(prefix + "sensitivityMax");
}
QString HeatmapSettingsStore::applyChartPatch(const QString &id, HeatmapChartSettings &current,
    const QJsonObject &patch, bool persist, const ClientHeatmapConfig &defaults, const std::string &symbol, int64_t tfMs,
    std::optional<SensitivityRange> sensitivity) {
    auto next = current;
    if (const auto error = applySettingsPatch(next, patch); !error.isEmpty()) return error;
    if (persist) {
        auto saved = load(id, defaults);
        if (const auto error = applySettingsPatch(saved, patch); !error.isEmpty()) return error;
        save(id, saved);
        // Merely entering Manual must not overwrite this symbol/tf's remembered
        // choice with a chart-wide default (or a previous process-only override).
        if (next.tickMode == TickMode::Manual && patch.contains("manualTick"))
            saveManualTick(symbol, tfMs, next.manualTick);
        if (!symbol.empty() && (patch.contains("sensitivityMin") || patch.contains("sensitivityMax"))) {
            auto range = sensitivity.value_or(SensitivityRange{next.sensitivityMin, next.sensitivityMax,
                RangeOrigin::User, QDateTime::currentMSecsSinceEpoch(), 0});
            range.min = next.sensitivityMin;
            range.max = next.sensitivityMax;
            saveSensitivity(symbol, range);
        }
    }
    current = std::move(next);
    return {};
}
void HeatmapSettingsStore::saveLayout(const QString &name, const QString &id, const ClientHeatmapConfig &defaults) {
    if (name == "_last_session") return;
    saveAt("layouts/" + segment(name) + "/heatmap/" + segment(id) + "/", load(id, defaults));
}
void HeatmapSettingsStore::restoreLayoutInto(const QString &name, const QString &id, HeatmapChartSettings &current,
                                            const ClientHeatmapConfig &defaults) {
    if (name == "_last_session") return;
    current = restoreLayout(name, id, defaults);
}
HeatmapChartSettings HeatmapSettingsStore::restoreLayout(const QString &name, const QString &id, const ClientHeatmapConfig &defaults) {
    if (name == "_last_session") return load(id, defaults);
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
std::optional<HeatmapSettingsStore::SensitivityRange> HeatmapSettingsStore::loadSensitivity(const std::string &symbol) const {
    if (symbol.empty()) return std::nullopt;
    const auto prefix = "heatmap/sensitivity/" + segment(QString::fromStdString(symbol)) + "/";
    bool lowOk = false, highOk = false;
    const double low = settings_->value(prefix + "min").toDouble(&lowOk);
    const double high = settings_->value(prefix + "max").toDouble(&highOk);
    if (!lowOk || !highOk || !std::isfinite(low) || !std::isfinite(high) || low < 1e-9 || low > 1e12 ||
        high > 1e15 || high <= low) return std::nullopt;
    const QString origin = settings_->value(prefix + "origin", "user").toString();
    bool timeOk = false, priceOk = false;
    const int64_t atMs = settings_->value(prefix + "fittedAtMs", 0).toLongLong(&timeOk);
    const double atPrice = settings_->value(prefix + "fittedAtPrice", 0.0).toDouble(&priceOk);
    if ((origin != "auto" && origin != "user") || !timeOk || atMs < 0 || !priceOk ||
        !std::isfinite(atPrice) || atPrice < 0) return std::nullopt;
    return SensitivityRange{low, high, origin == "auto" ? RangeOrigin::Auto : RangeOrigin::User, atMs, atPrice};
}
bool HeatmapSettingsStore::saveSensitivity(const std::string &symbol, SensitivityRange range) {
    if (symbol.empty() || !std::isfinite(range.min) || !std::isfinite(range.max) ||
        range.min < 1e-9 || range.min > 1e12 || range.max > 1e15 || range.max <= range.min ||
        range.fittedAtMs < 0 || !std::isfinite(range.fittedAtPrice) || range.fittedAtPrice < 0) return false;
    const auto prefix = "heatmap/sensitivity/" + segment(QString::fromStdString(symbol)) + "/";
    settings_->setValue(prefix + "min", range.min);
    settings_->setValue(prefix + "max", range.max);
    settings_->setValue(prefix + "origin", range.origin == RangeOrigin::Auto ? "auto" : "user");
    settings_->setValue(prefix + "fittedAtMs", qlonglong(range.fittedAtMs));
    settings_->setValue(prefix + "fittedAtPrice", range.fittedAtPrice);
    return true;
}
void HeatmapSettingsStore::removeSensitivity(const std::string &symbol) {
    if (!symbol.empty()) settings_->remove("heatmap/sensitivity/" + segment(QString::fromStdString(symbol)));
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
