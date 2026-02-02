/*
Sentinel — PerformanceMonitor
*/
#include "PerformanceMonitor.hpp"
#include "SentinelLogging.hpp"
#include <QTimer>
#include <QFile>
#include <QTextStream>
#include <QThread>
#include <numeric>

#ifdef _WIN32
#include <windows.h>
#include <pdh.h>
#pragma comment(lib, "pdh.lib")
#endif

PerformanceMonitor& PerformanceMonitor::instance() {
    static PerformanceMonitor instance;
    return instance;
}

PerformanceMonitor::PerformanceMonitor()
    : QObject(nullptr)
{
#ifdef _WIN32
    initWindowsCounters();
#endif

    // Set up CPU/GPU monitoring timer
    m_cpuUpdateTimer = new QTimer(this);
    connect(m_cpuUpdateTimer, &QTimer::timeout, this, &PerformanceMonitor::updateCpuMetrics);
    m_cpuUpdateTimer->start(2000);  // Update every 2 seconds
}

PerformanceMonitor::~PerformanceMonitor() {
#ifdef _WIN32
    cleanupWindowsCounters();
#endif
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

void PerformanceMonitor::updateLatency(int milliseconds) {
    m_latencyMs.store(milliseconds);
    emit latencyChanged(milliseconds);
}

void PerformanceMonitor::updateCpuMetrics() {
#ifdef _WIN32
    // Windows PDH API for CPU and GPU
    if (!m_pdhQuery) return;

    PDH_STATUS status = PdhCollectQueryData(static_cast<PDH_HQUERY>(m_pdhQuery));
    if (status != ERROR_SUCCESS) {
        return;
    }

    // Query CPU usage
    if (m_cpuCounter) {
        PDH_FMT_COUNTERVALUE counterValue;
        status = PdhGetFormattedCounterValue(
            static_cast<PDH_HCOUNTER>(m_cpuCounter),
            PDH_FMT_DOUBLE,
            nullptr,
            &counterValue
        );
        if (status == ERROR_SUCCESS) {
            int cpuPercent = static_cast<int>(counterValue.doubleValue);
            updateCpuUsage(cpuPercent);
        }
    }

    // Query GPU usage (Windows 10+, may fail on older systems)
    if (m_gpuCounter) {
        PDH_FMT_COUNTERVALUE counterValue;
        status = PdhGetFormattedCounterValue(
            static_cast<PDH_HCOUNTER>(m_gpuCounter),
            PDH_FMT_DOUBLE,
            nullptr,
            &counterValue
        );
        if (status == ERROR_SUCCESS) {
            int gpuPercent = static_cast<int>(counterValue.doubleValue);
            updateGpuUsage(gpuPercent);
        }
    }

#elif defined(__linux__)
    // Linux /proc/stat CPU monitoring
    static QFile statFile("/proc/stat");
    static qint64 prevIdle = 0, prevTotal = 0;

    if (statFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&statFile);
        QString line = in.readLine();
        statFile.close();

        if (line.startsWith("cpu ")) {
            QStringList tokens = line.split(' ', Qt::SkipEmptyParts);
            if (tokens.size() >= 5) {
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
#else
    // macOS: TODO
    updateCpuUsage(0);
#endif
}

#ifdef _WIN32
void PerformanceMonitor::initWindowsCounters() {
    PDH_STATUS status;

    // Create PDH query
    status = PdhOpenQuery(nullptr, 0, reinterpret_cast<PDH_HQUERY*>(&m_pdhQuery));
    if (status != ERROR_SUCCESS) {
        sLog_Error("Failed to open PDH query for performance monitoring");
        return;
    }

    // Add CPU counter: Total processor time
    status = PdhAddCounterW(
        static_cast<PDH_HQUERY>(m_pdhQuery),
        L"\\Processor(_Total)\\% Processor Time",
        0,
        reinterpret_cast<PDH_HCOUNTER*>(&m_cpuCounter)
    );
    if (status != ERROR_SUCCESS) {
        sLog_Warning("Failed to add CPU counter (PDH)");
        m_cpuCounter = nullptr;
    } else {
        sLog_App("Windows CPU monitoring initialized (PDH API)");
    }

    // Add GPU counter: GPU Engine utilization (Windows 10+ only)
    status = PdhAddCounterW(
        static_cast<PDH_HQUERY>(m_pdhQuery),
        L"\\GPU Engine(*)\\Utilization Percentage",
        0,
        reinterpret_cast<PDH_HCOUNTER*>(&m_gpuCounter)
    );
    if (status != ERROR_SUCCESS) {
        // GPU counter may fail on older Windows or systems without compatible GPU
        sLog_App("GPU monitoring not available (Windows 10+ required or no compatible GPU)");
        m_gpuCounter = nullptr;
    } else {
        sLog_App("Windows GPU monitoring initialized (PDH API)");
    }

    // Initial query to prime the counters (first query returns 0)
    PdhCollectQueryData(static_cast<PDH_HQUERY>(m_pdhQuery));
}

void PerformanceMonitor::cleanupWindowsCounters() {
    if (m_pdhQuery) {
        PdhCloseQuery(static_cast<PDH_HQUERY>(m_pdhQuery));
        m_pdhQuery = nullptr;
        m_cpuCounter = nullptr;
        m_gpuCounter = nullptr;
    }
}
#endif
