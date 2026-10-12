#pragma once
#include "config/ConfigTypes.hpp"
#include <QColor>
#include <QString>
#include <functional>

namespace heatmap { struct HeatmapChartSettings; }
namespace chart {
struct ChartLayers;
class ChartViewController;
}
class AxisTextService;

namespace chart {
class ChartLookSettings {
public:
    enum class Notice { TpoConfig, TpoStyle, Gamma, Contrast, ShaderFloor, CandleStyle, CandleAppearance };
    explicit ChartLookSettings(ChartLayers& layers) : layers_(layers) {}
    std::function<void()> requestUpdate, onViewportChanged;
    std::function<void(Notice)> notify;
    std::function<void(float, float, float)> applyTone;

    double gamma() const { return gamma_; }
    double contrast() const { return contrast_; }
    double shaderFloor() const { return shaderFloor_; }
    int candleStyle() const { return candleStyle_; }
    QColor candleUpColor() const { return candleUpColor_; }
    QColor candleDownColor() const { return candleDownColor_; }
    QString candleWickColor() const { return candleWickColor_; }
    double candleBodyOpacity() const { return candleBodyOpacity_; }
    int candleWickWidth() const { return candleWickWidth_; }
    int tpoTimeframeMs() const { return tpoTimeframeMs_; }
    int tpoSessionType() const { return tpoSessionType_; }
    int tpoSessions() const;
    QString tpoLayout() const;
    QString tpoTheme() const;

    void setInitialTone(const ClientConfig& config);
    void syncGpuTone();
    void setHeatmapGamma(double gamma);
    void setHeatmapContrast(double contrast);
    void setHeatmapShaderFloor(double floor);
    void setCandleStyle(int style);
    void setCandleAppearance(const heatmap::HeatmapChartSettings& settings);
    void applyTpoConfig(const ClientTpoConfig& config);
    void setTpoTimeframeMs(int timeframeMs);
    void setTpoSessionType(int sessionType);
    void setTpoLayout(const QString& layout);
    void setTpoTheme(const QString& theme);
    void applyClientConfig(const ClientConfig& config, ChartViewController& view, AxisTextService* axes);
    void applyServerConfig(const ServerConfig& config, ChartViewController& view);
private:
    void changed(Notice notice) { if (notify) notify(notice); }
    void update() { if (requestUpdate) requestUpdate(); }
    ChartLayers& layers_;
    double gamma_ = 1.05;
    double contrast_ = 1.15;
    double shaderFloor_ = 0.01;
    int candleStyle_ = 0;
    QColor candleUpColor_{"#2EBD85"}, candleDownColor_{"#F6465D"};
    QString candleWickColor_ = "auto";
    double candleBodyOpacity_ = 1;
    int candleWickWidth_ = 1;
    int tpoTimeframeMs_ = 1800000;
    int tpoSessionType_ = 4;
};
} // namespace chart
