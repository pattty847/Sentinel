#include "TimeAxisModel.hpp"
#include "../render/GridViewState.hpp"
#include <QDebug>
#include <cmath>
#include <algorithm>

// Define nice time steps (in milliseconds)
const std::vector<TimeAxisModel::TimeStep> TimeAxisModel::TIME_STEPS = {
    {100, "100ms"},       // 0.1 second
    {250, "250ms"},       // 0.25 second  
    {500, "500ms"},       // 0.5 second
    {1000, "1s"},         // 1 second
    {2000, "2s"},         // 2 seconds
    {5000, "5s"},         // 5 seconds
    {10000, "10s"},       // 10 seconds
    {15000, "15s"},       // 15 seconds
    {30000, "30s"},       // 30 seconds
    {60000, "1min"},      // 1 minute
    {120000, "2min"},     // 2 minutes
    {300000, "5min"},     // 5 minutes
    {600000, "10min"},    // 10 minutes
    {900000, "15min"},    // 15 minutes
    {1800000, "30min"},   // 30 minutes
    {3600000, "1h"},      // 1 hour
    {7200000, "2h"},      // 2 hours
    {14400000, "4h"},     // 4 hours
    {21600000, "6h"},     // 6 hours
    {43200000, "12h"},    // 12 hours
    {86400000, "1d"}      // 1 day
};

TimeAxisModel::TimeAxisModel(QObject* parent)
    : AxisModel(parent) {
}

void TimeAxisModel::recalculateTicks() {
    if (isViewportValid()) {
        beginResetModel();
        calculateTicks();
        endResetModel();
    }
}

void TimeAxisModel::calculateTicks() {
    clearTicks();
    
    if (!isViewportValid()) return;
    
    qint64 timeStart = static_cast<qint64>(getViewportStart());
    qint64 timeEnd = static_cast<qint64>(getViewportEnd());
    qint64 timeRange = timeEnd - timeStart;
    
    if (timeRange <= 0) return;
    
    // Target 6-12 ticks for good spacing
    int targetTicks = static_cast<int>(getViewportWidth() / 80.0); // ~80 pixels per tick
    targetTicks = std::max(4, std::min(15, targetTicks));
    
    qint64 step = calculateNiceTimeStep(timeRange, targetTicks);
    if (step <= 0) return;
    
    // Find first tick at or before timeStart
    qint64 firstTick = (timeStart / step) * step;
    
    // Generate ticks
    for (qint64 timestamp = firstTick; timestamp <= timeEnd + step; timestamp += step) {
        if (timestamp < timeStart - step) continue;
        
        double screenX = valueToScreenPosition(static_cast<double>(timestamp));
        
        // Check if tick is within visible area
        if (screenX >= 0 && screenX <= getViewportWidth()) {
            QString label = formatTimeLabel(timestamp, step);
            bool isMajor = true; // All time ticks are major for now
            
            addTick(static_cast<double>(timestamp), screenX, label, isMajor);
        }
    }
    
    qDebug() << "TimeAxisModel: Generated" << m_ticks.size() 
             << "time ticks for range" << timeRange << "ms, step=" << step << "ms";
}

QString TimeAxisModel::formatLabel(double value) const {
    qint64 timestampMs = static_cast<qint64>(value);
    
    // Use the range to determine appropriate formatting
    qint64 rangeMs = static_cast<qint64>(getViewportEnd() - getViewportStart());
    
    return formatTimeLabel(timestampMs, rangeMs);
}

double TimeAxisModel::getViewportStart() const {
    return m_viewState ? static_cast<double>(m_viewState->getVisibleTimeStart()) : 0.0;
}

double TimeAxisModel::getViewportEnd() const {
    return m_viewState ? static_cast<double>(m_viewState->getVisibleTimeEnd()) : 60000.0;
}

double TimeAxisModel::valueToScreenPosition(double value) const {
    if (!isViewportValid()) return 0.0;
    
    double timeStart = getViewportStart();
    double timeEnd = getViewportEnd();
    
    if (timeEnd <= timeStart) return 0.0;
    
    // Time axis is horizontal - earlier times at left
    double normalized = (value - timeStart) / (timeEnd - timeStart);
    
    return normalized * getViewportWidth();
}

qint64 TimeAxisModel::calculateNiceTimeStep(qint64 rangeMs, int targetTicks) const {
    if (rangeMs <= 0 || targetTicks <= 0) return 1000; // Default 1 second
    
    qint64 rawStep = rangeMs / targetTicks;
    
    // Find the best matching time step
    auto it = std::lower_bound(TIME_STEPS.begin(), TIME_STEPS.end(), rawStep,
        [](const TimeStep& step, qint64 value) {
            return step.milliseconds < value;
        });
    
    if (it == TIME_STEPS.end()) {
        // Use the largest step
        return TIME_STEPS.back().milliseconds;
    } else if (it == TIME_STEPS.begin()) {
        // Use the smallest step
        return TIME_STEPS.front().milliseconds;
    } else {
        // Choose between current and previous step based on which is closer
        auto prev = it - 1;
        qint64 currDiff = std::abs(it->milliseconds - rawStep);
        qint64 prevDiff = std::abs(prev->milliseconds - rawStep);
        
        return (currDiff < prevDiff) ? it->milliseconds : prev->milliseconds;
    }
}

QString TimeAxisModel::formatTimeLabel(qint64 timestampMs, qint64 stepMs) const {
    QDateTime dateTime = QDateTime::fromMSecsSinceEpoch(timestampMs);
    
    if (stepMs < 1000) {
        // Sub-second precision - show milliseconds
        return dateTime.toString("hh:mm:ss.zzz");
    } else if (stepMs < 60000) {
        // Second precision
        return dateTime.toString("hh:mm:ss");
    } else if (stepMs < 3600000) {
        // Minute precision
        return dateTime.toString("hh:mm");
    } else {
        // Hour precision or larger
        return dateTime.toString("hh:mm");
    }
}