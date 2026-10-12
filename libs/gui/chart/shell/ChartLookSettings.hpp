#pragma once
#include "config/ConfigTypes.hpp"
#include <QColor>
#include <QObject>
#include "ChartTpoQml.hpp"
#include <QString>
#include <functional>

namespace heatmap { struct HeatmapChartSettings; }
namespace chart {
struct ChartLayers;
class ChartViewController;
}
class AxisTextService;

namespace chart {
class ChartLookSettings : public QObject {
    Q_OBJECT
    Q_PROPERTY(double heatmapGamma READ gamma WRITE setHeatmapGamma NOTIFY heatmapGammaChanged)
    Q_PROPERTY(double heatmapContrast READ contrast WRITE setHeatmapContrast NOTIFY heatmapContrastChanged)
    Q_PROPERTY(double heatmapShaderFloor READ shaderFloor WRITE setHeatmapShaderFloor NOTIFY heatmapShaderFloorChanged)
    Q_PROPERTY(bool tradesAboveCandles READ tradesAboveCandles NOTIFY tradeBubbleSettingsChanged)
    Q_PROPERTY(int candleStyle READ candleStyle WRITE setCandleStyle NOTIFY candleStyleChanged)
    Q_PROPERTY(QColor candleUpColor READ candleUpColor NOTIFY candleAppearanceChanged)
    Q_PROPERTY(QColor candleDownColor READ candleDownColor NOTIFY candleAppearanceChanged)
    Q_PROPERTY(QString candleWickColor READ candleWickColor NOTIFY candleAppearanceChanged)
    Q_PROPERTY(double candleBodyOpacity READ candleBodyOpacity NOTIFY candleAppearanceChanged)
    Q_PROPERTY(int candleWickWidth READ candleWickWidth NOTIFY candleAppearanceChanged)
public:
    ChartTpoQml tpo{*this};
    std::function<bool()> tradesAboveCandlesSource;
    bool tradesAboveCandles() const { return tradesAboveCandlesSource ? tradesAboveCandlesSource() : true; }
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
signals:
    void candleStyleChanged();
    void candleAppearanceChanged();
    void heatmapGammaChanged();
    void heatmapContrastChanged();
    void heatmapShaderFloorChanged();
    void tradeBubbleSettingsChanged();
private:
    void changed(Notice notice);
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
