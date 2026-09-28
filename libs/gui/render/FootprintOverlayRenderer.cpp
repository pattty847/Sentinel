#include "FootprintOverlayRenderer.hpp"
#include "HeatmapIntensityNode.hpp"

#include "FootprintIntensityNode.hpp"
#include "SentinelLogging.hpp"

#include <QColor>
#include <QSGNode>
#include <QSGRendererInterface>
#include <QSGTexture>
#include <QtEndian>
#include <cmath>
#include <cstring>

void FootprintOverlayRenderer::enqueue(PendingUpload upload) {
    std::lock_guard<std::mutex> lock(m_pendingMutex);
    m_pendingUploads.push_back(std::move(upload));
}

void FootprintOverlayRenderer::drainPending(std::vector<PendingUpload>& out) {
    std::lock_guard<std::mutex> lock(m_pendingMutex);
    if (!m_pendingUploads.empty()) {
        out.swap(m_pendingUploads);
    }
}

void FootprintOverlayRenderer::clearPending() {
    std::lock_guard<std::mutex> lock(m_pendingMutex);
    m_pendingUploads.clear();
}

void FootprintOverlayRenderer::onRootRebuilt() {
    m_node = nullptr;
    m_columnTexture = nullptr;  // owned and deleted by the old node
    m_lastWriteColumn = -1;
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
    const auto* rendererInterface = window->rendererInterface();
    const bool useIncrementalGlUploads = rendererInterface &&
        rendererInterface->graphicsApi() == QSGRendererInterface::OpenGL;

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
        for (const auto& upload : pendingUploads) {
            if (upload.gridWidth == m_gridWidth && upload.gridHeight == m_gridHeight &&
                upload.x >= 0 && upload.x < m_gridWidth) {
                m_lastWriteColumn = upload.x;
            }
        }
    }

    if ((m_gridWidth <= 0 || m_gridHeight <= 0) && sharedGridWidth > 0 && sharedGridHeight > 0) {
        m_gridWidth = sharedGridWidth;
        m_gridHeight = sharedGridHeight;
        m_textureDirty = true;
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

    if (!useIncrementalGlUploads && hasPending && m_gridWidth > 0 && m_gridHeight > 0) {
        ensureImage();
        if (!m_image.isNull()) {
            const int expectedBytes = m_gridHeight * static_cast<int>(sizeof(uint16_t));
            for (const auto& upload : pendingUploads) {
                if (upload.gridWidth != m_gridWidth || upload.gridHeight != m_gridHeight) {
                    continue;
                }
                if (upload.x < 0 || upload.x >= m_gridWidth || upload.data.size() != expectedBytes) {
                    sLog_RenderN(1000, "Footprint column upload dropped: x=" << upload.x
                                 << " bytes=" << upload.data.size() << " expected=" << expectedBytes
                                 << " grid=" << m_gridWidth << "x" << m_gridHeight);
                    continue;
                }
                const auto* src = reinterpret_cast<const uint8_t*>(upload.data.constData());
                for (int y = 0; y < m_gridHeight; ++y) {
                    auto* row = m_image.scanLine(y);
                    std::memcpy(row + upload.x * 2, src + y * 2, 2);
                }
                // RHI backends upload only the changed column (see HeatmapColumnTexture).
                if (m_columnTexture && !m_textureDirty) {
                    m_columnTexture->enqueueColumn(upload.x, upload.data);
                }
            }
            if (!m_columnTexture) {
                m_textureDirty = true;
            }
        }
    }

    const bool textureMissing = !m_node->hasTexture() || m_node->textureSize().isEmpty();
    const bool needTextureRefresh = m_textureDirty || (drawFootprint && textureMissing);

    if (needTextureRefresh && m_gridWidth > 0 && m_gridHeight > 0) {
        ensureImage();
        if (!m_image.isNull()) {
            QSGTexture* footprintTexture = nullptr;
            if (!useIncrementalGlUploads) {
                if (!m_columnTexture) {
                    m_columnTexture = new HeatmapColumnTexture();
                }
                m_columnTexture->setFull(m_image.size(), 2,
                                         QByteArray(reinterpret_cast<const char*>(m_image.constBits()),
                                                    static_cast<qsizetype>(m_image.sizeInBytes())),
                                         static_cast<int>(m_image.bytesPerLine()));
                footprintTexture = m_columnTexture;
            } else {
                footprintTexture = window->createTextureFromImage(m_image);
            }
            if (!footprintTexture) {
                const QImage fallback = m_image.convertToFormat(QImage::Format_Grayscale16);
                footprintTexture = window->createTextureFromImage(fallback);
            }
            if (footprintTexture) {
                footprintTexture->setFiltering(QSGTexture::Nearest);
                m_node->setTexture(footprintTexture);
                m_textureDirty = false;
            } else {
                sLog_RenderN(1000, "Footprint texture create failed (retry next frame): grid="
                             << m_gridWidth << "x" << m_gridHeight);
                m_textureDirty = true;
            }
        } else {
            sLog_RenderN(1000, "Footprint image alloc failed (retry next frame): grid="
                         << m_gridWidth << "x" << m_gridHeight);
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
    float footprintTimeOffset = 0.0f;
    if (!forceFull && m_gridWidth > 0 && m_lastWriteColumn >= 0 && m_lastWriteColumn < m_gridWidth) {
        const float wrapped = timeOffset * static_cast<float>(m_gridWidth);
        const float fractional = wrapped - std::floor(wrapped);
        const int oldestColumn = (m_lastWriteColumn + 1) % m_gridWidth;
        footprintTimeOffset = (static_cast<float>(oldestColumn) + fractional) /
                              static_cast<float>(m_gridWidth);
    }
    m_node->setTimeOffset(footprintTimeOffset);
    if (drawFootprint) {
        m_node->setRect(drawRect);
        m_node->setSourceRect(footprintSrcRect);
    } else {
        m_node->setRect(QRectF());
    }

    if (useIncrementalGlUploads && hasPending) {
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
