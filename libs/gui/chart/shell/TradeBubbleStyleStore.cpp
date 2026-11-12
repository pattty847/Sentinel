#include "TradeBubbleStyleStore.hpp"
#include "config/GuiConfigStore.hpp"
#include "themes/ThemePalette.hpp"
#include <map>

namespace chart {
namespace {
constexpr auto kGroup = "tradeBubbles/style";
struct Quote { double price = 0; int64_t timeMs = 0; double minimum = 0; };
std::map<std::string, Quote> prices;
QString minimumKey(const std::string& product) {
    return "tradeBubbles/minimum/" + QString::fromStdString(product);
}
tpo::Rgba rgba(const QColor& c) { return {uint8_t(c.red()), uint8_t(c.green()), uint8_t(c.blue()), 255}; }
using Field = trade_bubbles::Element trade_bubbles::StyleModel::*;
constexpr std::pair<const char*, Field> fields[] = {
    {"buy", &trade_bubbles::StyleModel::buy}, {"sell", &trade_bubbles::StyleModel::sell},
    {"ring", &trade_bubbles::StyleModel::ring}};
}
TradeBubbleStyleChanges& TradeBubbleStyleChanges::instance() {
    static TradeBubbleStyleChanges changes;
    return changes;
}
void TradeBubbleStyleStore::notePrice(const std::string& product, double price, int64_t timeMs) {
    if (product.empty() || !std::isfinite(price) || price <= 0) return;
    auto& quote = prices[product];
    if (timeMs > 0 && timeMs < quote.timeMs) return;
    quote.price = price;
    quote.timeMs = timeMs;
}
double TradeBubbleStyleStore::minimum(const std::string& product) {
    const auto price = prices.find(product), btc = prices.find("BTC-USD");
    const double candidate = trade_bubbles::defaultMinimum(product, price == prices.end() ? 0 : price->second.price,
        btc == prices.end() ? 0 : btc->second.price, GuiConfigStore::instance().clientConfig().bubbles.minimumBtc);
    if (product == "BTC-USD" || price == prices.end()) return candidate;
    auto& active = price->second.minimum;
    active = trade_bubbles::stabilizedMinimum(candidate, active);
    return active;
}
trade_bubbles::StyleModel TradeBubbleStyleStore::defaults(const std::string& product) {
    trade_bubbles::StyleModel s;
    const auto& config = GuiConfigStore::instance().clientConfig().bubbles;
    s.buy.color = rgba(theme::positive());
    s.sell.color = rgba(theme::negative());
    s.ring.color = rgba(theme::chartBackground());
    s.buy.opacity = s.sell.opacity = config.fillOpacity;
    s.buy.width = s.sell.width = config.maxRadius;
    s.ring.width = config.ringWidth;
    s.ring.visible = config.ringVisible;
    s.minimumBaseSize = minimum(product);
    return trade_bubbles::clamped(s);
}
trade_bubbles::StyleModel TradeBubbleStyleStore::load(const std::string& product) const {
    auto s = defaults(product);
    settings_->beginGroup(kGroup);
    for (const auto& [name, field] : fields) {
        auto& e = s.*field;
        const QString prefix = QString::fromLatin1(name) + '.';
        tpo::parseHexColor(settings_->value(prefix + "color").toString().toStdString(), e.color);
        e.opacity = settings_->value(prefix + "opacity", e.opacity).toDouble();
        e.visible = settings_->value(prefix + "visible", e.visible).toBool();
        e.width = settings_->value(prefix + "width", e.width).toDouble();
    }
    settings_->endGroup();
    s.minimumBaseSize = settings_->value(minimumKey(product), s.minimumBaseSize).toDouble();
    return trade_bubbles::clamped(s);
}
bool TradeBubbleStyleStore::hasMinimum(const std::string& product) const { return settings_->contains(minimumKey(product)); }
void TradeBubbleStyleStore::save(const std::string& product, const trade_bubbles::StyleModel& style, bool minimumEdited) {
    const auto s = trade_bubbles::clamped(style);
    settings_->beginGroup(kGroup);
    settings_->remove("");
    for (const auto& [name, field] : fields) {
        const auto& e = s.*field;
        const QString prefix = QString::fromLatin1(name) + '.';
        settings_->setValue(prefix + "color", QString::fromStdString(tpo::hexColor(e.color)));
        settings_->setValue(prefix + "opacity", e.opacity);
        settings_->setValue(prefix + "visible", e.visible);
        settings_->setValue(prefix + "width", e.width);
    }
    settings_->endGroup();
    if (minimumEdited) settings_->setValue(minimumKey(product), s.minimumBaseSize);
    emit TradeBubbleStyleChanges::instance().changed();
}
void TradeBubbleStyleStore::clear(const std::string& product) {
    settings_->remove(kGroup);
    settings_->remove(minimumKey(product));
    emit TradeBubbleStyleChanges::instance().changed();
}
} // namespace chart
