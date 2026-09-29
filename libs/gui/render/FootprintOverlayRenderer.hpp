// Render-thread footprint overlay module used by the chart renderer host.
#pragma once
#include "TradeOverlayMapping.hpp"

#include "IOverlayRenderer.hpp"

#include <QByteArray>
#include <QImage>
#include <QRectF>
#include <atomic>
#include <mutex>
#include <vector>

class QQuickWindow;
class QSGNode;
class FootprintIntensityNode;


class HeatmapColumnTexture;
class FootprintOverlayRenderer : public IOverlayRenderer {
public:
    struct PendingUpload {
        int x = 0;
        int gridWidth = 0;
        int gridHeight = 0;
        QByteArray data;
        TradeOverlayGrid grid;
    };

    /// Thread-safe enqueue (called from GUI thread).
    void enqueue(PendingUpload upload);
    /// Swap pending uploads into caller's vector (called from render thread).
    void drainPending(std::vector<PendingUpload>& out);
    /// Clear all pending uploads (called from GUI thread on clearData).
    void clearPending();

    void onRootRebuilt() override;
    int zOrder() const override { return 1; }
    void requestNeutralReset();

    void render(QQuickWindow* window,
                QSGNode* parentNode,
                bool drawFootprint,
                int64_t viewStartMs, int64_t viewEndMs,
                double viewMin, double viewMax, const QRectF& surface,
                std::vector<PendingUpload>& pendingUploads);

private:
    void ensureImage();

    FootprintIntensityNode* m_node = nullptr;
    int m_gridWidth = 5120;
    int m_gridHeight = 2048;
    int m_lastWriteColumn = -1;
    bool m_textureDirty = true;
    TradeOverlayGrid m_grid;
    QImage m_image;                                   // CPU mirror for full uploads
    HeatmapColumnTexture* m_columnTexture = nullptr;  // RHI path; owned by the node
    std::atomic<bool> m_resetPending{false};

    mutable std::mutex m_pendingMutex;
    std::vector<PendingUpload> m_pendingUploads;
};
