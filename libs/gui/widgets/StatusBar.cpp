#include "StatusBar.hpp"
#include "Version.hpp"
#include <QHBoxLayout>
#include <QLabel>
#include <QShowEvent>
#include <QHideEvent>

StatusBar::StatusBar(QWidget* parent) : QWidget(parent) {
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(8, 2, 8, 2);
    layout->setSpacing(8);
    m_symbolLabel = new QLabel("No symbol", this);
    m_symbolLabel->setObjectName("marketSymbol");
    m_stateLabel = new QLabel("Initializing", this);
    m_stateLabel->setObjectName("marketState");
    m_ageLabel = new QLabel("Book age unknown", this);
    m_ageLabel->setObjectName("marketBookAge");
    m_ageLabel->setToolTip("Time since the last accepted book message. A quiet market is not necessarily stale.");
    m_versionLabel = new QLabel(QString::fromStdString(Sentinel::getVersionString()), this);
    m_versionLabel->hide();
    setStyleSheet("StatusBar QLabel { color: #a0a0a0; font-size: 11px; } QLabel#marketSymbol { color: #ddd; font-weight: bold; }");
    layout->addWidget(m_symbolLabel);
    layout->addWidget(m_stateLabel);
    layout->addStretch();
    layout->addWidget(m_ageLabel);
    layout->addWidget(m_versionLabel);
    m_ageTimer.setInterval(1000);
    connect(&m_ageTimer, &QTimer::timeout, this, &StatusBar::refresh);
}
void StatusBar::setMarketHealth(MarketHealth* health) {
    if (m_health) disconnect(m_health, nullptr, this, nullptr);
    m_health = health;
    if (health) {
        connect(health, &MarketHealth::changed, this, [this] { if (isVisible()) refresh(); });
        connect(health, &QObject::destroyed, this, [this] { m_ageTimer.stop(); if (isVisible()) refresh(); });
    }
    if (isVisible() && health) m_ageTimer.start();
    else m_ageTimer.stop();
    if (isVisible()) refresh();
}
void StatusBar::refresh() {
    if (!isVisible() || window()->isMinimized()) return;
    if (!m_health) {
        m_symbolLabel->setText("No symbol");
        m_stateLabel->setText("Unavailable");
        m_ageLabel->setText("Book age unknown");
        return;
    }
    m_health->refreshChartState();
    const auto s = m_health->snapshot();
    m_symbolLabel->setText(s.symbol.isEmpty() ? "No symbol" : s.symbol);
    m_stateLabel->setText(s.text());
    m_stateLabel->setToolTip(s.reason);
    const bool degraded = s.state == MarketHealth::State::Stale || s.state == MarketHealth::State::HistoryPartial || s.state == MarketHealth::State::Unavailable;
    const bool disconnected = s.state == MarketHealth::State::Disconnected || s.state == MarketHealth::State::Reconnecting;
    m_stateLabel->setStyleSheet(QString("color: %1;").arg(disconnected ? "#e57373" : degraded ? "#ffcc80" : "#b0b0b0"));
    m_ageLabel->setText(s.bookAgeMs ? "Book " + MarketHealth::ageText(s.bookAgeMs) : "Book age unknown");
}
void StatusBar::showEvent(QShowEvent* event) { QWidget::showEvent(event); refresh(); if (m_health) m_ageTimer.start(); }
void StatusBar::hideEvent(QHideEvent* event) { m_ageTimer.stop(); QWidget::hideEvent(event); }
void StatusBar::setConnectionStatus(bool connected) { if (!m_health) m_stateLabel->setText(connected ? "Waiting for subscription" : "Disconnected"); }
void StatusBar::setConnectionConnecting() { if (!m_health) m_stateLabel->setText("Initializing"); }
void StatusBar::setReadyStatus(const QString&) { /* A generic Ready string cannot prove market health. */ }
void StatusBar::showVersion() { m_versionLabel->setVisible(!m_versionLabel->isVisible()); }
