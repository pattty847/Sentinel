#pragma once
#include "render/TradeBubbleStyle.hpp"
#include <QObject>
#include <QSettings>

namespace chart {
// GUI-thread notification only. Render code consumes plain value snapshots.
class TradeBubbleStyleChanges : public QObject {
    Q_OBJECT
public:
    static TradeBubbleStyleChanges& instance();
signals:
    void changed();
};
// Mirrors TpoStyleStore: global element properties plus a per-product base filter.
class TradeBubbleStyleStore {
public:
    TradeBubbleStyleStore() = default;
    explicit TradeBubbleStyleStore(QSettings& settings) : settings_(&settings) {}
    trade_bubbles::StyleModel load(const std::string& product) const;
    // Appearance edits leave the per-product automatic minimum unpinned.
    void save(const std::string& product, const trade_bubbles::StyleModel& style, bool minimumEdited = false);
    void clear(const std::string& product);
    bool hasMinimum(const std::string& product) const;
    static trade_bubbles::StyleModel defaults(const std::string& product);
    static void notePrice(const std::string& product, double price, int64_t timeMs = 0);
    static double minimum(const std::string& product);
private:
    QSettings owned_{QSettings::defaultFormat(), QSettings::UserScope, "Sentinel", "SentinelTerminal"};
    QSettings* settings_ = &owned_;
};
} // namespace chart
