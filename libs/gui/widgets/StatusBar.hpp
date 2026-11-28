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
    void setMessage(const QString& message);
    void setConnectionStatus(bool connected);
    void setConnectionConnecting();
    void showVersion();
protected:
    void showEvent(QShowEvent*) override;
    void hideEvent(QHideEvent*) override;
private:
    void refresh();
    void refreshMessage();
    QPointer<MarketHealth> m_health;
    QString m_message;
    QLabel *m_symbolLabel, *m_stateLabel, *m_ageLabel, *m_messageLabel, *m_versionLabel;
    QTimer m_ageTimer;
};
