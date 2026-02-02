/*
Sentinel — PerformanceMonitor
*/
#include "PerformanceMonitor.hpp"
#include <QTimer>
#include <QFile>
#include <QTextStream>
#include <QThread>
#include <numeric>
#include <chrono>
#include <fstream>
#include <sstream>

// #region agent log
namespace {
inline int64_t debugNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

inline void appendDebugLog(const char* location,
                           const char* message,
                           const char* hypothesisId,
                           const std::string& dataJson,
                           const char* runId = "post-fix") {
    std::ofstream out("c:\\Users\\Pepe\\Documents\\Programming\\Sentinel\\.cursor\\debug.log", std::ios::app);
    if (!out.is_open()) {
        return;
    }
    out << "{\"sessionId\":\"debug-session\",\"runId\":\"" << runId
        << "\",\"hypothesisId\":\"" << hypothesisId
        << "\",\"location\":\"" << location
        << "\",\"message\":\"" << message
        << "\",\"data\":" << dataJson
        << ",\"timestamp\":" << debugNowMs() << "}\n";
}
}  // namespace
// #endregion

PerformanceMonitor& PerformanceMonitor::instance() {
    static PerformanceMonitor instance;
    return instance;
}

PerformanceMonitor::PerformanceMonitor()
    : QObject(nullptr)
{
    // Set up CPU monitoring timer
    m_cpuUpdateTimer = new QTimer(this);
    connect(m_cpuUpdateTimer, &QTimer::timeout, this, &PerformanceMonitor::updateCpuMetrics);
    m_cpuUpdateTimer->start(2000);  // Update CPU every 2 seconds (not too aggressive)
}

void PerformanceMonitor::attachToWindow(QQuickWindow* window) {
    if (!window) return;

    // Connect to frameSwapped signal for accurate FPS tracking
    connect(window, &QQuickWindow::frameSwapped, this, &PerformanceMonitor::onFrameSwapped, Qt::UniqueConnection);

    m_fpsTimer.start();
    m_frameCount = 0;
}

void PerformanceMonitor::onFrameSwapped() {
    // Track frame times
    const qint64 currentMs = m_fpsTimer.elapsed();
    static qint64 lastFrameMs = 0;

    if (lastFrameMs > 0) {
        const qint64 frameTimeMs = currentMs - lastFrameMs;
        m_frameTimesMs.push_back(frameTimeMs);
        if (m_frameTimesMs.size() > MAX_FRAME_SAMPLES) {
            m_frameTimesMs.pop_front();
        }
        if (frameTimeMs > 40) {
            std::ostringstream payload;
            payload << "{"
                    << "\"frameTimeMs\":" << frameTimeMs
                    << ",\"thread\":" << reinterpret_cast<quintptr>(QThread::currentThreadId())
                    << "}";
            appendDebugLog("PerformanceMonitor.cpp:34", "frame_swap_slow", "H5", payload.str());
        }

        // Calculate average frame time
        if (!m_frameTimesMs.empty()) {
            const double avgMs = std::accumulate(m_frameTimesMs.begin(), m_frameTimesMs.end(), 0.0) / m_frameTimesMs.size();
            m_avgFrameTimeMs.store(avgMs);
        }
    }
    lastFrameMs = currentMs;

    // Update FPS counter every second
    ++m_frameCount;
    if (currentMs >= 1000) {
        const double fps = (static_cast<double>(m_frameCount) * 1000.0) / currentMs;
        m_currentFps.store(fps);
        emit fpsChanged(fps);

        m_frameCount = 0;
        m_fpsTimer.restart();
        lastFrameMs = 0;
    }
}

void PerformanceMonitor::updateCpuUsage(int percent) {
    m_cpuPercent.store(percent);
    emit cpuUsageChanged(percent);
}

void PerformanceMonitor::updateGpuUsage(int percent) {
    m_gpuPercent.store(percent);
    emit gpuUsageChanged(percent);
}

void PerformanceMonitor::updateCpuMetrics() {
#ifdef __linux__
    const auto start = std::chrono::steady_clock::now();
    // Read /proc/stat for CPU usage
    static QFile statFile("/proc/stat");
    static qint64 prevIdle = 0, prevTotal = 0;

    if (statFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&statFile);
        QString line = in.readLine();  // First line is aggregate CPU
        statFile.close();

        if (line.startsWith("cpu ")) {
            QStringList tokens = line.split(' ', Qt::SkipEmptyParts);
            if (tokens.size() >= 5) {
                // user, nice, system, idle, iowait, irq, softirq, ...
                qint64 user = tokens[1].toLongLong();
                qint64 nice = tokens[2].toLongLong();
                qint64 system = tokens[3].toLongLong();
                qint64 idle = tokens[4].toLongLong();
                qint64 iowait = tokens.size() > 5 ? tokens[5].toLongLong() : 0;

                qint64 idleTotal = idle + iowait;
                qint64 total = user + nice + system + idle + iowait;

                if (prevTotal > 0) {
                    qint64 diffIdle = idleTotal - prevIdle;
                    qint64 diffTotal = total - prevTotal;

                    if (diffTotal > 0) {
                        int cpuPercent = static_cast<int>(100.0 * (diffTotal - diffIdle) / diffTotal);
                        updateCpuUsage(cpuPercent);
                    }
                }

                prevIdle = idleTotal;
                prevTotal = total;
            }
        }
    }
    const auto end = std::chrono::steady_clock::now();
    const auto durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    if (durationMs > 10) {
        std::ostringstream payload;
        payload << "{"
                << "\"durationMs\":" << durationMs
                << ",\"thread\":" << reinterpret_cast<quintptr>(QThread::currentThreadId())
                << "}";
        appendDebugLog("PerformanceMonitor.cpp:77", "cpu_metrics_slow", "H6", payload.str());
    }
#else
    // TODO: Windows/macOS CPU tracking
    updateCpuUsage(0);
#endif
}
