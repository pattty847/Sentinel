#pragma once
#include <QObject>
#include <QElapsedTimer>
#include <QTimer>
#include <atomic>
#include <chrono>

class PerformanceMonitor : public QObject
{
    Q_OBJECT

public:
    explicit PerformanceMonitor(QObject* parent = nullptr);
    
    // Frame timing
    void startFrame();
    void endFrame();
    void checkFrameTimes();
    
    // Data throughput tracking
    void recordPointsPushed(size_t count);
    void recordTradesProcessed(size_t count);
    
    // Performance metrics
    double getPointsThroughput() const;
    double getTradesThroughput() const;
    qint64 getWorstFrameTime() const { return m_worstFrameTime.load(); }
    int getFrameDrops() const { return m_frameDrops.load(); }
    bool getAllGatesPassed() const;
    
    // CLI flag --perf dumps metrics every second
    void enableCLIOutput(bool enabled);
    
    // Reset statistics
    void reset();

signals:
    void frameDropDetected(qint64 frameTimeMs);
    void performanceAlert(const QString& message);

private slots:
    void dumpMetrics();

private:
    // Frame timing
    QElapsedTimer m_frameTimer;
    std::atomic<qint64> m_worstFrameTime{0};
    std::atomic<int> m_frameDrops{0};
    bool m_frameTimingActive = false;
    
    // Throughput counters
    std::atomic<size_t> m_pointsPushed{0};
    std::atomic<size_t> m_tradesProcessed{0};
    std::chrono::steady_clock::time_point m_startTime;
    
    // Rolling window for frame times (60 frames = 1 second at 60 FPS)
    static constexpr int FRAME_WINDOW_SIZE = 60;
    std::array<qint64, FRAME_WINDOW_SIZE> m_frameTimes;
    size_t m_frameIndex = 0;
    
    // Performance gate thresholds
    static constexpr qint64 MAX_FRAME_TIME_MS = 16; // 16ms = 60 FPS
    static constexpr qint64 ALERT_FRAME_TIME_MS = 20; // 20ms warning threshold
    
    // CLI output
    bool m_cliOutputEnabled = false;
    QTimer* m_metricsTimer = nullptr;
    
    // Helper methods
    double getElapsedSeconds() const;
    qint64 getAverageFrameTime() const;
}; 