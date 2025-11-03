/*
Sentinel — SentinelMonitor
Role: Unified performance, latency, and market data monitoring system for professional trading.
Inputs/Outputs: Collects metrics from all subsystems; provides real-time stats and alerts.
Threading: Thread-safe atomic operations; designed for multi-threaded trading environment.
Performance: Lightweight metric collection with minimal impact on trading performance.
Integration: Central monitoring hub for MarketDataCore, rendering pipeline, and network layer.
Observability: Primary observability tool for the entire Sentinel trading terminal.
Related: SentinelMonitor.cpp, MarketDataCore.hpp, UnifiedGridRenderer.h.
Assumptions: Exchange timestamps are parsed correctly for latency calculations.
*/
#pragma once

#include <QObject>
#include <QElapsedTimer>
#include <QTimer>
#include <QString>
#include <chrono>
#include <vector>
#include <atomic>
#include <memory>
#include <mutex>
#include "marketdata/model/TradeData.h"

/**
 *  SENTINEL UNIFIED MONITORING SYSTEM
 * 
 * Consolidates all monitoring functionality into a single, professional-grade system:
 * - Network & Exchange Latency Analysis
 * - Rendering Performance & GPU Metrics  
 * - Market Data Flow & Statistics
 * - System Resources & Alerts
 * - Real-time Performance Dashboard
 */
class SentinelMonitor : public QObject {
    Q_OBJECT

public:
    explicit SentinelMonitor(QObject* parent = nullptr);
    ~SentinelMonitor() = default;

    // 🌐 NETWORK & LATENCY MONITORING
    void recordTradeLatency(std::chrono::system_clock::time_point exchange_time,
                           std::chrono::system_clock::time_point arrival_time);
    void recordOrderBookLatency(std::chrono::system_clock::time_point exchange_time,
                               std::chrono::system_clock::time_point arrival_time);
    void recordNetworkReconnect();
    void recordNetworkError(const QString& error);
    
    // 🎮 RENDERING & GPU MONITORING  
    void startFrame();
    void endFrame();
    void recordCacheHit();
    void recordCacheMiss();
    void recordGeometryRebuild();
    void recordGPUUpload(size_t bytes);
    void recordTransformApplied();
    
    //  MARKET DATA MONITORING
    void recordTradeProcessed(const Trade& trade);
    void recordOrderBookUpdate(const QString& symbol, size_t bidLevels, size_t askLevels);
    void recordCVDUpdate(double cvd);
    void recordPriceMovement(const QString& symbol, double oldPrice, double newPrice);
    
    // 💻 SYSTEM RESOURCE MONITORING
    void recordMemoryUsage();
    void recordCPUUsage();
    void recordPointsPushed(size_t count);
    
    //  PERFORMANCE METRICS ACCESS
    double getCurrentFPS() const;
    double getAverageFrameTime() const;
    double getAverageTradeLatency() const;
    double getAverageOrderBookLatency() const;
    double getCacheHitRate() const;
    size_t getTotalGPUUploads() const;
    double getTradesThroughput() const;
    double getPointsThroughput() const;
    size_t getCurrentMemoryUsage() const;
    double getLastCpuTimeMs() const;
    
    // 🚨 PERFORMANCE ANALYSIS
    bool isPerformanceHealthy() const;
    bool isLatencyAcceptable() const;
    bool isNetworkStable() const;
    QString getPerformanceStatus() const;
    QString getComprehensiveStats() const;
    
    //  PROFESSIONAL DASHBOARD
    void enablePerformanceOverlay(bool enabled);
    void enableCLIOutput(bool enabled);
    bool isOverlayEnabled() const { return m_overlayEnabled; }
    // Note: Dashboard creation moved to GUI layer to avoid QWidget dependency
    
    //  CONTROL
    void reset();
    void startMonitoring();
    void stopMonitoring();

public slots:
    void onPerformanceTimer();

signals:
    void performanceAlert(const QString& issue);
    void latencyAlert(double latency_ms);
    void networkIssue(const QString& details);
    void memoryPressure(size_t usage_mb);
    void frameDropDetected(double frame_time_ms);
    
    // Real-time metric updates
    void fpsChanged(double fps);
    void latencyChanged(double avg_latency_ms);
    void throughputChanged(double trades_per_sec);

private:
    struct NetworkMetrics {
        std::vector<double> tradeLatencies;          // Exchange → arrival latency (ms)
        std::vector<double> orderBookLatencies;     // Exchange → arrival latency (ms)
        std::atomic<size_t> reconnectCount{0};
        std::atomic<size_t> networkErrors{0};
        std::chrono::steady_clock::time_point lastReconnect;
        mutable std::mutex latencyMutex;
    };
    
    struct RenderingMetrics {
        QElapsedTimer frameTimer;
        QElapsedTimer frameProcessingTimer;
        std::vector<qint64> frameTimes;             // Frame times in microseconds
        std::atomic<qint64> cacheHits{0};
        std::atomic<qint64> cacheMisses{0};
        std::atomic<qint64> geometryRebuilds{0};
        std::atomic<qint64> transformsApplied{0};
        std::atomic<size_t> gpuBytesUploaded{0};
        std::atomic<size_t> frameDrops{0};
        qint64 lastFrameTime_us = 0;
        qint64 lastCpuTime_us = 0;
        bool frameTimingActive = false;
        bool hasFrameBaseline = false;
        mutable std::mutex frameMutex;
    };
    
    struct MarketDataMetrics {
        std::atomic<size_t> tradesProcessed{0};
        std::atomic<size_t> orderBookUpdates{0};
        std::atomic<size_t> pointsPushed{0};
        double currentCVD = 0.0;
        std::vector<std::pair<QString, double>> recentPriceMovements;
        std::chrono::steady_clock::time_point startTime;
        mutable std::mutex marketMutex;
    };
    
    struct SystemMetrics {
        std::vector<size_t> memoryUsageHistory;
        std::vector<double> cpuUsageHistory;
        size_t peakMemoryUsage = 0;
        mutable std::mutex systemMutex;
    };
    
    NetworkMetrics m_network;
    RenderingMetrics m_rendering;
    MarketDataMetrics m_marketData;
    SystemMetrics m_system;
    
    // Configuration & Control
    bool m_overlayEnabled = false;
    bool m_cliOutputEnabled = false;
    bool m_monitoringActive = false;
    QTimer* m_performanceTimer = nullptr;
    
    // Performance thresholds (configurable)
    static constexpr double MAX_ACCEPTABLE_LATENCY_MS = 50.0;
    static constexpr double MAX_FRAME_TIME_MS = 16.67; // 60 FPS
    static constexpr qint64 ALERT_FRAME_TIME_MS = 20;
    static constexpr size_t MAX_FRAME_SAMPLES = 60;
    static constexpr size_t MAX_LATENCY_SAMPLES = 100;
    static constexpr size_t MEMORY_ALERT_THRESHOLD_MB = 1024;
    
    // Helper methods
    double getElapsedSeconds() const;
    void addLatencySample(std::vector<double>& samples, double latency_ms);
    void addFrameSample(qint64 frame_time_us);
    double computeCurrentFPSLocked() const;
    double computeAverageFrameTimeLocked() const;
    void checkPerformanceThresholds();
    void updateSystemMetrics();
    QString buildPerformanceSummary() const;
    
    // System-specific memory tracking
    size_t getMemoryUsage() const;
    double getCPUUsage() const;
};
