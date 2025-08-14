/*
Sentinel — RenderDiagnostics
Role: Collects, calculates, and displays real-time rendering performance metrics.
Inputs/Outputs: Takes frame event notifications; provides calculated stats and a visual overlay node.
Threading: All methods are designed to be called only on the Qt Quick render thread.
Performance: Calculations are lightweight to minimize impact on the render loop.
Integration: Owned by UnifiedGridRenderer and driven by the updatePaintNode V2 implementation.
Observability: This class is the primary observability tool for the rendering pipeline.
Related: RenderDiagnostics.cpp, UnifiedGridRenderer.h.
Assumptions: Assumes startFrame() and endFrame() are called consistently for each frame.
*/
#pragma once
#include <QElapsedTimer>
#include <vector>
#include <atomic>

class RenderDiagnostics {
public:
    RenderDiagnostics();
    
    void startFrame();
    void endFrame();
    
    void recordCacheHit();  
    void recordCacheMiss();
    void recordGeometryRebuild();
    void recordTransformApplied();
    void recordBytesUploaded(size_t bytes);
    
    double getCurrentFPS() const;
    double getAverageRenderTime() const;
    double getCacheHitRate() const;
    size_t getTotalBytesUploaded() const;
    
    bool isOverlayEnabled() const { return m_showOverlay; }
    void toggleOverlay() { m_showOverlay = !m_showOverlay; }
    
    QString getPerformanceStats() const;
    
private:
    struct PerformanceMetrics {
        QElapsedTimer frameTimer;
        std::vector<qint64> frameTimes;  // Last 60 frame times in microseconds
        qint64 lastFrameTime_us = 0;
        qint64 renderTime_us = 0;
        qint64 cacheHits = 0;
        qint64 cacheMisses = 0;
        qint64 geometryRebuilds = 0;
        qint64 transformsApplied = 0;
        size_t cachedCellCount = 0;
        bool showOverlay = false;
    };
    
    mutable PerformanceMetrics m_metrics;
    mutable size_t m_bytesUploadedThisFrame = 0;
    mutable std::atomic<size_t> m_totalBytesUploaded{0};
    bool m_showOverlay = false;
    
    static constexpr double PCIE_BUDGET_MB_PER_SECOND = 200.0;
    static constexpr size_t MAX_FRAME_SAMPLES = 60;
};