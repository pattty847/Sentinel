#include "RenderDiagnostics.hpp"
#include <QString>
#include <algorithm>

RenderDiagnostics::RenderDiagnostics() {
    m_metrics.frameTimes.reserve(MAX_FRAME_SAMPLES);
    m_metrics.frameTimer.start();
}

void RenderDiagnostics::startFrame() {
    m_metrics.frameTimer.restart();
    m_bytesUploadedThisFrame = 0;
}

void RenderDiagnostics::endFrame() {
    qint64 frameTime = m_metrics.frameTimer.nsecsElapsed() / 1000; // Convert to microseconds
    m_metrics.lastFrameTime_us = frameTime;
    
    // Keep rolling window of frame times
    m_metrics.frameTimes.push_back(frameTime);
    if (m_metrics.frameTimes.size() > MAX_FRAME_SAMPLES) {
        m_metrics.frameTimes.erase(m_metrics.frameTimes.begin());
    }
    
    // Add this frame's bytes to total
    m_totalBytesUploaded.fetch_add(m_bytesUploadedThisFrame);
}

void RenderDiagnostics::recordCacheHit() {
    m_metrics.cacheHits++;
}

void RenderDiagnostics::recordCacheMiss() {
    m_metrics.cacheMisses++;
}

void RenderDiagnostics::recordGeometryRebuild() {
    m_metrics.geometryRebuilds++;
}

void RenderDiagnostics::recordTransformApplied() {
    m_metrics.transformsApplied++;
}

void RenderDiagnostics::recordBytesUploaded(size_t bytes) {
    m_bytesUploadedThisFrame += bytes;
}

double RenderDiagnostics::getCurrentFPS() const {
    if (m_metrics.frameTimes.size() < 2) return 0.0;
    
    qint64 totalTime = 0;
    for (qint64 time : m_metrics.frameTimes) {
        totalTime += time;
    }
    
    return (m_metrics.frameTimes.size() - 1) * 1000000.0 / totalTime;  // Convert μs to FPS
}

double RenderDiagnostics::getAverageRenderTime() const {
    if (m_metrics.frameTimes.empty()) return 0.0;
    
    qint64 totalTime = 0;
    for (qint64 time : m_metrics.frameTimes) {
        totalTime += time;
    }
    
    return totalTime / (m_metrics.frameTimes.size() * 1000.0);  // Convert μs to ms
}

double RenderDiagnostics::getCacheHitRate() const {
    qint64 total = m_metrics.cacheHits + m_metrics.cacheMisses;
    return (total > 0) ? (m_metrics.cacheHits * 100.0 / total) : 0.0;
}

size_t RenderDiagnostics::getTotalBytesUploaded() const {
    return m_totalBytesUploaded.load();
}

QString RenderDiagnostics::getPerformanceStats() const {
    double fps = getCurrentFPS();
    double avgRenderTime = getAverageRenderTime();
    double cacheHitRate = getCacheHitRate();
    size_t totalBytes = getTotalBytesUploaded();
    
    return QString("FPS: %1 | Render: %2ms | Cache: %3% | Uploads: %4MB")
        .arg(fps, 0, 'f', 1)
        .arg(avgRenderTime, 0, 'f', 2)
        .arg(cacheHitRate, 0, 'f', 1)
        .arg(totalBytes / (1024.0 * 1024.0), 0, 'f', 2);
}