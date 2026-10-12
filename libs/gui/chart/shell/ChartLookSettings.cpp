#include "chart/shell/ChartLookSettings.hpp"
#include "chart/layers/ChartLayers.hpp"
#include "chart/core/ChartViewController.hpp"
#include "render/AxisTextService.hpp"
#include "render/heatmap/HeatmapSettingsStore.hpp"
#include "servermodel/SessionManager.hpp"
#include "SentinelLogging.hpp"
#include <algorithm>
#include <cmath>

namespace chart {
void ChartLookSettings::setInitialTone(const ClientConfig& config) {
    gamma_ = config.heatmap.gamma;
    contrast_ = config.heatmap.contrast;
    shaderFloor_ = config.heatmap.shaderFloor;
}

void ChartLookSettings::syncGpuTone() {
    if (applyTone) {
        applyTone(static_cast<float>(gamma_), static_cast<float>(contrast_),
                  static_cast<float>(shaderFloor_));
    }
}

void ChartLookSettings::setHeatmapGamma(double gamma) {
    const double clamped = std::clamp(gamma, 0.1, 5.0);
    if (std::abs(gamma_ - clamped) < 1e-6) return;
    gamma_ = clamped;
    syncGpuTone();
    update();
    changed(Notice::Gamma);
}

void ChartLookSettings::setHeatmapContrast(double contrast) {
    const double clamped = std::clamp(contrast, 0.1, 5.0);
    if (std::abs(contrast_ - clamped) < 1e-6) return;
    contrast_ = clamped;
    syncGpuTone();
    update();
    changed(Notice::Contrast);
}

void ChartLookSettings::setHeatmapShaderFloor(double floor) {
    const double clamped = std::clamp(floor, 0.0, 0.5);
    if (std::abs(shaderFloor_ - clamped) < 1e-6) return;
    shaderFloor_ = clamped;
    syncGpuTone();
    update();
    changed(Notice::ShaderFloor);
}

void ChartLookSettings::setCandleStyle(int style) {
    style = std::clamp(style, 0, 2);
    if (candleStyle_ == style) return;
    candleStyle_ = style;
    changed(Notice::CandleStyle);
}

void ChartLookSettings::setCandleAppearance(const heatmap::HeatmapChartSettings& settings) {
    const QColor up(QString::fromStdString(settings.candleUpColor));
    const QColor down(QString::fromStdString(settings.candleDownColor));
    const QString wick = QString::fromStdString(settings.candleWickColor);
    if (candleUpColor_ == up && candleDownColor_ == down && candleWickColor_ == wick &&
        candleBodyOpacity_ == settings.candleBodyOpacity && candleWickWidth_ == settings.candleWickWidth) return;
    candleUpColor_ = up;
    candleDownColor_ = down;
    candleWickColor_ = wick;
    candleBodyOpacity_ = settings.candleBodyOpacity;
    candleWickWidth_ = settings.candleWickWidth;
    changed(Notice::CandleAppearance);
}

void ChartLookSettings::applyTpoConfig(const ClientTpoConfig& config) {
    TpoOverlayRenderer::Style style = layers_.tpo.renderer.style();
    style.layout = tpo::parseLayout(config.layout, tpo::Layout::Collapsed);
    style.theme = tpo::parseTheme(config.theme, tpo::Theme::Rainbow);
    style.rowPx = config.rowPx;
    style.maxSessions = config.sessions;
    layers_.tpo.renderer.setStyle(style);
    const int sessionType = tpo::parseSessionType(config.session, 4);
    const int period = static_cast<int>(tpo::resolvePeriodMs(
        sessionType, static_cast<int64_t>(std::max(1, config.periodMinutes)) * 60000));
    if (sessionType != tpoSessionType_ || period != tpoTimeframeMs_) {
        tpoSessionType_ = sessionType;
        tpoTimeframeMs_ = period;
        changed(Notice::TpoConfig);
    }
    sLog_Render("TPO config: layout=" << tpo::layoutName(style.layout)
                << " theme=" << tpo::themeName(style.theme)
                << " session=" << tpo::sessionTypeName(sessionType)
                << " periodMs=" << period << " sessions=" << layers_.tpo.renderer.style().maxSessions
                << " rowPx=" << layers_.tpo.renderer.style().rowPx);
    update();
}

void ChartLookSettings::setTpoTimeframeMs(int timeframeMs) {
    const int resolved = static_cast<int>(tpo::resolvePeriodMs(tpoSessionType_, timeframeMs));
    if (tpoTimeframeMs_ == resolved) return;
    tpoTimeframeMs_ = resolved;
    changed(Notice::TpoConfig);
}

void ChartLookSettings::setTpoSessionType(int sessionType) {
    const int clamped = (sessionType >= 0 &&
                         sessionType <= static_cast<int>(SessionManager::SessionType::M1)) ? sessionType : 4;
    const int period = static_cast<int>(tpo::resolvePeriodMs(clamped, tpoTimeframeMs_));
    if (tpoSessionType_ == clamped && tpoTimeframeMs_ == period) return;
    tpoSessionType_ = clamped;
    tpoTimeframeMs_ = period;
    changed(Notice::TpoConfig);
}

void ChartLookSettings::setTpoLayout(const QString& layout) {
    auto style = layers_.tpo.renderer.style();
    const auto next = tpo::parseLayout(layout.toStdString(), style.layout);
    if (next == style.layout) return;
    style.layout = next;
    layers_.tpo.renderer.setStyle(style);
    sLog_Render("TPO layout=" << tpo::layoutName(next));
    update();
    changed(Notice::TpoStyle);
}

void ChartLookSettings::setTpoTheme(const QString& theme) {
    auto style = layers_.tpo.renderer.style();
    const auto next = tpo::parseTheme(theme.toStdString(), style.theme);
    if (next == style.theme) return;
    style.theme = next;
    layers_.tpo.renderer.setStyle(style);
    sLog_Render("TPO theme=" << tpo::themeName(next));
    update();
    changed(Notice::TpoStyle);
}

QString ChartLookSettings::tpoLayout() const {
    return QString::fromLatin1(tpo::layoutName(layers_.tpo.renderer.style().layout));
}

QString ChartLookSettings::tpoTheme() const {
    return QString::fromLatin1(tpo::themeName(layers_.tpo.renderer.style().theme));
}

int ChartLookSettings::tpoSessions() const { return layers_.tpo.renderer.style().maxSessions; }

void ChartLookSettings::applyClientConfig(const ClientConfig& config, ChartViewController& view,
                                          AxisTextService* axes) {
    setHeatmapGamma(config.heatmap.gamma);
    setHeatmapContrast(config.heatmap.contrast);
    setHeatmapShaderFloor(config.heatmap.shaderFloor);
    syncGpuTone();
    view.initialViewSettings().columnPx = std::clamp(config.heatmap.initialColumnPx, 2, 64);
    view.initialViewSettings().pricePct = std::clamp(config.heatmap.initialPricePct, 0, 100);
    if (axes) {
        axes->setAxisLabelPxOverride(config.gui.axisLabelPx);
        axes->refreshAxisLayout();
    }
    applyTpoConfig(config.tpo);
    if (onViewportChanged) onViewportChanged();
    update();
}

void ChartLookSettings::applyServerConfig(const ServerConfig& config, ChartViewController& view) {
    int64_t forcedTf = config.chart.defaultTimeframeMs;
    if (forcedTf <= 0 && !config.chart.timeframesMs.empty()) {
        forcedTf = config.chart.timeframesMs.front();
    }
    if (forcedTf > 0) view.setTimeframe(static_cast<int>(forcedTf));
}
} // namespace chart

void chart::ChartLookSettings::changed(Notice notice) {
    switch (notice) {
      case Notice::TpoConfig: emit tpo.tpoConfigChanged(); break;
      case Notice::TpoStyle: emit tpo.tpoStyleChanged(); break;
      case Notice::Gamma: emit heatmapGammaChanged(); break;
      case Notice::Contrast: emit heatmapContrastChanged(); break;
      case Notice::ShaderFloor: emit heatmapShaderFloorChanged(); break;
      case Notice::CandleStyle: emit candleStyleChanged(); break;
      case Notice::CandleAppearance: emit candleAppearanceChanged(); break;
    }
    if (notify) notify(notice);
}
