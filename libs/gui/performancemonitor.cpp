#include "performancemonitor.h"
#include <QApplication>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QDebug>
#include <algorithm>
#include <numeric>

// macOS-specific memory usage
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

PerformanceMonitor::PerformanceMonitor(QObject* parent)
    : QObject(parent)
    , m_updateTimer(new QTimer(this))
{
    // Reserve space for performance data
    m_frameTimes.reserve(MAX_SAMPLES);
    m_fpsHistory.reserve(MAX_SAMPLES);
    m_mouseLatencies.reserve(MAX_SAMPLES);
    m_memoryUsageHistory.reserve(MAX_SAMPLES);
    
    // Setup update timer for display refresh
    connect(m_updateTimer, &QTimer::timeout, this, &PerformanceMonitor::onUpdateTimer);
    m_updateTimer->start(250); // Update display 4 times per second
    
    qDebug() << "🎯 PerformanceMonitor initialized - Ready to track FPS, mouse latency, and memory usage";
}

void PerformanceMonitor::beginFrame() {
    m_frameStartTime = std::chrono::high_resolution_clock::now();
}

void PerformanceMonitor::endFrame() {
    if (m_frameStartTime.time_since_epoch().count() == 0) {
        return; // beginFrame() was never called
    }
    
    auto now = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(now - m_frameStartTime);
    double frameTimeMs = duration.count() / 1000.0; // Convert to milliseconds
    
    addSample(m_frameTimes, frameTimeMs);
    
    // Calculate FPS from frame time
    double fps = frameTimeMs > 0 ? 1000.0 / frameTimeMs : 0.0;
    addSample(m_fpsHistory, fps);
    
    emit fpsChanged(fps);
}

void PerformanceMonitor::beginMouseInteraction() {
    m_mouseTimer.start();
    m_mouseInteractionActive = true;
}

void PerformanceMonitor::endMouseInteraction() {
    if (m_mouseInteractionActive && m_mouseTimer.isValid()) {
        double latencyMs = m_mouseTimer.elapsed();
        addSample(m_mouseLatencies, latencyMs);
        m_mouseInteractionActive = false;
    }
}

void PerformanceMonitor::recordMemoryUsage() {
    size_t currentUsage = getMemoryUsage();
    m_memoryUsageHistory.push_back(currentUsage);
    
    if (m_memoryUsageHistory.size() > MAX_SAMPLES) {
        m_memoryUsageHistory.erase(m_memoryUsageHistory.begin());
    }
    
    if (currentUsage > m_peakMemoryUsage) {
        m_peakMemoryUsage = currentUsage;
    }
}

double PerformanceMonitor::getCurrentFPS() const {
    return m_fpsHistory.empty() ? 0.0 : m_fpsHistory.back();
}

double PerformanceMonitor::getAverageFrameTime() const {
    return calculateAverage(m_frameTimes);
}

double PerformanceMonitor::getLastMouseLatency() const {
    return m_mouseLatencies.empty() ? 0.0 : m_mouseLatencies.back();
}

double PerformanceMonitor::getAverageMouseLatency() const {
    return calculateAverage(m_mouseLatencies);
}

size_t PerformanceMonitor::getCurrentMemoryUsage() const {
    return getMemoryUsage();
}

size_t PerformanceMonitor::getPeakMemoryUsage() const {
    return m_peakMemoryUsage;
}

QWidget* PerformanceMonitor::createPerformanceWidget(QWidget* parent) {
    auto* widget = new QWidget(parent);
    auto* layout = new QVBoxLayout(widget);
    
    // Create performance display group
    auto* perfGroup = new QGroupBox("🎯 Performance Monitor", widget);
    auto* perfLayout = new QVBoxLayout(perfGroup);
    
    // FPS display
    auto* fpsLayout = new QHBoxLayout();
    m_fpsLabel = new QLabel("FPS: --");
    m_frameTimeLabel = new QLabel("Frame: --ms");
    fpsLayout->addWidget(m_fpsLabel);
    fpsLayout->addWidget(m_frameTimeLabel);
    perfLayout->addLayout(fpsLayout);
    
    // Mouse latency display
    m_mouseLatencyLabel = new QLabel("Mouse: --ms");
    perfLayout->addWidget(m_mouseLatencyLabel);
    
    // Memory usage display
    m_memoryLabel = new QLabel("Memory: --MB");
    perfLayout->addWidget(m_memoryLabel);
    
    // Status display
    m_statusLabel = new QLabel("Status: Initializing...");
    m_statusLabel->setStyleSheet("font-weight: bold;");
    perfLayout->addWidget(m_statusLabel);
    
    layout->addWidget(perfGroup);
    
    // Initial display update
    updateDisplay();
    
    return widget;
}

void PerformanceMonitor::updateDisplay() {
    if (!m_fpsLabel) return;
    
    // Update FPS display
    double currentFPS = getCurrentFPS();
    double avgFrameTime = getAverageFrameTime();
    
    QString fpsColor = currentFPS >= TARGET_FPS ? "green" : (currentFPS >= 30 ? "orange" : "red");
    m_fpsLabel->setText(QString("FPS: <span style='color: %1'>%2</span>")
                        .arg(fpsColor)
                        .arg(currentFPS, 0, 'f', 1));
    
    QString frameColor = avgFrameTime <= MAX_FRAME_TIME ? "green" : (avgFrameTime <= 33.33 ? "orange" : "red");
    m_frameTimeLabel->setText(QString("Frame: <span style='color: %1'>%2ms</span>")
                              .arg(frameColor)
                              .arg(avgFrameTime, 0, 'f', 1));
    
    // Update mouse latency display
    double avgMouseLatency = getAverageMouseLatency();
    QString mouseColor = avgMouseLatency <= MAX_MOUSE_LATENCY ? "green" : (avgMouseLatency <= 10 ? "orange" : "red");
    m_mouseLatencyLabel->setText(QString("Mouse: <span style='color: %1'>%2ms</span>")
                                 .arg(mouseColor)
                                 .arg(avgMouseLatency, 0, 'f', 1));
    
    // Update memory display
    size_t currentMem = getCurrentMemoryUsage();
    QString memoryText = QString("Memory: %1 (Peak: %2)")
                        .arg(formatMemorySize(currentMem))
                        .arg(formatMemorySize(m_peakMemoryUsage));
    m_memoryLabel->setText(memoryText);
    
    // Update status
    QString status = getPerformanceStatus();
    QString statusColor = isPerformanceGood() ? "green" : "red";
    m_statusLabel->setText(QString("Status: <span style='color: %1'>%2</span>")
                          .arg(statusColor)
                          .arg(status));
}

void PerformanceMonitor::onUpdateTimer() {
    recordMemoryUsage();
    updateDisplay();
}

bool PerformanceMonitor::isPerformanceGood() const {
    double currentFPS = getCurrentFPS();
    double avgFrameTime = getAverageFrameTime();
    double avgMouseLatency = getAverageMouseLatency();
    
    return currentFPS >= 30.0 && 
           avgFrameTime <= 33.33 && 
           avgMouseLatency <= 10.0;
}

QString PerformanceMonitor::getPerformanceStatus() const {
    if (m_fpsHistory.empty()) {
        return "Initializing...";
    }
    
    double currentFPS = getCurrentFPS();
    double avgFrameTime = getAverageFrameTime();
    
    if (currentFPS >= TARGET_FPS && avgFrameTime <= MAX_FRAME_TIME) {
        return "Excellent";
    } else if (currentFPS >= 30.0 && avgFrameTime <= 33.33) {
        return "Good";
    } else if (currentFPS >= 15.0) {
        return "Poor";
    } else {
        return "Critical";
    }
}

// Helper methods
double PerformanceMonitor::calculateAverage(const std::vector<double>& values) const {
    if (values.empty()) return 0.0;
    
    double sum = std::accumulate(values.begin(), values.end(), 0.0);
    return sum / values.size();
}

void PerformanceMonitor::addSample(std::vector<double>& container, double value) {
    container.push_back(value);
    if (container.size() > MAX_SAMPLES) {
        container.erase(container.begin());
    }
}

size_t PerformanceMonitor::getMemoryUsage() const {
#ifdef __APPLE__
    struct task_basic_info info;
    mach_msg_type_number_t size = TASK_BASIC_INFO_COUNT;
    kern_return_t kerr = task_info(mach_task_self(), TASK_BASIC_INFO, (task_info_t)&info, &size);
    
    if (kerr == KERN_SUCCESS) {
        return info.resident_size;
    }
    return 0;
#elif _WIN32
    PROCESS_MEMORY_COUNTERS_EX pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) {
        return pmc.WorkingSetSize;
    }
    return 0;
#else
    // Linux/Unix fallback
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        return usage.ru_maxrss * 1024; // Convert KB to bytes
    }
    return 0;
#endif
}

QString PerformanceMonitor::formatMemorySize(size_t bytes) const {
    if (bytes < 1024) {
        return QString("%1 B").arg(bytes);
    } else if (bytes < 1024 * 1024) {
        return QString("%1 KB").arg(bytes / 1024.0, 0, 'f', 1);
    } else if (bytes < 1024 * 1024 * 1024) {
        return QString("%1 MB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 1);
    } else {
        return QString("%1 GB").arg(bytes / (1024.0 * 1024.0 * 1024.0), 0, 'f', 1);
    }
} 