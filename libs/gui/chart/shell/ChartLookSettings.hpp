#pragma once
#include "config/ConfigTypes.hpp"
#include <QColor>
#include "themes/ThemePalette.hpp"
#include <QObject>
#include "ChartTpoQml.hpp"
#include "chart/core/TpoStyle.hpp"
#include <memory>
#include <QString>
#include <functional>

namespace heatmap { struct HeatmapChartSettings; }
namespace chart {
struct ChartLayers;
class ChartViewController;
}
class AxisTextService;
namespace chart { class TpoStyleStore; }

namespace chart {
class ChartLookSettings : public QObject {
    Q_OBJECT
    Q_PROPERTY(double heatmapGamma READ gamma WRITE setHeatmapGamma NOTIFY heatmapGammaChanged)
    Q_PROPERTY(double heatmapContrast READ contrast WRITE setHeatmapContrast NOTIFY heatmapContrastChanged)
    Q_PROPERTY(double heatmapShaderFloor READ shaderFloor WRITE setHeatmapShaderFloor NOTIFY heatmapShaderFloorChanged)
    Q_PROPERTY(bool tradesAboveCandles READ tradesAboveCandles NOTIFY tradeBubbleSettingsChanged)
    Q_PROPERTY(int candleStyle READ candleStyle WRITE setCandleStyle NOTIFY candleStyleChanged)
    Q_PROPERTY(QString paperPositionStyle READ paperPositionStyle NOTIFY paperPositionStyleChanged)
    Q_PROPERTY(QColor candleUpColor READ candleUpColor NOTIFY candleAppearanceChanged)
    Q_PROPERTY(QColor candleDownColor READ candleDownColor NOTIFY candleAppearanceChanged)
    Q_PROPERTY(QString candleWickColor READ candleWickColor NOTIFY candleAppearanceChanged)
    Q_PROPERTY(double candleBodyOpacity READ candleBodyOpacity NOTIFY candleAppearanceChanged)
    Q_PROPERTY(int candleWickWidth READ candleWickWidth NOTIFY candleAppearanceChanged)
    Q_PROPERTY(double candleBodyFraction READ candleBodyFraction NOTIFY candleAppearanceChanged)
    Q_PROPERTY(int candleMaxBodyPx READ candleMaxBodyPx NOTIFY candleAppearanceChanged)
    Q_PROPERTY(bool showVolumeBars READ showVolumeBars NOTIFY candleAppearanceChanged)
    Q_PROPERTY(double volumeBandPercent READ volumeBandPercent NOTIFY candleAppearanceChanged)
    Q_PROPERTY(double volumeOpacity READ volumeOpacity NOTIFY candleAppearanceChanged)
public:
    static QColor chartBackground() { return theme::chartBackground(); }
    static QColor footprintSellAggr() { return theme::negative(); }
    static QColor footprintBuyAggr() { return theme::positive(); }
    static QColor footprintText() { return QColor(240, 244, 248); }
    static QColor footprintInkDark() { return QColor(11, 13, 16); }
    ChartTpoQml tpo{*this};
    std::function<bool()> tradesAboveCandlesSource;
    bool tradesAboveCandles() const { return tradesAboveCandlesSource ? tradesAboveCandlesSource() : true; }
    enum class Notice { TpoConfig, TpoStyle, Gamma, Contrast, ShaderFloor, CandleStyle, CandleAppearance };
    explicit ChartLookSettings(ChartLayers& layers) : layers_(layers) {}
    std::function<void()> requestUpdate, onViewportChanged;
    std::function<void(float, float, float)> applyTone;

    double gamma() const { return gamma_; }
    double contrast() const { return contrast_; }
    double shaderFloor() const { return shaderFloor_; }
    int candleStyle() const { return candleStyle_; }
    QString paperPositionStyle() const { return paperPositionStyle_; }
    QColor candleUpColor() const { return candleUpColor_; }
    QColor candleDownColor() const { return candleDownColor_; }
    QString candleWickColor() const { return candleWickColor_; }
    double candleBodyOpacity() const { return candleBodyOpacity_; }
    int candleWickWidth() const { return candleWickWidth_; }
    double candleBodyFraction() const { return candleBodyFraction_; }
    int candleMaxBodyPx() const { return candleMaxBodyPx_; }
    bool showVolumeBars() const { return showVolumeBars_; }
    double volumeBandPercent() const { return volumeBandPercent_; }
    double volumeOpacity() const { return volumeOpacity_; }
    int tpoTimeframeMs() const { return tpoTimeframeMs_; }
    int tpoSessionType() const { return tpoSessionType_; }
    int tpoSessions() const;
    QString tpoLayout() const;
    QString tpoTheme() const;
    QString tpoPreset() const;
    tpo::StyleModel tpoLook() const;

    void setInitialTone(const ClientConfig& config);
    void syncGpuTone();
    void setHeatmapGamma(double gamma);
    void setHeatmapContrast(double contrast);
    void setHeatmapShaderFloor(double floor);
    void setCandleStyle(int style);
    void setCandleAppearance(const heatmap::HeatmapChartSettings& settings);
    void setPaperPositionStyle(const heatmap::HeatmapChartSettings& settings);
    void applyTpoConfig(const ClientTpoConfig& config);
    void setTpoTimeframeMs(int timeframeMs);
    void setTpoSessionType(int sessionType);
    void setTpoLayout(const QString& layout);
    void setTpoTheme(const QString& theme);
    void setTpoPreset(const QString& preset);
    void setTpoLook(const tpo::StyleModel& look);
    // Forgets the saved style and returns to the default preset with the configured theme.
    void resetTpoLook();
    void setTpoStyleStore(std::shared_ptr<TpoStyleStore> store);
    void applyClientConfig(const ClientConfig& config, ChartViewController& view, AxisTextService* axes);
    void applyServerConfig(const ServerConfig& config, ChartViewController& view);
signals:
    void candleStyleChanged();
    void candleAppearanceChanged();
    void heatmapGammaChanged();
    void heatmapContrastChanged();
    void heatmapShaderFloorChanged();
    void tradeBubbleSettingsChanged();
    void paperPositionStyleChanged();
private:
    void changed(Notice notice);
    bool applyTpoLook(const tpo::StyleModel& look);
    void update() { if (requestUpdate) requestUpdate(); }
    ChartLayers& layers_;
    double gamma_ = 1.05;
    double contrast_ = 1.15;
    double shaderFloor_ = 0.01;
    int candleStyle_ = 0;
    QString paperPositionStyle_ = QStringLiteral("tag");
    QColor candleUpColor_{"#2EBD85"}, candleDownColor_{"#F6465D"};
    QString candleWickColor_ = "auto";
    double candleBodyOpacity_ = 1;
    int candleWickWidth_ = 1;
    double candleBodyFraction_ = 0.85;
    int candleMaxBodyPx_ = 71;
    bool showVolumeBars_ = true;
    double volumeBandPercent_ = 22, volumeOpacity_ = 0.45;
    int tpoTimeframeMs_ = 1800000;
    int tpoSessionType_ = 4;
    tpo::Theme tpoConfigTheme_ = tpo::Theme::Rainbow;
    std::shared_ptr<TpoStyleStore> tpoStore_;
};
} // namespace chart
