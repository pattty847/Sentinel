/*
 * Sentinel — HeatmapStreamService
 *
 * Owns the heatmap ring-buffer (HeatmapStreamState), presentation clock,
 * cadence tracker (TimeAuthority), and auto-scroll controller.
 * Provides column ingestion and render-tick logic.
 *
 * Threading: GUI thread only (same as UGR).  The owned objects are accessed
 * from the render thread via immutable snapshots — that contract is preserved.
 */
#pragma once

#include "HeatmapStreamState.hpp"
#include "TimeAuthority.hpp"
#include "ViewportAutoScrollController.hpp"

#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>
#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>

class GridViewState;
class HeatmapOverlayRenderer;

class HeatmapStreamService : public QObject {
    Q_OBJECT
public:
    // ── Event struct (moved from UnifiedGridRenderer) ────────────────────────
    struct HeatmapColumnEvent {
        int64_t sliceStartMs = 0;
        int64_t sliceEndMs = 0;
        int64_t timeframeMs = 0;
        double minPrice = 0.0;
        double maxPrice = 0.0;
        double tickSize = 0.0;
        QByteArray column;
        QByteArray liquidityColumn;
        double liquidityScale = 1.0;
        int intensityBytesPerCell = 1;
    };

    // ── Ingest result (returned to caller so it can emit Q_PROPERTY signals) ─
    struct IngestResult {
        bool accepted = false;
        bool tickSizeChanged = false;
        double newTickSize = 0.0;
        bool maxLiquidityChanged = false;
        double newMaxLiquidity = 0.0;
        bool minLiquidityChanged = false;
        double newMinLiquidity = 0.0;
        bool autoScrollApplied = false;
    };

    // ── Render-tick result ───────────────────────────────────────────────────
    struct RenderTickResult {
        bool shouldUpdate = false;
        bool autoScrollApplied = false;
    };

    // ── Range-reset result ───────────────────────────────────────────────────
    struct RangeResetResult {
        bool tickSizeChanged = false;
        double newTickSize = 0.0;
    };

    explicit HeatmapStreamService(QObject* parent = nullptr);
    ~HeatmapStreamService() override;

    // ── Lifecycle ────────────────────────────────────────────────────────────
    void init(int gridWidth, int gridHeight, int64_t timeframeMs, int intensityBytesPerCell);
    void ensureClockStarted();

    // ── Core state access (render path + debug) ──────────────────────────────
    HeatmapStreamState* stream() const { return m_stream.get(); }
    const TimeAuthority& timeAuthority() const { return m_timeAuthority; }
    TimeAuthority& timeAuthority() { return m_timeAuthority; }
    const QElapsedTimer& clock() const { return m_clock; }
    ViewportAutoScrollController* autoScrollController() const { return m_autoScrollController.get(); }
    int gridWidth() const { return m_gridWidth; }
    int gridHeight() const { return m_gridHeight; }
    int intensityBytesPerCell() const { return m_intensityBytesPerCell; }
    double tickSize() const { return m_tickSize; }
    double maxObservedLiquidity() const { return m_maxObservedLiquidity; }
    double minObservedLiquidity() const {
        return m_minObservedLiquidity == std::numeric_limits<double>::max()
                   ? 0.0
                   : m_minObservedLiquidity;
    }
    uint64_t streamGeneration() const { return m_streamGeneration.load(std::memory_order_acquire); }
    void incrementGeneration() { m_streamGeneration.fetch_add(1, std::memory_order_acq_rel); }

    // ── Column ingestion ─────────────────────────────────────────────────────
    IngestResult ingestColumn(const HeatmapColumnEvent& event,
                              GridViewState* viewState,
                              HeatmapOverlayRenderer& overlay,
                              int liquidityLabelMode,
                              int64_t currentTimeframeMs);

    // ── Render loop tick ─────────────────────────────────────────────────────
    RenderTickResult handleRenderTick(GridViewState* viewState);

    // ── Timeframe change ─────────────────────────────────────────────────────
    void handleTimeframeChange(int64_t timeframeMs);

    // ── Range reset ──────────────────────────────────────────────────────────
    RangeResetResult handleRangeReset(double minPrice, double maxPrice, double tickSize,
                                      int gridWidth, int gridHeight,
                                      GridViewState* viewState,
                                      HeatmapOverlayRenderer& overlay);

    // ── Auto-scroll configuration ────────────────────────────────────────────
    void setAutoScrollPaddingFrac(double frac);
    void setAutoScrollSmoothEnabled(bool enabled);
    void setInitialViewportPct(int pct);
    void setInitialPricePct(int pct);
    void resetAutoScrollSpan();
    void updateAutoScrollLag(GridViewState& vs, int64_t cadenceMs);

    // ── Grid dimensions ──────────────────────────────────────────────────────
    void setGridDimensions(int w, int h, HeatmapOverlayRenderer& overlay);

    // ── Texture rebuild from ring buffer ────────────────────────────────────
    void rebuildTextureFromRing(HeatmapOverlayRenderer& overlay,
                                double liquidityThreshold,
                                int liquidityLabelMode);

    // ── Fit viewport to full data range ─────────────────────────────────────
    bool fitToDataRange(GridViewState* viewState);

private:
    std::unique_ptr<HeatmapStreamState> m_stream;
    QElapsedTimer m_clock;
    TimeAuthority m_timeAuthority;
    std::unique_ptr<ViewportAutoScrollController> m_autoScrollController;
    bool m_viewportInitialized = false;
    int m_gridWidth = 5120;
    int m_gridHeight = 2048;
    int m_intensityBytesPerCell = 1;
    std::atomic<uint64_t> m_streamGeneration{0};
    double m_tickSize = 0.0;
    double m_maxObservedLiquidity = 0.0;
    double m_minObservedLiquidity = std::numeric_limits<double>::max();
};
