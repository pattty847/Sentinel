#include "StatusBar.hpp"
#include "Version.hpp"
#include <QHBoxLayout>
#include <QTimer>
#include <QApplication>
#include <QStyle>

StatusBar::StatusBar(QWidget* parent)
    : QWidget(parent)
{
    // Don't set fixed height here - let QStatusBar handle sizing
    // setFixedHeight(24);  // Compact, dock-like height
    setStyleSheet(
        "StatusBar { "
        "  background-color: transparent; "  // Let parent QStatusBar handle background
        "  border: none; "
        "}"
    );
    
    QHBoxLayout* layout = new QHBoxLayout(this);
    layout->setContentsMargins(8, 2, 8, 2);
    layout->setSpacing(12);
    
    // Left: Ready status
    m_readyLabel = new QLabel("Ready", this);
    m_readyLabel->setStyleSheet("QLabel { color: #888; font-size: 10px; }");
    layout->addWidget(m_readyLabel);
    
    layout->addStretch();
    
    // Right: Connection status
    m_connectionLabel = new QLabel("Disconnected", this);
    m_connectionLabel->setStyleSheet("QLabel { color: #ff4444; font-size: 10px; }");
    layout->addWidget(m_connectionLabel);

    // FPS
    m_fpsLabel = new QLabel("FPS: --", this);
    m_fpsLabel->setStyleSheet("QLabel { color: #888; font-size: 10px; }");
    layout->addWidget(m_fpsLabel);

    // CPU usage
    m_cpuLabel = new QLabel("CPU: --%", this);
    m_cpuLabel->setStyleSheet("QLabel { color: #888; font-size: 10px; }");
    layout->addWidget(m_cpuLabel);
    
    // GPU usage
    m_gpuLabel = new QLabel("GPU: --%", this);
    m_gpuLabel->setStyleSheet("QLabel { color: #888; font-size: 10px; }");
    layout->addWidget(m_gpuLabel);
    
    // Latency
    m_latencyLabel = new QLabel("Latency: -- ms", this);
    m_latencyLabel->setStyleSheet("QLabel { color: #888; font-size: 10px; }");
    layout->addWidget(m_latencyLabel);
    
    // Version (initially hidden, shown on demand)
    m_versionLabel = new QLabel(QString::fromStdString(Sentinel::getVersionString()), this);
    m_versionLabel->setStyleSheet("QLabel { color: #666; font-size: 9px; }");
    m_versionLabel->hide();
    layout->addWidget(m_versionLabel);
    
    setLayout(layout);
    
    // Set up periodic updates (placeholder - will be connected to real metrics later)
    m_updateTimer = new QTimer(this);
    connect(m_updateTimer, &QTimer::timeout, this, &StatusBar::updateMetrics);
    m_updateTimer->start(1000);  // Update every second
}

void StatusBar::setConnectionStatus(bool connected) {
    m_connected = connected;
    if (connected) {
        m_connectionLabel->setText("🟢 Connected");
        m_connectionLabel->setStyleSheet("QLabel { color: #44ff44; font-size: 10px; }");
    } else {
        m_connectionLabel->setText("🔴 Disconnected");
        m_connectionLabel->setStyleSheet("QLabel { color: #ff4444; font-size: 10px; }");
    }
}

void StatusBar::setCpuUsage(int percent) {
    m_cpuPercent = percent;
    m_cpuLabel->setText(QString("CPU: %1%").arg(percent));
    
    // Color coding: green < 50%, yellow < 80%, red >= 80%
    QString color = percent < 50 ? "#44ff44" : (percent < 80 ? "#ffaa00" : "#ff4444");
    m_cpuLabel->setStyleSheet(QString("QLabel { color: %1; font-size: 10px; }").arg(color));
}

void StatusBar::setGpuUsage(int percent) {
    m_gpuPercent = percent;
    m_gpuLabel->setText(QString("GPU: %1%").arg(percent));

    // Color coding: green < 50%, yellow < 80%, red >= 80%
    QString color = percent < 50 ? "#44ff44" : (percent < 80 ? "#ffaa00" : "#ff4444");
    m_gpuLabel->setStyleSheet(QString("QLabel { color: %1; font-size: 10px; }").arg(color));
}

void StatusBar::setFps(double fps) {
    m_fps = fps;
    m_fpsLabel->setText(QString("FPS: %1").arg(static_cast<int>(fps)));

    // Color coding: green >= 55, yellow >= 30, red < 30
    QString color = fps >= 55 ? "#44ff44" : (fps >= 30 ? "#ffaa00" : "#ff4444");
    m_fpsLabel->setStyleSheet(QString("QLabel { color: %1; font-size: 10px; }").arg(color));
}

void StatusBar::setLatency(int milliseconds) {
    m_latencyMs = milliseconds;
    m_latencyLabel->setText(QString("Latency: %1 ms").arg(milliseconds));
    
    // Color coding: green < 50ms, yellow < 100ms, red >= 100ms
    QString color = milliseconds < 50 ? "#44ff44" : (milliseconds < 100 ? "#ffaa00" : "#ff4444");
    m_latencyLabel->setStyleSheet(QString("QLabel { color: %1; font-size: 10px; }").arg(color));
}

void StatusBar::setReadyStatus(const QString& status) {
    m_readyLabel->setText(status);
}

void StatusBar::updateMetrics() {
    // Metrics are now updated directly via PerformanceMonitor signals
    // This periodic timer is kept for potential future use
}

void StatusBar::showVersion() {
    m_versionLabel->setVisible(!m_versionLabel->isVisible());
}

