#pragma once
#include "chart/core/TpoStyle.hpp"
#include <QSettings>
#include <optional>

namespace chart {
// The user's TPO style under "tpo/style/<key>" (tpo::toKeyValues keys), shared by every chart.
class TpoStyleStore {
public:
    TpoStyleStore() = default;  // QSettings("Sentinel", "SentinelTerminal"), process default format
    explicit TpoStyleStore(QSettings& settings) : settings_(&settings) {}
    // The saved style over the default preset, or nothing when none is saved.
    std::optional<tpo::StyleModel> load() const;
    void save(const tpo::StyleModel& style);
    void clear();
private:
    QSettings owned_{QSettings::defaultFormat(), QSettings::UserScope, "Sentinel", "SentinelTerminal"};
    QSettings* settings_ = &owned_;
};
} // namespace chart
