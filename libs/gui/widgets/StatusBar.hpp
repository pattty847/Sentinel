#pragma once
#include <QWidget>
#include <QPointer>
#include <QTimer>
#include "../models/MarketHealth.hpp"
class QLabel;

// Primary market context. Engineering metrics belong in telemetry.
class StatusBar : public QWidget {
    Q_OBJECT
public:
    explicit StatusBar(QWidget* parent = nullptr);
    void setMarketHealth(MarketHealth* health);
    void setConnectionStatus(bool connected);
    void setConnectionConnecting();
    // Compatibility with existing hubs; these measurements are intentionally
    // omitted from the compact status bar (including unsupported CPU/GPU zeros).
    void setCpuUsage(int) {}
    void setGpuUsage(int) {}
    void setFrameStats(const QString&) {}
    void setLatency(int) {}
    void setCoinbaseLatency(int) {}
    void setUploadBandwidth(double) {}
    void setReadyStatus(const QString& status = "Ready");
    void showVersion();
protected:
    void showEvent(QShowEvent*) override;
    void hideEvent(QHideEvent*) override;
private:
    void refresh();
    QPointer<MarketHealth> m_health;
    QLabel *m_symbolLabel, *m_stateLabel, *m_ageLabel, *m_versionLabel;
    QTimer m_ageTimer;
};
