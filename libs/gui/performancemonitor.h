#pragma once

#include <chrono>
#include <vector>
#include <memory>
#include <QObject>
#include <QTimer>
#include <QElapsedTimer>
#include <QLabel>
#include <QVBoxLayout>
#include <QWidget>

/**
 * @brief Performance monitoring system for the trading terminal
 * 
 * Tracks FPS, frame times, mouse interaction latency, and memory usage
 * without interfering with the existing rendering pipeline.
 */
class PerformanceMonitor : public QObject {
    Q_OBJECT

public:
    explicit PerformanceMonitor(QObject* parent = nullptr);
    ~PerformanceMonitor() = default;

    // 🎯 FPS Monitoring
    void beginFrame();
    void endFrame();
    double getCurrentFPS() const;
    double getAverageFrameTime() const;
    
    // 🎯 Mouse Interaction Monitoring
    void beginMouseInteraction();
    void endMouseInteraction();
    double getLastMouseLatency() const;
    double getAverageMouseLatency() const;
    
    // 🎯 Memory Usage Monitoring
    void recordMemoryUsage();
    size_t getCurrentMemoryUsage() const;
    size_t getPeakMemoryUsage() const;
    
    // 🎯 UI Display
    QWidget* createPerformanceWidget(QWidget* parent = nullptr);
    void updateDisplay();
    
    // 🎯 Performance Alerts
    bool isPerformanceGood() const;
    QString getPerformanceStatus() const;

public slots:
    void onUpdateTimer();

signals:
    void performanceAlert(const QString& message);
    void fpsChanged(double fps);

private:
    // Frame timing data
    std::chrono::high_resolution_clock::time_point m_frameStartTime;
    std::vector<double> m_frameTimes;
    std::vector<double> m_fpsHistory;
    static constexpr size_t MAX_SAMPLES = 60; // 1 second of data at 60fps
    
    // Mouse interaction timing
    QElapsedTimer m_mouseTimer;
    std::vector<double> m_mouseLatencies;
    bool m_mouseInteractionActive = false;
    
    // Memory usage tracking
    std::vector<size_t> m_memoryUsageHistory;
    size_t m_peakMemoryUsage = 0;
    
    // UI components
    QLabel* m_fpsLabel = nullptr;
    QLabel* m_frameTimeLabel = nullptr;
    QLabel* m_mouseLatencyLabel = nullptr;
    QLabel* m_memoryLabel = nullptr;
    QLabel* m_statusLabel = nullptr;
    QTimer* m_updateTimer = nullptr;
    
    // Performance thresholds
    static constexpr double TARGET_FPS = 60.0;
    static constexpr double MAX_FRAME_TIME = 16.67; // 60fps = 16.67ms per frame
    static constexpr double MAX_MOUSE_LATENCY = 5.0; // 5ms max for smooth interaction
    
    // Helper methods
    double calculateAverage(const std::vector<double>& values) const;
    void addSample(std::vector<double>& container, double value);
    size_t getMemoryUsage() const;
    QString formatMemorySize(size_t bytes) const;
}; 