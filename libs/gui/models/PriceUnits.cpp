#include "PriceUnits.hpp"
#include "SentinelLogging.hpp"

PriceUnits& PriceUnits::instance() {
    static PriceUnits units;
    return units;
}

void PriceUnits::observe(const QString& symbol, double referencePrice) {
    if (symbol.isEmpty() || m_units.contains(symbol) || !std::isfinite(referencePrice) || !(referencePrice > 0)) return;
    const auto unit = sentinel::choosePriceDisplayUnit(referencePrice);
    m_units.insert(symbol, unit);
    if (!unit.scaled()) return;
    sLog_App("price display unit: symbol=" << symbol << " ref=" << referencePrice << " unit=" << unit.tag());
    emit unitChanged(symbol);
}

QString PriceUnits::label(const QString& symbol) const {
    const auto unit = unitFor(symbol);
    if (!unit.scaled()) return {};
    return symbol.section('-', 0, 0) + QLatin1Char(' ') + QLatin1String(unit.tag());
}

QString PriceUnits::format(const QString& symbol, double raw, double rawStep) const {
    return QString::fromStdString(sentinel::formatDisplayPrice(raw, rawStep, unitFor(symbol)));
}
