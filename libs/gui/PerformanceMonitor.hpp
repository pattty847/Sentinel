// Main thread performance metrics tracker; connect to QQuickWindow::frameSwapped for FPS.
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

    void attachToWindow(QQuickWindow* window);

    double getCurrentFPS() const { return m_currentFps.load(); }
    double getAverageFrameTime() const { return m_avgFrameTimeMs.load(); }
    int getCpuUsage() const { return m_cpuPercent.load(); }
    int getGpuUsage() const { return m_gpuPercent.load(); }
    int getLatency() const { return m_latencyMs.load(); }

    // Manual updates (called from other systems)
    void updateCpuUsage(int percent);
    void updateGpuUsage(int percent);
    void updateLatency(int milliseconds);

signals:
    void fpsChanged(double fps);
    void cpuUsageChanged(int percent);
    void gpuUsageChanged(int percent);
    void latencyChanged(int milliseconds);

private slots:
    void onFrameSwapped();
    void updateCpuMetrics();

private:
    PerformanceMonitor();
    ~PerformanceMonitor() override;
    PerformanceMonitor(const PerformanceMonitor&) = delete;
    PerformanceMonitor& operator=(const PerformanceMonitor&) = delete;

    void initWindowsCounters();
    void cleanupWindowsCounters();

    QElapsedTimer m_fpsTimer;
    int m_frameCount = 0;
    std::atomic<double> m_currentFps{0.0};
    std::atomic<double> m_avgFrameTimeMs{0.0};

    // Frame time history for smoothing
    std::deque<qint64> m_frameTimesMs;
    static constexpr int MAX_FRAME_SAMPLES = 60;

    std::atomic<int> m_cpuPercent{0};
    std::atomic<int> m_gpuPercent{0};
    std::atomic<int> m_latencyMs{0};
    QTimer* m_cpuUpdateTimer = nullptr;

#ifdef _WIN32
    void* m_pdhQuery = nullptr;
    void* m_cpuCounter = nullptr;
    void* m_gpuCounter = nullptr;
#endif
};
