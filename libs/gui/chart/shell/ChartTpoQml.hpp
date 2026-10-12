#pragma once
#include <QObject>
#include <QString>
struct ClientTpoConfig;
namespace chart {
class ChartLookSettings;
class ChartTpoQml : public QObject {
    Q_OBJECT
    Q_PROPERTY(int tpoTimeframeMs READ tpoTimeframeMs WRITE setTpoTimeframeMs NOTIFY tpoConfigChanged)
    Q_PROPERTY(int tpoSessionType READ tpoSessionType WRITE setTpoSessionType NOTIFY tpoConfigChanged)
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
    int tpoSessions() const;
    void applyTpoConfig(const ClientTpoConfig& tpo);
signals:
    void tpoConfigChanged();
    void tpoStyleChanged();
private:
    ChartLookSettings& look_;
};
} // namespace chart
