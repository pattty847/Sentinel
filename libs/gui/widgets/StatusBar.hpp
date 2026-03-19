#pragma once

#include <QWidget>
#include <QLabel>
#include <QHBoxLayout>
#include <QTimer>

/**
 * Bottom status bar (dock-like) for Sentinel terminal.
 * Displays connection status, CPU/GPU/Latency metrics, and "Ready" status.
 * Designed to eventually hold minimized dock icons.
 */
class StatusBar : public QWidget {
    Q_OBJECT

public:
    explicit StatusBar(QWidget* parent = nullptr);
    ~StatusBar() override = default;
    
    void setConnectionStatus(bool connected);
    void setCpuUsage(int percent);
    void setGpuUsage(int percent);
    void setFps(double fps);
    void setLatency(int milliseconds);
    void setCoinbaseLatency(int milliseconds);
    void setUploadBandwidth(double mbPerSec);
    void setReadyStatus(const QString& status = "Ready");
    void showVersion();

private slots:
    void updateMetrics();  // Periodic update slot

private:
    QLabel* m_readyLabel;
    QLabel* m_connectionLabel;
    QLabel* m_fpsLabel;
    QLabel* m_cpuLabel;
    QLabel* m_gpuLabel;
    QLabel* m_latencyLabel;
    QLabel* m_uploadLabel;
    QLabel* m_versionLabel;

    // Metrics storage
    double m_fps = 0.0;
    int m_cpuPercent = 0;
    int m_gpuPercent = 0;
    int m_latencyMs = 0;
    int m_coinbaseLatencyMs = -1;
    bool m_connected = false;
    
    QTimer* m_updateTimer;
};

