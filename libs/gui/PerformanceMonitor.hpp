/*
Sentinel — PerformanceMonitor
Role: Tracks app-wide performance metrics (FPS, CPU, GPU, frame times)
Threading: Main thread only
Usage: Connect to QQuickWindow::frameSwapped for true FPS
*/
#pragma once

#include <QObject>
#include <QElapsedTimer>
#include <QQuickWindow>
#include <atomic>
#include <deque>

class PerformanceMonitor : public QObject {
    Q_OBJECT

public:
    static PerformanceMonitor& instance();

    // Connect to QQuickWindow for frame tracking
    void attachToWindow(QQuickWindow* window);

    // Getters (thread-safe)
    double getCurrentFPS() const { return m_currentFps.load(); }
    double getAverageFrameTime() const { return m_avgFrameTimeMs.load(); }
    int getCpuUsage() const { return m_cpuPercent.load(); }
    int getGpuUsage() const { return m_gpuPercent.load(); }

    // Manual updates (called from other systems)
    void updateCpuUsage(int percent);
    void updateGpuUsage(int percent);

signals:
    void fpsChanged(double fps);
    void cpuUsageChanged(int percent);
    void gpuUsageChanged(int percent);

private slots:
    void onFrameSwapped();
    void updateCpuMetrics();

private:
    PerformanceMonitor();
    ~PerformanceMonitor() override = default;
    PerformanceMonitor(const PerformanceMonitor&) = delete;
    PerformanceMonitor& operator=(const PerformanceMonitor&) = delete;

    // FPS tracking
    QElapsedTimer m_fpsTimer;
    int m_frameCount = 0;
    std::atomic<double> m_currentFps{0.0};
    std::atomic<double> m_avgFrameTimeMs{0.0};

    // Frame time history for smoothing
    std::deque<qint64> m_frameTimesMs;
    static constexpr int MAX_FRAME_SAMPLES = 60;

    // CPU/GPU metrics
    std::atomic<int> m_cpuPercent{0};
    std::atomic<int> m_gpuPercent{0};
    QTimer* m_cpuUpdateTimer = nullptr;
};
