#pragma once

#include "IOverlayRenderer.hpp"
#include "TpoStreamState.hpp"   // for TpoStreamState::DisplayMode

#include <QByteArray>
#include <QImage>
#include <QRectF>
#include <cstdint>
#include <mutex>
#include <vector>

class QQuickWindow;
class QSGNode;
class FootprintIntensityNode;

/*
 * TpoOverlayRenderer
 *
 * GPU-upload renderer for TPO profile data.  Supports two display modes:
 *
 *   HorizontalProfile (default / Mode A-adjacent):
 *     Texture columns are rank-indexed.  The profile shape is anchored
 *     to the left of the VP panel (right side of the chart area when
 *     combined with VolumeProfileRenderer, or the full drawRect otherwise).
 *     This mode is the "letter distribution" histogram where horizontal
 *     extent visualises time-at-price.
 *
 *   VerticalTimeline (Mode B):
 *     Texture columns are time-period-indexed.  The renderer clips the
 *     drawRect to [sessionStartMs, sessionEndMs] on the time axis,
 *     aligning letters with the underlying candle/heatmap series.
 *     Requires valid sessionStartMs / sessionEndMs in the snapshot plus
 *     a valid TimeAxisMapping (passed as extra parameters).
 */
class TpoOverlayRenderer : public IOverlayRenderer {
public:
    struct PendingUpload {
        int x = 0;
        int gridWidth = 0;
        int gridHeight = 0;
        QByteArray letters;
    };

    /// Thread-safe enqueue with session metadata (called from GUI thread).
    void enqueue(PendingUpload upload,
                 int64_t sessionStartMs,
                 int64_t sessionEndMs,
                 int64_t bracketMs,
                 int sessionColumns);
    /// Swap pending uploads and snapshot session metadata (called from render thread).
    void drainPending(std::vector<PendingUpload>& out,
                      int64_t& sessionStartMs,
                      int64_t& sessionEndMs,
                      int64_t& bracketMs,
                      int& sessionColumns);

    void onRootRebuilt() override;
    int zOrder() const override { return 2; }

    // Set display mode (determines how drawRect is computed).
    void setDisplayMode(TpoStreamState::DisplayMode mode);
    TpoStreamState::DisplayMode displayMode() const { return m_displayMode; }

    // drawRect      – heatmap overlap rect; used for HorizontalProfile draw area.
    // surfaceBounds – full item area (0,0,w,h); VerticalTimeline uses this for world→screen
    //                 projection so session columns align with candles regardless of ring state.
    void render(QQuickWindow* window,
                QSGNode* parentNode,
                bool drawTpo,
                bool forceFull,
                float timeOffset,
                const QRectF& drawRect,
                const QRectF& sourceRect,
                int sharedGridWidth,
                int sharedGridHeight,
                std::vector<PendingUpload>& pendingUploads,
                // VerticalTimeline extras (ignored in HorizontalProfile mode):
                int64_t sessionStartMs    = 0,
                int64_t sessionEndMs      = 0,
                int64_t viewStartMs       = 0,
                int64_t viewEndMs         = 0,
                QRectF  surfaceBounds     = QRectF());

private:
    void ensureImage();
    // Compute the clipped drawRect for VerticalTimeline mode.
    QRectF computeTimelinedDrawRect(const QRectF& fullDrawRect,
                                    int64_t sessionStartMs,
                                    int64_t sessionEndMs,
                                    int64_t viewStartMs,
                                    int64_t viewEndMs) const;

    FootprintIntensityNode* m_node = nullptr;
    int m_gridWidth = 5120;
    int m_gridHeight = 2048;
    int m_lastWriteColumn = -1;
    bool m_textureDirty = true;
    QImage m_image;
    TpoStreamState::DisplayMode m_displayMode =
        TpoStreamState::DisplayMode::VerticalTimeline;

    mutable std::mutex m_pendingMutex;
    std::vector<PendingUpload> m_pendingUploads;
    int64_t m_sessionStartMs = 0;
    int64_t m_sessionEndMs = 0;
    int64_t m_bracketMs = 0;
    int m_sessionColumns = 0;
};
