#include "TpoOverlayRenderer.hpp"

#include <QQuickWindow>
#include <QSGNode>

void TpoOverlayRenderer::onRootRebuilt() {}

void TpoOverlayRenderer::render(QQuickWindow* window,
                                QSGNode* parentNode,
                                bool drawTpo,
                                const QRectF& drawRect,
                                const QRectF& sourceRect,
                                std::vector<PendingUpload>& pendingUploads) {
    Q_UNUSED(window);
    Q_UNUSED(parentNode);
    Q_UNUSED(drawTpo);
    Q_UNUSED(drawRect);
    Q_UNUSED(sourceRect);
    pendingUploads.clear();
}

