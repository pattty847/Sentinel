/*
Sentinel — PerformanceMonitor
*/
#include "PerformanceMonitor.hpp"
#include "SentinelLogging.hpp"
#include <QTimer>
#include <QFile>
#include <QTextStream>
#include <QThread>
#include <QMouseEvent>
#include <chrono>

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

    m_cpuUpdateTimer = new QTimer(this);
    connect(m_cpuUpdateTimer, &QTimer::timeout, this, &PerformanceMonitor::updateCpuMetrics);
    m_cpuUpdateTimer->start(2000);

    m_frameUpdateTimer = new QTimer(this);
    connect(m_frameUpdateTimer, &QTimer::timeout, this, &PerformanceMonitor::refreshFrameStats);
    m_frameUpdateTimer->start(250);
    m_lastBandwidthMs = monotonicNs() / 1000000;
}

PerformanceMonitor::~PerformanceMonitor() {
#ifdef _WIN32
    cleanupWindowsCounters();
#endif
}

void PerformanceMonitor::attachToWindow(QQuickWindow* window) {
    if (!window || window == m_window) return;
    if (m_window) {
        m_window->removeEventFilter(this);
        disconnect(m_window, nullptr, this, nullptr);
    }
    m_window = window;
    {
        std::lock_guard lock(m_frameMutex);
        m_frameWindow.clear();
    }
    m_latestFrameStats = {};
    m_window->installEventFilter(this);
    connect(window, &QQuickWindow::beforeSynchronizing, this,
            &PerformanceMonitor::onRenderStart, Qt::DirectConnection);
    connect(window, &QQuickWindow::afterRendering, this,
            &PerformanceMonitor::onRenderEnd, Qt::DirectConnection);
}

int64_t PerformanceMonitor::monotonicNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void PerformanceMonitor::onRenderStart() {
    m_renderStartNs.store(monotonicNs(), std::memory_order_relaxed);
}

void PerformanceMonitor::onRenderEnd() {
    const int64_t endNs = monotonicNs();
    const int64_t startNs = m_renderStartNs.exchange(0, std::memory_order_relaxed);
    if (startNs <= 0 || endNs < startNs) return;
    const double durationMs = static_cast<double>(endNs - startNs) / 1000000.0;
    std::lock_guard lock(m_frameMutex);
    m_frameWindow.add(endNs / 1000000, durationMs);
}

bool PerformanceMonitor::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_window) {
        const auto type = event->type();
        if (type == QEvent::MouseButtonPress || type == QEvent::MouseButtonRelease ||
            type == QEvent::Wheel || type == QEvent::KeyPress ||
            type == QEvent::TouchBegin || type == QEvent::TouchUpdate ||
            (type == QEvent::MouseMove &&
             static_cast<QMouseEvent*>(event)->buttons() != Qt::NoButton)) {
            m_lastInputMs = monotonicNs() / 1000000;
        }
    }
    return QObject::eventFilter(watched, event);
}

QString PerformanceMonitor::frameStatsText() const {
    const auto& stats = m_latestFrameStats;
    if (!stats.window.samples) return QStringLiteral("Frame: -- | idle");
    return QStringLiteral("Frame p50/p95: %1/%2 ms | %3 renders/s%4")
        .arg(stats.window.p50Ms, 0, 'f', 1)
        .arg(stats.window.p95Ms, 0, 'f', 1)
        .arg(stats.window.samples)
        .arg(stats.idle ? QStringLiteral(" idle") : QString());
}

void PerformanceMonitor::refreshFrameStats() {
    const int64_t nowMs = monotonicNs() / 1000000;
    FrameStatsWindow snapshot;
    {
        std::lock_guard lock(m_frameMutex);
        snapshot = m_frameWindow;
    }
    m_latestFrameStats.window = snapshot.summarize(nowMs);
    m_latestFrameStats.idle = m_lastInputMs == 0 || nowMs - m_lastInputMs >= 1000;
    emit frameStatsChanged(frameStatsText());

    if (nowMs - m_lastBandwidthMs >= 1000) {
        const double elapsedSeconds = static_cast<double>(nowMs - m_lastBandwidthMs) / 1000.0;
        const qint64 uploadBytes = m_uploadBytesAccum.exchange(0, std::memory_order_relaxed);
        const double mbps = (static_cast<double>(uploadBytes) / (1024.0 * 1024.0)) / elapsedSeconds;
        m_uploadBandwidthMBps.store(mbps);
        emit uploadBandwidthChanged(mbps);
        m_lastBandwidthMs = nowMs;
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
    if (!m_pdhQuery) return;

    PDH_STATUS status = PdhCollectQueryData(static_cast<PDH_HQUERY>(m_pdhQuery));
    if (status != ERROR_SUCCESS) {
        return;
    }

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
    updateCpuUsage(0);
#endif
}

#ifdef _WIN32
void PerformanceMonitor::initWindowsCounters() {
    PDH_STATUS status;

    status = PdhOpenQuery(nullptr, 0, reinterpret_cast<PDH_HQUERY*>(&m_pdhQuery));
    if (status != ERROR_SUCCESS) {
        sLog_Warning("Failed to open PDH query, CPU/GPU monitoring disabled: status=0x"
                     << QString::number(static_cast<quint32>(status), 16));
        return;
    }

    status = PdhAddCounterW(
        static_cast<PDH_HQUERY>(m_pdhQuery),
        L"\\Processor(_Total)\\% Processor Time",
        0,
        reinterpret_cast<PDH_HCOUNTER*>(&m_cpuCounter)
    );
    if (status != ERROR_SUCCESS) {
        sLog_Warning("Failed to add PDH CPU counter: status=0x"
                     << QString::number(static_cast<quint32>(status), 16));
        m_cpuCounter = nullptr;
    } else {
        sLog_App("Windows CPU monitoring initialized (PDH API)");
    }

    status = PdhAddCounterW(
        static_cast<PDH_HQUERY>(m_pdhQuery),
        L"\\GPU Engine(*)\\Utilization Percentage",
        0,
        reinterpret_cast<PDH_HCOUNTER*>(&m_gpuCounter)
    );
    if (status != ERROR_SUCCESS) {
        sLog_App("GPU monitoring not available (Windows 10+ required or no compatible GPU): status=0x"
                 << QString::number(static_cast<quint32>(status), 16));
        m_gpuCounter = nullptr;
    } else {
        sLog_App("Windows GPU monitoring initialized (PDH API)");
    }

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
