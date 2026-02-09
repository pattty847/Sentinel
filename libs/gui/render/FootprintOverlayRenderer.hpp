// Render-thread footprint overlay module used by the chart renderer host.
#pragma once

#include <QByteArray>
#include <QImage>
#include <QRectF>
#include <atomic>
#include <vector>

class QQuickWindow;
class QSGNode;
class FootprintIntensityNode;

class FootprintOverlayRenderer {
public:
    struct PendingUpload {
        int x = 0;
        int gridWidth = 0;
        int gridHeight = 0;
        QByteArray data;
    };

    void onRootRebuilt();
    void requestNeutralReset();

    void render(QQuickWindow* window,
                QSGNode* parentNode,
                bool drawFootprint,
                bool forceFull,
                float timeOffset,
                const QRectF& drawRect,
                const QRectF& sharedSrcRect,
                int sharedGridWidth,
                int sharedGridHeight,
                std::vector<PendingUpload>& pendingUploads);

private:
    void ensureImage();

    FootprintIntensityNode* m_node = nullptr;
    int m_gridWidth = 5120;
    int m_gridHeight = 2048;
    bool m_textureDirty = true;
    QImage m_image;
    std::atomic<bool> m_resetPending{false};
};
