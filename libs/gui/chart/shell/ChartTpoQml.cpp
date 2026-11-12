#include "ChartTpoQml.hpp"
#include "ChartLookSettings.hpp"
namespace chart {
ChartTpoQml::ChartTpoQml(ChartLookSettings& look) : QObject(&look), look_(look) {}
void ChartTpoQml::applyTpoConfig(const ClientTpoConfig& config) { look_.applyTpoConfig(config); }

void ChartTpoQml::setTpoTimeframeMs(int ms) { look_.setTpoTimeframeMs(ms); }

void ChartTpoQml::setTpoSessionType(int type) { look_.setTpoSessionType(type); }

void ChartTpoQml::setTpoLayout(const QString& layout) { look_.setTpoLayout(layout); }

void ChartTpoQml::setTpoTheme(const QString& theme) { look_.setTpoTheme(theme); }

QString ChartTpoQml::tpoLayout() const { return look_.tpoLayout(); }

QString ChartTpoQml::tpoTheme() const { return look_.tpoTheme(); }
int ChartTpoQml::tpoTimeframeMs() const { return look_.tpoTimeframeMs(); }

int ChartTpoQml::tpoSessionType() const { return look_.tpoSessionType(); }

int ChartTpoQml::tpoSessions() const { return look_.tpoSessions(); }

void ChartTpoQml::setTpoPreset(const QString& preset) { look_.setTpoPreset(preset); }
QString ChartTpoQml::tpoPreset() const { return look_.tpoPreset(); }
tpo::StyleModel ChartTpoQml::tpoLook() const { return look_.tpoLook(); }
void ChartTpoQml::setTpoLook(const tpo::StyleModel& look) { look_.setTpoLook(look); }
void ChartTpoQml::resetTpoLook() { look_.resetTpoLook(); }
void ChartTpoQml::setStyleStore(std::shared_ptr<TpoStyleStore> store) { look_.setTpoStyleStore(std::move(store)); }

} // namespace chart
