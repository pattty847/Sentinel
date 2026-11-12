#include "chart/shell/TpoStyleStore.hpp"

namespace chart {
namespace {
constexpr auto kGroup = "tpo/style";
}

std::optional<tpo::StyleModel> TpoStyleStore::load() const {
    settings_->beginGroup(kGroup);
    tpo::KeyValues values;
    for (const QString& key : settings_->childKeys()) {
        values.emplace_back(key.toStdString(), settings_->value(key).toString().toStdString());
    }
    settings_->endGroup();
    if (values.empty()) return std::nullopt;
    return tpo::fromKeyValues(values, tpo::presetStyle(tpo::Preset::Letters));
}

void TpoStyleStore::save(const tpo::StyleModel& style) {
    settings_->beginGroup(kGroup);
    settings_->remove("");
    for (const auto& [key, value] : tpo::toKeyValues(style)) {
        settings_->setValue(QString::fromStdString(key), QString::fromStdString(value));
    }
    settings_->endGroup();
}

void TpoStyleStore::clear() {
    settings_->remove(kGroup);
}
} // namespace chart
