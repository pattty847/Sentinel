#pragma once
#include "chart/core/CvdSeries.hpp"
#include <cmath>
#include <functional>
#include <QObject>
#include <QJsonObject>
#include <string_view>

namespace chart {
class LayerVisibility : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool flowFooter READ flowFooter WRITE writeFlowFooter NOTIFY layerVisibilityChanged)
    Q_PROPERTY(int cvdStripPx READ cvdStripPx WRITE writeCvdStripPx NOTIFY layerVisibilityChanged)
    Q_PROPERTY(QString cvdReset READ cvdReset WRITE writeCvdReset NOTIFY layerVisibilityChanged)
    Q_PROPERTY(QString cvdStyle READ cvdStyle WRITE writeCvdStyle NOTIFY layerVisibilityChanged)
    Q_PROPERTY(QString cvdFillAnchor READ cvdFillAnchor WRITE writeCvdFillAnchor NOTIFY layerVisibilityChanged)
    Q_PROPERTY(int primaryField READ primaryField NOTIFY primaryFieldChanged)
    Q_PROPERTY(bool heatmapLayerEnabled READ heatmapEnabled NOTIFY layerVisibilityChanged)
    Q_PROPERTY(bool footprintLayerEnabled READ footprintEnabled NOTIFY layerVisibilityChanged)
    Q_PROPERTY(QString footprintIntensity READ footprintIntensity WRITE writeFootprintIntensity NOTIFY layerVisibilityChanged)
    Q_PROPERTY(QString footprintCandle READ footprintCandle WRITE writeFootprintCandle NOTIFY layerVisibilityChanged)
    Q_PROPERTY(double footprintDustNotional READ footprintDustNotional WRITE writeFootprintDustNotional NOTIFY layerVisibilityChanged)
    Q_PROPERTY(bool footprintNumbers READ footprintNumbers WRITE writeFootprintNumbers NOTIFY layerVisibilityChanged)
    Q_PROPERTY(bool tpoLayerEnabled READ tpoEnabled NOTIFY layerVisibilityChanged)
    Q_PROPERTY(bool volumeProfileLayerEnabled READ volumeProfileEnabled NOTIFY layerVisibilityChanged)
    // The book ladder strip (independent of the primary field; off by default).
    Q_PROPERTY(bool ladderLayerEnabled READ ladderEnabled NOTIFY layerVisibilityChanged)
    Q_PROPERTY(int ladderWidthPx READ ladderWidthPx NOTIFY layerVisibilityChanged)
    // Wall radar on the ladder (works only while the ladder is on; off by default) and its
    // threshold: a multiple of that side's median row size.
    Q_PROPERTY(bool wallRadarEnabled READ wallRadarEnabled NOTIFY layerVisibilityChanged)
    Q_PROPERTY(double wallMultiple READ wallMultiple NOTIFY layerVisibilityChanged)
public:
    static constexpr int kLadderWidthPx = 120, kLadderMinWidthPx = 60, kLadderMaxWidthPx = 240;
    static constexpr double kWallMultiple = 4, kWallMinMultiple = 1.5, kWallMaxMultiple = 20;
    std::function<void(std::string_view, bool, bool)> report;
    std::function<void()> requestUpdate;

    bool flowFooter() const { return flowFooter_; }
    void setFlowFooter(bool enabled) {
        if (flowFooter_ == enabled) return;
        flowFooter_ = enabled;
        changed("flowFooter", enabled);
        emit cvdHistoryChanged();
    }
    void writeFlowFooter(bool enabled) {
        setFlowFooter(enabled);
        emit footprintSettingsRequested({{"flowFooter", enabled}});
    }
    int cvdStripPx() const { return cvdStrip_; }
    CvdReset cvdResetRule() const { return cvdReset_; }
    QString cvdReset() const {
        if (cvdReset_ == CvdReset::Visible) return QStringLiteral("visible");
        return cvdReset_ == CvdReset::UtcDay ? QStringLiteral("utcDay") : QStringLiteral("tpoSession");
    }
    QString cvdStyle() const { return cvdArea_ ? QStringLiteral("area") : QStringLiteral("line"); }
    QString cvdFillAnchor() const { return cvdFillAnchor_; }
    void setCvdStripPx(int px) {
        px = std::clamp(px, 0, 400);
        if (cvdStrip_ == px) return;
        const bool gateChanged = (cvdStrip_ == 0) != (px == 0);
        cvdStrip_ = px; changed("cvdStripPx", px > 0);
        if (gateChanged) emit cvdHistoryChanged();
    }
    void setCvdReset(const QString& value) {
        if (value != "visible" && value != "utcDay" && value != "tpoSession") return;
        const auto reset = value == "visible" ? CvdReset::Visible : value == "utcDay" ? CvdReset::UtcDay : CvdReset::TpoSession;
        if (cvdReset_ == reset) return;
        cvdReset_ = reset; changed("cvdReset", true);
        if (cvdStrip_ > 0 || flowFooter_) emit cvdHistoryChanged();
    }
    void refreshCvdSessionHistory() {
        if ((cvdStrip_ > 0 || flowFooter_) && cvdReset_ == CvdReset::TpoSession) emit cvdHistoryChanged();
    }
    void setCvdStyle(const QString& value) {
        if (value != "line" && value != "area") return;
        setFootprintOption(cvdArea_, value == "area", "cvdStyle");
    }
    void setCvdFillAnchor(const QString& value) {
        if (value != "auto" && value != "zero" && value != "edge" && value != "none") return;
        if (cvdFillAnchor_ == value) return;
        cvdFillAnchor_ = value; changed("cvdFillAnchor", true);
    }
    void writeCvdStripPx(int px) {
        setCvdStripPx(px);
        emit footprintSettingsRequested({{"cvdStripPx", cvdStrip_}});
    }
    void writeCvdReset(const QString& value) {
        setCvdReset(value);
        if (cvdReset() == value) emit footprintSettingsRequested({{"cvdReset", value}});
    }
    void writeCvdStyle(const QString& value) {
        setCvdStyle(value);
        if (cvdStyle() == value) emit footprintSettingsRequested({{"cvdStyle", value}});
    }
    void writeCvdFillAnchor(const QString& value) {
        setCvdFillAnchor(value);
        if (cvdFillAnchor_ == value) emit footprintSettingsRequested({{"cvdFillAnchor", value}});
    }
    int primaryField() const { return primaryField_; }
    bool heatmapEnabled() const { return heatmap_; }
    bool footprintEnabled() const { return footprint_; }
    QString footprintIntensity() const { return footprintWindowIntensity_ ? QStringLiteral("window") : QStringLiteral("bar"); }
    QString footprintCandle() const { return footprintSpine_ ? QStringLiteral("spine") : QStringLiteral("full"); }
    double footprintDustNotional() const { return footprintDust_; }
    bool footprintNumbers() const { return footprintNumbers_; }
    void setFootprintIntensity(const QString& value) {
        if (value != "bar" && value != "window") return;
        const bool window = value == "window";
        if (window == footprintWindowIntensity_) return;
        footprintWindowIntensity_ = window;
        changed("footprintIntensity", window);
    }
    void setFootprintCandle(const QString& value) {
        if (value != "spine" && value != "full") return;
        setFootprintOption(footprintSpine_, value == "spine", "footprintCandle");
    }
    void setFootprintDustNotional(double value) {
        if (!std::isfinite(value) || value < 0 || value == footprintDust_) return;
        footprintDust_ = value; changed("footprintDustNotional", value > 0);
    }
    void writeFootprintDustNotional(double value) {
        setFootprintDustNotional(value);
        if (value == footprintDust_) emit footprintSettingsRequested({{"footprintDustNotional", value}});
    }
    void setFootprintNumbers(bool value) { setFootprintOption(footprintNumbers_, value, "footprintNumbers"); }
    void writeFootprintIntensity(const QString& value) {
        setFootprintIntensity(value);
        if (value == footprintIntensity()) emit footprintSettingsRequested({{"footprintIntensity", value}});
    }
    void writeFootprintCandle(const QString& value) {
        setFootprintCandle(value);
        if (value == footprintCandle()) emit footprintSettingsRequested({{"footprintCandle", value}});
    }
    void writeFootprintNumbers(bool value) {
        setFootprintNumbers(value);
        emit footprintSettingsRequested({{"footprintNumbers", value}});
    }
    bool tpoEnabled() const { return tpo_; }
    bool volumeProfileEnabled() const { return volumeProfile_; }
    bool ladderEnabled() const { return ladder_; }
    int ladderWidthPx() const { return ladder_ ? ladderWidth_ : 0; }
    // The strip width while the ladder is on, clamped to [kLadderMinWidthPx, kLadderMaxWidthPx].
    int ladderWidthSetting() const { return ladderWidth_; }
    bool wallRadarEnabled() const { return wallRadar_; }
    double wallMultiple() const { return wallMultiple_; }
    // Clamped to [kWallMinMultiple, kWallMaxMultiple]; a non-finite value is ignored.
    void setWallMultiple(double multiple) {
        if (!std::isfinite(multiple)) return;
        multiple = multiple < kWallMinMultiple ? kWallMinMultiple : multiple > kWallMaxMultiple ? kWallMaxMultiple : multiple;
        if (multiple == wallMultiple_) return;
        wallMultiple_ = multiple;
        if (requestUpdate) requestUpdate();
        emit layerVisibilityChanged();
    }
    void setLadderWidthPx(int px) {
        px = px < kLadderMinWidthPx ? kLadderMinWidthPx : px > kLadderMaxWidthPx ? kLadderMaxWidthPx : px;
        if (px == ladderWidth_) return;
        ladderWidth_ = px;
        if (!ladder_) return;
        if (requestUpdate) requestUpdate();
        emit layerVisibilityChanged();
    }

    void setPrimaryField(int field) {
        if (primaryField_ == field) return;
        primaryField_ = field;
        if (field == 0) {
            heatmap_ = true;
            tpo_ = false;
            volumeProfile_ = false;
        } else if (field == 1) {
            footprint_ = true;
            tpo_ = false;
            volumeProfile_ = false;
        } else if (field == 2) {
            tpo_ = true;
            heatmap_ = false;
            footprint_ = false;
            volumeProfile_ = false;
        } else if (field == 3) {
            volumeProfile_ = true;
            tpo_ = false;
            heatmap_ = false;
            footprint_ = false;
        }
        if (report) report("primaryField", false, false);
        if (requestUpdate) requestUpdate();
        emit primaryFieldChanged();
        emit layerVisibilityChanged();
    }

    // Independent layer patches preserve every other layer; primary-field UI
    // selection continues to use the exclusive setters below.
    void setLayerEnabled(std::string_view name, bool enabled) {
        bool* layer = nullptr;
        if (name == "heatmap") layer = &heatmap_;
        else if (name == "footprint") layer = &footprint_;
        else if (name == "tpo") layer = &tpo_;
        else if (name == "volumeProfile") layer = &volumeProfile_;
        else if (name == "ladder") layer = &ladder_;
        else if (name == "wallRadar") layer = &wallRadar_;
        if (!layer || *layer == enabled) return;
        *layer = enabled;
        changed(name, enabled);
    }

    Q_INVOKABLE void setLadderEnabled(bool enabled) { setLayerEnabled("ladder", enabled); }
    Q_INVOKABLE void setWallRadarEnabled(bool enabled) { setLayerEnabled("wallRadar", enabled); }

    Q_INVOKABLE void setVolumeProfileEnabled(bool enabled) {
        const bool newHeatmapEnabled = enabled ? false : heatmap_;
        const bool newFootprintEnabled = enabled ? false : footprint_;
        const bool newTpoEnabled = enabled ? false : tpo_;
        if (volumeProfile_ == enabled && heatmap_ == newHeatmapEnabled &&
            footprint_ == newFootprintEnabled && tpo_ == newTpoEnabled) return;
        volumeProfile_ = enabled;
        if (enabled) {
            heatmap_ = false;
            footprint_ = false;
            tpo_ = false;
        }
        changed("volumeProfile", enabled);
    }

    Q_INVOKABLE void setHeatmapEnabled(bool enabled) {
        const bool newTpoEnabled = enabled ? false : tpo_;
        const bool newVpEnabled = enabled ? false : volumeProfile_;
        if (heatmap_ == enabled && tpo_ == newTpoEnabled && volumeProfile_ == newVpEnabled) {
            if (report) report("heatmap", enabled, true);
            return;
        }
        heatmap_ = enabled;
        if (enabled) {
            tpo_ = false;
            volumeProfile_ = false;
        }
        changed("heatmap", enabled);
    }

    Q_INVOKABLE void setFootprintEnabled(bool enabled) {
        const bool newTpoEnabled = enabled ? false : tpo_;
        const bool newVpEnabled = enabled ? false : volumeProfile_;
        if (footprint_ == enabled && tpo_ == newTpoEnabled && volumeProfile_ == newVpEnabled) {
            if (report) report("footprint", enabled, true);
            return;
        }
        footprint_ = enabled;
        if (enabled) {
            tpo_ = false;
            volumeProfile_ = false;
        }
        changed("footprint", enabled);
    }

    Q_INVOKABLE void setTpoEnabled(bool enabled) {
        const bool newHeatmapEnabled = enabled ? false : heatmap_;
        const bool newFootprintEnabled = enabled ? false : footprint_;
        const bool newVolumeProfileEnabled = enabled ? false : volumeProfile_;
        if (tpo_ == enabled && heatmap_ == newHeatmapEnabled &&
            footprint_ == newFootprintEnabled && volumeProfile_ == newVolumeProfileEnabled) {
            if (report) report("tpo", enabled, true);
            return;
        }
        tpo_ = enabled;
        if (enabled) {
            heatmap_ = false;
            footprint_ = false;
            volumeProfile_ = false;
        }
        changed("tpo", enabled);
    }
signals:
    void cvdHistoryChanged();
    void footprintSettingsRequested(const QJsonObject& patch);
    void primaryFieldChanged();
    void layerVisibilityChanged();
private:
    void setFootprintOption(bool& option, bool value, std::string_view name) {
        if (option == value) return;
        option = value;
        changed(name, value);
    }
    bool flowFooter_ = false;
    int cvdStrip_ = 0;
    CvdReset cvdReset_ = CvdReset::Visible;
    QString cvdFillAnchor_ = QStringLiteral("auto");
    bool cvdArea_ = false;
    double footprintDust_ = 2500;
    bool footprintWindowIntensity_ = false, footprintSpine_ = true, footprintNumbers_ = true;
    void changed(std::string_view field, bool enabled) {
        if (report) report(field, enabled, false);
        if (requestUpdate) requestUpdate();
        emit layerVisibilityChanged();
    }
    int primaryField_ = 0;
    bool heatmap_ = true;
    bool footprint_ = false;
    bool tpo_ = false;
    bool volumeProfile_ = false;
    bool ladder_ = false;
    int ladderWidth_ = kLadderWidthPx;
    bool wallRadar_ = false;
    double wallMultiple_ = kWallMultiple;
};
} // namespace chart
