#include "FootprintOverlayRenderer.hpp"

#include "FootprintIntensityNode.hpp"

#include <QColor>
#include <QSGNode>
#include <QSGTexture>
#include <QtEndian>

void FootprintOverlayRenderer::onRootRebuilt() {
    m_node = nullptr;
    m_textureDirty = true;
}

void FootprintOverlayRenderer::requestNeutralReset() {
    m_resetPending.store(true, std::memory_order_release);
}

void FootprintOverlayRenderer::render(QQuickWindow* window,
                                      QSGNode* parentNode,
                                      bool drawFootprint,
                                      bool forceFull,
                                      float timeOffset,
                                      const QRectF& drawRect,
                                      const QRectF& sharedSrcRect,
                                      int sharedGridWidth,
                                      int sharedGridHeight,
                                      std::vector<PendingUpload>& pendingUploads) {
    if (!window || !parentNode) {
        return;
    }

    if (m_resetPending.exchange(false, std::memory_order_acq_rel)) {
        m_image = QImage();
        m_textureDirty = true;
        if (m_node) {
            m_node->setRect(QRectF());
        }
    }

    const bool hasPending = !pendingUploads.empty();
    if (hasPending) {
        for (auto it = pendingUploads.rbegin(); it != pendingUploads.rend(); ++it) {
            if (it->gridWidth > 0 && it->gridHeight > 0) {
                if (m_gridWidth != it->gridWidth || m_gridHeight != it->gridHeight) {
                    m_gridWidth = it->gridWidth;
                    m_gridHeight = it->gridHeight;
                    m_textureDirty = true;
                }
                break;
            }
        }
    }

    if (!drawFootprint && !hasPending) {
        if (m_node) {
            m_node->setRect(QRectF());
        }
        return;
    }

    if (!m_node) {
        m_node = new FootprintIntensityNode();
        parentNode->appendChildNode(m_node);
        m_textureDirty = true;
    }

    if (m_textureDirty && m_gridWidth > 0 && m_gridHeight > 0) {
        ensureImage();
        auto* footprintTexture = window->createTextureFromImage(m_image);
        if (!footprintTexture) {
            const QImage fallback = m_image.convertToFormat(QImage::Format_Grayscale16);
            footprintTexture = window->createTextureFromImage(fallback);
        }
        if (footprintTexture) {
            footprintTexture->setFiltering(QSGTexture::Nearest);
            m_node->setTexture(footprintTexture);
            m_textureDirty = false;
        } else {
            m_textureDirty = true;
        }
    }

    QRectF footprintSrcRect(0, 0, m_gridWidth, m_gridHeight);
    if (m_gridWidth == sharedGridWidth && m_gridHeight == sharedGridHeight) {
        footprintSrcRect = sharedSrcRect;
    }

    m_node->setColor(QColor(42, 50, 60, 180));
    m_node->setBidColor(QColor(46, 182, 230, 255));
    m_node->setAskColor(QColor(235, 92, 52, 255));
    m_node->setNeutralFloor(0.08f);
    m_node->setMagnitudeScale(20.0f);
    m_node->setMagnitudeGamma(0.75f);
    m_node->setTimeOffset(forceFull ? 0.0f : timeOffset);
    if (drawFootprint) {
        m_node->setRect(drawRect);
        m_node->setSourceRect(footprintSrcRect);
    } else {
        m_node->setRect(QRectF());
    }

    if (hasPending) {
        for (auto& upload : pendingUploads) {
            if (upload.gridWidth != m_gridWidth || upload.gridHeight != m_gridHeight) {
                continue;
            }
            m_node->enqueueColumn(upload.x, std::move(upload.data));
        }
    }
}

void FootprintOverlayRenderer::ensureImage() {
    if (!m_image.isNull() &&
        m_image.width() == m_gridWidth &&
        m_image.height() == m_gridHeight &&
        m_image.format() == QImage::Format_Grayscale16) {
        return;
    }

    m_image = QImage(m_gridWidth, m_gridHeight, QImage::Format_Grayscale16);
    if (m_image.isNull()) {
        return;
    }

    constexpr uint16_t kNeutralDelta = 0x8000u;
    for (int y = 0; y < m_image.height(); ++y) {
        auto* row = reinterpret_cast<uint16_t*>(m_image.scanLine(y));
        for (int x = 0; x < m_image.width(); ++x) {
            row[x] = qToLittleEndian<uint16_t>(kNeutralDelta);
        }
    }
}
