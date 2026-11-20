#pragma once
#include <QObject>
#include <QString>
#include <memory>
#include "chart/core/TpoStyle.hpp"
struct ClientTpoConfig;
namespace chart {
class ChartLookSettings;
class TpoStyleStore;
class ChartTpoQml : public QObject {
    Q_OBJECT
    Q_PROPERTY(int tpoTimeframeMs READ tpoTimeframeMs WRITE setTpoTimeframeMs NOTIFY tpoConfigChanged)
    Q_PROPERTY(int tpoSessionType READ tpoSessionType WRITE setTpoSessionType NOTIFY tpoConfigChanged)
    // Advances when a drawn frame changes what hoverText() returns for a still pointer.
    Q_PROPERTY(int hoverRevision READ hoverRevision NOTIFY hoverChanged)
public:
    explicit ChartTpoQml(ChartLookSettings& look);
    int tpoTimeframeMs() const;
    int tpoSessionType() const;
    Q_INVOKABLE void setTpoTimeframeMs(int timeframeMs);
    Q_INVOKABLE void setTpoSessionType(int sessionType);
    Q_INVOKABLE void setTpoLayout(const QString& layout);
    Q_INVOKABLE void setTpoTheme(const QString& theme);
    QString tpoLayout() const;
    QString tpoTheme() const;
    Q_INVOKABLE void setTpoPreset(const QString& preset);
    // Text for the TPO cell under an item point; empty unless its letters are approximate.
    Q_INVOKABLE QString hoverText(double x, double y) const;
    int hoverRevision() const { return hoverRevision_; }
    void notifyHoverChanged() { ++hoverRevision_; emit hoverChanged(); }
    QString tpoPreset() const;
    tpo::StyleModel tpoLook() const;
    void setTpoLook(const tpo::StyleModel& look);
    void resetTpoLook();
    void setStyleStore(std::shared_ptr<TpoStyleStore> store);
    int tpoSessions() const;
    void applyTpoConfig(const ClientTpoConfig& tpo);
signals:
    void tpoConfigChanged();
    void tpoStyleChanged();
    void hoverChanged();
private:
    ChartLookSettings& look_;
    int hoverRevision_ = 0;
};
} // namespace chart
