// Chart render-thread timing, summarized on the GUI thread for display/API reads.
#pragma once

#include "FrameStatsWindow.hpp"
#include <QObject>
#include <QPointer>
#include <QQuickWindow>
#include <atomic>
#include <mutex>

class PerformanceMonitor : public QObject {
    Q_OBJECT

public:
    static PerformanceMonitor& instance();

    void attachToWindow(QQuickWindow* window);

    struct FrameStats {
        FrameStatsWindow::Summary window;
        bool idle = true; // no chart input in the past second
    };
    FrameStats frameStats() const { return m_latestFrameStats; } // GUI thread only
    QString frameStatsText() const;
    int getCpuUsage() const { return m_cpuPercent.load(); }
    int getGpuUsage() const { return m_gpuPercent.load(); }
    int getLatency() const { return m_latencyMs.load(); }
    double getUploadBandwidthMBps() const { return m_uploadBandwidthMBps.load(); }

    // Manual updates (called from other systems)
    void updateCpuUsage(int percent);
    void updateGpuUsage(int percent);
    void updateLatency(int milliseconds);

    // Render-thread safe: accumulate bytes uploaded to GPU this frame
    void addUploadBytes(qint64 n) { m_uploadBytesAccum.fetch_add(n, std::memory_order_relaxed); }

signals:
    void frameStatsChanged(const QString& text);
    void cpuUsageChanged(int percent);
    void gpuUsageChanged(int percent);
    void latencyChanged(int milliseconds);
    void uploadBandwidthChanged(double mbPerSec);

private slots:
    void onRenderStart();
    void onRenderEnd();
    void refreshFrameStats();
    void updateCpuMetrics();

private:
    PerformanceMonitor();
    ~PerformanceMonitor() override;
    PerformanceMonitor(const PerformanceMonitor&) = delete;
    PerformanceMonitor& operator=(const PerformanceMonitor&) = delete;

    void initWindowsCounters();
    void cleanupWindowsCounters();

    bool eventFilter(QObject* watched, QEvent* event) override;
    static int64_t monotonicNs();
    std::mutex m_frameMutex;
    FrameStatsWindow m_frameWindow;
    std::atomic<int64_t> m_renderStartNs{0};
    int64_t m_lastInputMs = 0; // GUI thread only
    int64_t m_lastBandwidthMs = 0;
    FrameStats m_latestFrameStats; // GUI thread only
    QPointer<QQuickWindow> m_window;
    QTimer* m_frameUpdateTimer = nullptr;

    std::atomic<int> m_cpuPercent{0};
    std::atomic<int> m_gpuPercent{0};
    std::atomic<int> m_latencyMs{0};
    std::atomic<qint64> m_uploadBytesAccum{0};
    std::atomic<double> m_uploadBandwidthMBps{0.0};
    QTimer* m_cpuUpdateTimer = nullptr;

#ifdef _WIN32
    void* m_pdhQuery = nullptr;
    void* m_cpuCounter = nullptr;
    void* m_gpuCounter = nullptr;
#endif
};
