#pragma once

#include <QByteArray>
#include <QRectF>
#include <vector>

class QQuickWindow;
class QSGNode;

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
                const QRectF& drawRect,
                const QRectF& sourceRect,
                std::vector<PendingUpload>& pendingUploads);
};

