#pragma once

#include <QByteArray>
#include <QImage>
#include <QRectF>
#include <vector>

class QQuickWindow;
class QSGNode;
class FootprintIntensityNode;

// Skeleton renderer hook for upcoming TPO profile layer.
class TpoOverlayRenderer {
public:
    struct PendingUpload {
        int x = 0;
        int gridWidth = 0;
        int gridHeight = 0;
        QByteArray letters;
    };

    void onRootRebuilt();
    void render(QQuickWindow* window,
                QSGNode* parentNode,
                bool drawTpo,
                bool forceFull,
                float timeOffset,
                const QRectF& drawRect,
                const QRectF& sourceRect,
                int sharedGridWidth,
                int sharedGridHeight,
                std::vector<PendingUpload>& pendingUploads);

private:
    void ensureImage();

    FootprintIntensityNode* m_node = nullptr;
    int m_gridWidth = 5120;
    int m_gridHeight = 2048;
    int m_lastWriteColumn = -1;
    bool m_textureDirty = true;
    QImage m_image;
};

