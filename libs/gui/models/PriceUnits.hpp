#pragma once
// The display unit of each symbol's prices (PriceDisplayUnit, owner 2026-11-08 option A):
// one decision per symbol, shared by every widget that shows its prices (axis, book,
// toolbar, overlays). The first usable reference price of a symbol decides its unit
// for the rest of the process, so the unit never flips while a price moves near a
// threshold. GUI thread. The Agent API keeps raw prices.
#include "PriceDisplayUnit.hpp"
#include <QHash>
#include <QObject>
#include <QString>

class PriceUnits : public QObject {
    Q_OBJECT
public:
    static PriceUnits& instance();

    // A reference price of `symbol` (book mid, last trade). Decides the unit once.
    void observe(const QString& symbol, double referencePrice);
    sentinel::PriceDisplayUnit unitFor(const QString& symbol) const { return m_units.value(symbol); }
    // "PEPE x1M" for a scaled symbol; empty when plain.
    QString label(const QString& symbol) const;
    // `raw` in the symbol's unit with the decimals of `rawStep`: "5.04" (scaled) or the
    // plain number. No currency sign.
    QString format(const QString& symbol, double raw, double rawStep) const;
    void clearForTest() { m_units.clear(); }

signals:
    void unitChanged(const QString& symbol);

private:
    PriceUnits() = default;
    QHash<QString, sentinel::PriceDisplayUnit> m_units;
};
