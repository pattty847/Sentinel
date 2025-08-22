/*
Sentinel — SentinelMonitor
Role: Implements unified monitoring for all aspects of the trading terminal.
Inputs/Outputs: Collects metrics from trading pipeline; emits alerts and provides dashboard data.
Threading: Thread-safe implementation using atomic operations and mutexes where needed.
Performance: Optimized for minimal overhead in high-frequency trading environment.
Integration: Central hub connecting MarketDataCore, rendering, and system monitoring.
Observability: Comprehensive logging and real-time metric emission for professional trading.
Related: SentinelMonitor.hpp, MarketDataCore.cpp, UnifiedGridRenderer.cpp.
Assumptions: System clock is reasonably accurate for latency measurements.
*/

#include "SentinelMonitor.hpp"
#include "SentinelLogging.hpp"
#include "Cpp20Utils.hpp"
#include <QDebug>
#include <algorithm>
#include <numeric>

// Platform-specific includes for system metrics
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/task_info.h>
#include <mach/mach_init.h>
#include <mach/mach_host.h>
#elif _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#include <unistd.h>
#endif

SentinelMonitor::SentinelMonitor(QObject* parent) : QObject(parent) {
    // Initialize data structures
    m_rendering.frameTimes.reserve(MAX_FRAME_SAMPLES);
    m_network.tradeLatencies.reserve(MAX_LATENCY_SAMPLES);
    m_network.orderBookLatencies.reserve(MAX_LATENCY_SAMPLES);
    m_system.memoryUsageHistory.reserve(MAX_FRAME_SAMPLES);
    m_system.cpuUsageHistory.reserve(MAX_FRAME_SAMPLES);
    
    // Setup performance monitoring timer
    m_performanceTimer = new QTimer(this);
    connect(m_performanceTimer, &QTimer::timeout, this, &SentinelMonitor::onPerformanceTimer);
    
    // Initialize timing
    m_marketData.startTime = std::chrono::steady_clock::now();
    m_rendering.frameTimer.start();
    
    sLog_App("🚀 SentinelMonitor: Unified monitoring system initialized");
    sLog_App("📊 Monitoring: Network latency, rendering performance, market data flow");
}

// 🌐 NETWORK & LATENCY MONITORING

void SentinelMonitor::recordTradeLatency(std::chrono::system_clock::time_point exchange_time,
                                        std::chrono::system_clock::time_point arrival_time) {
    auto latency = std::chrono::duration_cast<std::chrono::microseconds>(arrival_time - exchange_time);
    double latency_ms = latency.count() / 1000.0;
    
    {
        std::lock_guard<std::mutex> lock(m_network.latencyMutex);
        addLatencySample(m_network.tradeLatencies, latency_ms);
    }
    
    // Alert on high latency
    if (latency_ms > MAX_ACCEPTABLE_LATENCY_MS) {
        emit latencyAlert(latency_ms);
        sLog_Warning("⚠️ High trade latency detected:" << latency_ms << "ms");
    }
    
    emit latencyChanged(getAverageTradeLatency());
}

void SentinelMonitor::recordOrderBookLatency(std::chrono::system_clock::time_point exchange_time,
                                           std::chrono::system_clock::time_point arrival_time) {
    auto latency = std::chrono::duration_cast<std::chrono::microseconds>(arrival_time - exchange_time);
    double latency_ms = latency.count() / 1000.0;
    
    {
        std::lock_guard<std::mutex> lock(m_network.latencyMutex);
        addLatencySample(m_network.orderBookLatencies, latency_ms);
    }
}

void SentinelMonitor::recordNetworkReconnect() {
    m_network.reconnectCount.fetch_add(1);
    m_network.lastReconnect = std::chrono::steady_clock::now();
    
    emit networkIssue(QString("Network reconnection #%1").arg(m_network.reconnectCount.load()));
    sLog_Warning("🔌 Network reconnect detected, count:" << m_network.reconnectCount.load());
}

void SentinelMonitor::recordNetworkError(const QString& error) {
    m_network.networkErrors.fetch_add(1);
    emit networkIssue(QString("Network error: %1").arg(error));
    sLog_Error("❌ Network error:" << error);
}

// 🎮 RENDERING & GPU MONITORING

void SentinelMonitor::startFrame() {
    std::lock_guard<std::mutex> lock(m_rendering.frameMutex);
    m_rendering.frameTimer.restart();
    m_rendering.frameTimingActive = true;
}

void SentinelMonitor::endFrame() {
    std::lock_guard<std::mutex> lock(m_rendering.frameMutex);
    
    if (!m_rendering.frameTimingActive) return;
    
    qint64 frameTime = m_rendering.frameTimer.nsecsElapsed() / 1000; // Convert to microseconds
    m_rendering.lastFrameTime_us = frameTime;
    addFrameSample(frameTime);
    
    // Check for frame drops
    double frameTimeMs = frameTime / 1000.0;
    if (frameTimeMs > MAX_FRAME_TIME_MS) {
        m_rendering.frameDrops.fetch_add(1);
        emit frameDropDetected(frameTimeMs);
        
        if (frameTimeMs > ALERT_FRAME_TIME_MS) {
            emit performanceAlert(QString("Severe frame drop: %1ms").arg(frameTimeMs, 0, 'f', 2));
        }
    }
    
    m_rendering.frameTimingActive = false;
    emit fpsChanged(getCurrentFPS());
}

void SentinelMonitor::recordCacheHit() {
    m_rendering.cacheHits.fetch_add(1);
}

void SentinelMonitor::recordCacheMiss() {
    m_rendering.cacheMisses.fetch_add(1);
}

void SentinelMonitor::recordGeometryRebuild() {
    m_rendering.geometryRebuilds.fetch_add(1);
}

void SentinelMonitor::recordGPUUpload(size_t bytes) {
    m_rendering.gpuBytesUploaded.fetch_add(bytes);
}

void SentinelMonitor::recordTransformApplied() {
    m_rendering.transformsApplied.fetch_add(1);
}

// 📊 MARKET DATA MONITORING

void SentinelMonitor::recordTradeProcessed(const Trade& trade) {
    m_marketData.tradesProcessed.fetch_add(1);
    
    // Calculate trade flow rate for throughput
    emit throughputChanged(getTradesThroughput());
    
    // Log significant trades (throttled)
    static std::atomic<size_t> tradeLogCount{0};
    if (++tradeLogCount % 100 == 1) {
        std::string logMessage = Cpp20Utils::formatTradeLog(
            trade.product_id, trade.price, trade.size, 
            (trade.side == AggressorSide::Buy ? "BUY" : "SELL"), 
            tradeLogCount.load()
        );
        sLog_Data(QString::fromStdString(logMessage));
    }
}

void SentinelMonitor::recordOrderBookUpdate(const QString& symbol, size_t bidLevels, size_t askLevels) {
    m_marketData.orderBookUpdates.fetch_add(1);
    
    // Log order book health (throttled)
    static std::atomic<size_t> bookLogCount{0};
    if (++bookLogCount % 1000 == 1) {
        std::string logMessage = Cpp20Utils::formatOrderBookLog(
            symbol.toStdString(), bidLevels, askLevels, 1
        );
        sLog_Data(QString::fromStdString(logMessage));
    }
}

void SentinelMonitor::recordCVDUpdate(double cvd) {
    std::lock_guard<std::mutex> lock(m_marketData.marketMutex);
    m_marketData.currentCVD = cvd;
}

void SentinelMonitor::recordPriceMovement(const QString& symbol, double oldPrice, double newPrice) {
    std::lock_guard<std::mutex> lock(m_marketData.marketMutex);
    
    double changePercent = ((newPrice - oldPrice) / oldPrice) * 100.0;
    m_marketData.recentPriceMovements.emplace_back(symbol, changePercent);
    
    // Keep only recent movements
    if (m_marketData.recentPriceMovements.size() > 100) {
        m_marketData.recentPriceMovements.erase(m_marketData.recentPriceMovements.begin());
    }
}

void SentinelMonitor::recordPointsPushed(size_t count) {
    m_marketData.pointsPushed.fetch_add(count);
}

// 💻 SYSTEM RESOURCE MONITORING

void SentinelMonitor::recordMemoryUsage() {
    size_t currentUsage = getMemoryUsage();
    
    {
        std::lock_guard<std::mutex> lock(m_system.systemMutex);
        m_system.memoryUsageHistory.push_back(currentUsage);
        
        if (m_system.memoryUsageHistory.size() > MAX_FRAME_SAMPLES) {
            m_system.memoryUsageHistory.erase(m_system.memoryUsageHistory.begin());
        }
        
        if (currentUsage > m_system.peakMemoryUsage) {
            m_system.peakMemoryUsage = currentUsage;
        }
    }
    
    // Alert on high memory usage
    if (currentUsage > MEMORY_ALERT_THRESHOLD_MB * 1024 * 1024) {
        emit memoryPressure(currentUsage / (1024 * 1024));
    }
}

void SentinelMonitor::recordCPUUsage() {
    double cpuUsage = getCPUUsage();
    
    std::lock_guard<std::mutex> lock(m_system.systemMutex);
    m_system.cpuUsageHistory.push_back(cpuUsage);
    
    if (m_system.cpuUsageHistory.size() > MAX_FRAME_SAMPLES) {
        m_system.cpuUsageHistory.erase(m_system.cpuUsageHistory.begin());
    }
}

// 📈 PERFORMANCE METRICS ACCESS

double SentinelMonitor::getCurrentFPS() const {
    std::lock_guard<std::mutex> lock(m_rendering.frameMutex);
    
    if (m_rendering.frameTimes.size() < 2) return 0.0;
    
    qint64 totalTime = 0;
    for (qint64 time : m_rendering.frameTimes) {
        totalTime += time;
    }
    
    return (m_rendering.frameTimes.size() - 1) * 1000000.0 / totalTime;  // Convert μs to FPS
}

double SentinelMonitor::getAverageFrameTime() const {
    std::lock_guard<std::mutex> lock(m_rendering.frameMutex);
    
    if (m_rendering.frameTimes.empty()) return 0.0;
    
    qint64 totalTime = 0;
    for (qint64 time : m_rendering.frameTimes) {
        totalTime += time;
    }
    
    return totalTime / (m_rendering.frameTimes.size() * 1000.0);  // Convert μs to ms
}

double SentinelMonitor::getAverageTradeLatency() const {
    std::lock_guard<std::mutex> lock(m_network.latencyMutex);
    
    if (m_network.tradeLatencies.empty()) return 0.0;
    
    double sum = std::accumulate(m_network.tradeLatencies.begin(), m_network.tradeLatencies.end(), 0.0);
    return sum / m_network.tradeLatencies.size();
}

double SentinelMonitor::getAverageOrderBookLatency() const {
    std::lock_guard<std::mutex> lock(m_network.latencyMutex);
    
    if (m_network.orderBookLatencies.empty()) return 0.0;
    
    double sum = std::accumulate(m_network.orderBookLatencies.begin(), m_network.orderBookLatencies.end(), 0.0);
    return sum / m_network.orderBookLatencies.size();
}

double SentinelMonitor::getCacheHitRate() const {
    qint64 hits = m_rendering.cacheHits.load();
    qint64 misses = m_rendering.cacheMisses.load();
    qint64 total = hits + misses;
    
    return (total > 0) ? (hits * 100.0 / total) : 0.0;
}

size_t SentinelMonitor::getTotalGPUUploads() const {
    return m_rendering.gpuBytesUploaded.load();
}

double SentinelMonitor::getTradesThroughput() const {
    double elapsed = getElapsedSeconds();
    if (elapsed < 0.001) return 0.0;
    
    return static_cast<double>(m_marketData.tradesProcessed.load()) / elapsed;
}

double SentinelMonitor::getPointsThroughput() const {
    double elapsed = getElapsedSeconds();
    if (elapsed < 0.001) return 0.0;
    
    return static_cast<double>(m_marketData.pointsPushed.load()) / elapsed;
}

size_t SentinelMonitor::getCurrentMemoryUsage() const {
    return getMemoryUsage();
}

// 🚨 PERFORMANCE ANALYSIS

bool SentinelMonitor::isPerformanceHealthy() const {
    bool noFrameDrops = (m_rendering.frameDrops.load() == 0);
    bool goodThroughput = (getPointsThroughput() >= 20000.0);
    bool acceptableLatency = isLatencyAcceptable();
    
    return noFrameDrops && goodThroughput && acceptableLatency;
}

bool SentinelMonitor::isLatencyAcceptable() const {
    double avgLatency = getAverageTradeLatency();
    return avgLatency <= MAX_ACCEPTABLE_LATENCY_MS;
}

bool SentinelMonitor::isNetworkStable() const {
    size_t reconnects = m_network.reconnectCount.load();
    size_t errors = m_network.networkErrors.load();
    
    // Network is stable if we have few reconnects and errors
    return reconnects < 5 && errors < 10;
}

QString SentinelMonitor::getPerformanceStatus() const {
    if (isPerformanceHealthy()) {
        return "🟢 EXCELLENT - All systems optimal";
    } else if (isLatencyAcceptable() && isNetworkStable()) {
        return "🟡 GOOD - Minor performance issues";
    } else {
        return "🔴 ISSUES - Performance degraded";
    }
}

QString SentinelMonitor::getComprehensiveStats() const {
    return QString(
        "FPS: %1 | Frame: %2ms | Trade Latency: %3ms | Cache: %4% | "
        "Trades/s: %5 | Memory: %6MB | Network: %7 reconnects"
    )
    .arg(getCurrentFPS(), 0, 'f', 1)
    .arg(getAverageFrameTime(), 0, 'f', 2)
    .arg(getAverageTradeLatency(), 0, 'f', 2)
    .arg(getCacheHitRate(), 0, 'f', 1)
    .arg(getTradesThroughput(), 0, 'f', 0)
    .arg(static_cast<double>(getCurrentMemoryUsage()) / (1024 * 1024), 0, 'f', 0)
    .arg(static_cast<int>(m_network.reconnectCount.load()));
}

// 🎯 CONTROL & MANAGEMENT

void SentinelMonitor::enablePerformanceOverlay(bool enabled) {
    m_overlayEnabled = enabled;
    sLog_App("📊 Performance overlay:" << (enabled ? "ENABLED" : "DISABLED"));
}

void SentinelMonitor::enableCLIOutput(bool enabled) {
    m_cliOutputEnabled = enabled;
    
    if (enabled) {
        m_performanceTimer->start(1000); // Dump every second
        sLog_App("📊 CLI monitoring output: ENABLED (1s interval)");
    } else {
        m_performanceTimer->stop();
        sLog_App("📊 CLI monitoring output: DISABLED");
    }
}

void SentinelMonitor::reset() {
    // Reset all metrics
    {
        std::lock_guard<std::mutex> lock(m_rendering.frameMutex);
        m_rendering.frameTimes.clear();
        m_rendering.cacheHits.store(0);
        m_rendering.cacheMisses.store(0);
        m_rendering.geometryRebuilds.store(0);
        m_rendering.transformsApplied.store(0);
        m_rendering.gpuBytesUploaded.store(0);
        m_rendering.frameDrops.store(0);
    }
    
    {
        std::lock_guard<std::mutex> lock(m_network.latencyMutex);
        m_network.tradeLatencies.clear();
        m_network.orderBookLatencies.clear();
        m_network.reconnectCount.store(0);
        m_network.networkErrors.store(0);
    }
    
    m_marketData.tradesProcessed.store(0);
    m_marketData.orderBookUpdates.store(0);
    m_marketData.pointsPushed.store(0);
    m_marketData.startTime = std::chrono::steady_clock::now();
    
    {
        std::lock_guard<std::mutex> lock(m_system.systemMutex);
        m_system.memoryUsageHistory.clear();
        m_system.cpuUsageHistory.clear();
        m_system.peakMemoryUsage = 0;
    }
    
    sLog_App("🔄 SentinelMonitor: All metrics reset");
}

void SentinelMonitor::startMonitoring() {
    m_monitoringActive = true;
    sLog_App("▶️ SentinelMonitor: Monitoring started");
}

void SentinelMonitor::stopMonitoring() {
    m_monitoringActive = false;
    m_performanceTimer->stop();
    sLog_App("⏹️ SentinelMonitor: Monitoring stopped");
}

void SentinelMonitor::onPerformanceTimer() {
    if (!m_cliOutputEnabled || !m_monitoringActive) return;
    
    updateSystemMetrics();
    
    qDebug() << "📊 SENTINEL PERFORMANCE:"
             << "FPS:" << static_cast<int>(getCurrentFPS())
             << "Latency:" << static_cast<int>(getAverageTradeLatency()) << "ms"
             << "Trades/s:" << static_cast<int>(getTradesThroughput())
             << "Cache:" << static_cast<int>(getCacheHitRate()) << "%"
             << "Memory:" << static_cast<int>(getCurrentMemoryUsage() / (1024 * 1024)) << "MB"
             << "Status:" << getPerformanceStatus();
    
    checkPerformanceThresholds();
}

// 🔧 HELPER METHODS

double SentinelMonitor::getElapsedSeconds() const {
    auto now = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(now - m_marketData.startTime);
    return static_cast<double>(duration.count()) / 1'000'000.0;
}

void SentinelMonitor::addLatencySample(std::vector<double>& samples, double latency_ms) {
    samples.push_back(latency_ms);
    if (samples.size() > MAX_LATENCY_SAMPLES) {
        samples.erase(samples.begin());
    }
}

void SentinelMonitor::addFrameSample(qint64 frame_time_us) {
    m_rendering.frameTimes.push_back(frame_time_us);
    if (m_rendering.frameTimes.size() > MAX_FRAME_SAMPLES) {
        m_rendering.frameTimes.erase(m_rendering.frameTimes.begin());
    }
}

void SentinelMonitor::checkPerformanceThresholds() {
    // Check for performance degradation
    if (!isPerformanceHealthy()) {
        emit performanceAlert("Performance degraded - check FPS, latency, and throughput");
    }
    
    // Check memory pressure
    size_t memUsage = getCurrentMemoryUsage() / (1024 * 1024);
    if (memUsage > MEMORY_ALERT_THRESHOLD_MB) {
        emit memoryPressure(memUsage);
    }
}

void SentinelMonitor::updateSystemMetrics() {
    recordMemoryUsage();
    recordCPUUsage();
}

// Platform-specific system metrics implementation
size_t SentinelMonitor::getMemoryUsage() const {
#ifdef __APPLE__
    task_basic_info info;
    mach_msg_type_number_t size = sizeof(info);
    task_info(mach_task_self(), TASK_BASIC_INFO, (task_info_t)&info, &size);
    return info.resident_size;
#elif _WIN32
    PROCESS_MEMORY_COUNTERS pmc;
    GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
    return pmc.WorkingSetSize;
#else
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss * 1024; // Convert KB to bytes on Linux
#endif
}

double SentinelMonitor::getCPUUsage() const {
    // Simplified CPU usage - in production, this would use platform-specific APIs
    // For now, return 0.0 as placeholder
    return 0.0;
}
