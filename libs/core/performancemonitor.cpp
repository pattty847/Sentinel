#include "performancemonitor.h"
#include "Log.hpp"
#include <QTimer>
#include <algorithm>

static constexpr auto CAT = "Perf";

PerformanceMonitor::PerformanceMonitor(QObject* parent)
    : QObject(parent)
    , m_startTime(std::chrono::steady_clock::now())
{
    // Initialize frame times array
    m_frameTimes.fill(0);
    
    LOG_I(CAT, "🚀 PerformanceMonitor: Initialized with frame drop threshold {} ms", MAX_FRAME_TIME_MS);
}

void PerformanceMonitor::startFrame() {
    m_frameTimer.start();
    m_frameTimingActive = true;
}

void PerformanceMonitor::endFrame() {
    if (!m_frameTimingActive) return;
    
    qint64 frameTime = m_frameTimer.elapsed();
    
    // Update rolling window
    m_frameTimes[m_frameIndex] = frameTime;
    m_frameIndex = (m_frameIndex + 1) % FRAME_WINDOW_SIZE;
    
    // Track worst frame time
    qint64 currentWorst = m_worstFrameTime.load();
    while (frameTime > currentWorst && 
           !m_worstFrameTime.compare_exchange_weak(currentWorst, frameTime)) {
        // CAS loop - keep trying until we succeed or frameTime is no longer worst
    }
    
    // Check for frame drops
    if (frameTime > MAX_FRAME_TIME_MS) {
        m_frameDrops.fetch_add(1, std::memory_order_relaxed);
        emit frameDropDetected(frameTime);
        
        if (frameTime > ALERT_FRAME_TIME_MS) {
            emit performanceAlert(QString("Severe frame drop: %1ms").arg(frameTime));
        }
    }
    
    m_frameTimingActive = false;
}

void PerformanceMonitor::checkFrameTimes() {
    qint64 avgFrameTime = getAverageFrameTime();
    if (avgFrameTime > MAX_FRAME_TIME_MS) {
        emit performanceAlert(QString("Average frame time degraded: %1ms").arg(avgFrameTime));
    }
}

void PerformanceMonitor::recordPointsPushed(size_t count) {
    m_pointsPushed.fetch_add(count, std::memory_order_relaxed);
}

void PerformanceMonitor::recordTradesProcessed(size_t count) {
    m_tradesProcessed.fetch_add(count, std::memory_order_relaxed);
}

double PerformanceMonitor::getPointsThroughput() const {
    double elapsed = getElapsedSeconds();
    if (elapsed < 0.001) return 0.0; // Avoid division by zero
    
    return static_cast<double>(m_pointsPushed.load()) / elapsed;
}

double PerformanceMonitor::getTradesThroughput() const {
    double elapsed = getElapsedSeconds();
    if (elapsed < 0.001) return 0.0; // Avoid division by zero
    
    return static_cast<double>(m_tradesProcessed.load()) / elapsed;
}

bool PerformanceMonitor::getAllGatesPassed() const {
    // Performance gates from PLAN.md Phase 3:
    // - 0 dropped frames @ 20k msg/s
    // - Points throughput >= 20k points/second
    
    bool noFrameDrops = (m_frameDrops.load() == 0);
    bool sufficientThroughput = (getPointsThroughput() >= 20000.0);
    
    return noFrameDrops && sufficientThroughput;
}

void PerformanceMonitor::enableCLIOutput(bool enabled) {
    m_cliOutputEnabled = enabled;
    
    if (enabled) {
        if (!m_metricsTimer) {
            m_metricsTimer = new QTimer(this);
            connect(m_metricsTimer, &QTimer::timeout, this, &PerformanceMonitor::dumpMetrics);
        }
        m_metricsTimer->start(1000); // Dump every second
        LOG_I(CAT, "📊 PerformanceMonitor: CLI output enabled (1s interval)");
    } else {
        if (m_metricsTimer) {
            m_metricsTimer->stop();
        }
        LOG_I(CAT, "📊 PerformanceMonitor: CLI output disabled");
    }
}

void PerformanceMonitor::reset() {
    m_pointsPushed.store(0);
    m_tradesProcessed.store(0);
    m_frameDrops.store(0);
    m_worstFrameTime.store(0);
    m_frameIndex = 0;
    m_frameTimes.fill(0);
    m_startTime = std::chrono::steady_clock::now();
    
    LOG_D(CAT, "🔄 PerformanceMonitor: Statistics reset");
}

void PerformanceMonitor::dumpMetrics() {
    if (!m_cliOutputEnabled) return;
    
    double elapsed = getElapsedSeconds();
    double pointsThroughput = getPointsThroughput();
    double tradesThroughput = getTradesThroughput();
    qint64 avgFrameTime = getAverageFrameTime();
    
    // Emit "points pushed / Δt" to prove zero drops at 20k msgs/s
    LOG_I(CAT,
           "📊 PERFORMANCE METRICS: Points/s:{} Trades/s:{} Avg frame:{} ms Worst frame:{} ms Frame drops:{} Runtime:{} s",
           static_cast<int>(pointsThroughput), static_cast<int>(tradesThroughput),
           avgFrameTime, m_worstFrameTime.load(), m_frameDrops.load(), static_cast<int>(elapsed));
    
    // Performance gate validation
    if (!getAllGatesPassed()) {
        LOG_W(CAT, "⚠️ PERFORMANCE GATE FAILURE! - Frame drops:{} - Points throughput:{} (target: 20000)",
              m_frameDrops.load(), static_cast<int>(pointsThroughput));
    }
}

double PerformanceMonitor::getElapsedSeconds() const {
    auto now = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(now - m_startTime);
    return static_cast<double>(duration.count()) / 1'000'000.0;
}

qint64 PerformanceMonitor::getAverageFrameTime() const {
    qint64 sum = 0;
    int validFrames = 0;
    
    for (qint64 frameTime : m_frameTimes) {
        if (frameTime > 0) {
            sum += frameTime;
            validFrames++;
        }
    }
    
    return validFrames > 0 ? (sum / validFrames) : 0;
} 