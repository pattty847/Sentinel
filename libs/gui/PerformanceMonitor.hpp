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

    // FPS tracking
    QElapsedTimer m_fpsTimer;
    int m_frameCount = 0;
    std::atomic<double> m_currentFps{0.0};
    std::atomic<double> m_avgFrameTimeMs{0.0};

    // Frame time history for smoothing
    std::deque<qint64> m_frameTimesMs;
    static constexpr int MAX_FRAME_SAMPLES = 60;

    // CPU/GPU/Latency metrics
    std::atomic<int> m_cpuPercent{0};
    std::atomic<int> m_gpuPercent{0};
    std::atomic<int> m_latencyMs{0};
    QTimer* m_cpuUpdateTimer = nullptr;

#ifdef _WIN32
    // Windows Performance Counters (PDH API)
    void* m_pdhQuery = nullptr;      // PDH_HQUERY
    void* m_cpuCounter = nullptr;    // PDH_HCOUNTER
    void* m_gpuCounter = nullptr;    // PDH_HCOUNTER
#endif
};
