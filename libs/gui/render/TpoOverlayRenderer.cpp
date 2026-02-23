#include "TpoOverlayRenderer.hpp"

#include "FootprintIntensityNode.hpp"

#include "SentinelLogging.hpp"

#include <QColor>
#include <QElapsedTimer>
#include <QQuickWindow>
#include <QSGNode>
#include <QSGRendererInterface>
#include <QSGTexture>
#include <QtEndian>
#include <cmath>
#include <cstring>

void TpoOverlayRenderer::onRootRebuilt() {
    m_node = nullptr;
    m_lastWriteColumn = -1;
    m_textureDirty = true;
}

void TpoOverlayRenderer::render(QQuickWindow* window,
                                QSGNode* parentNode,
                                bool drawTpo,
                                bool forceFull,
                                float timeOffset,
                                const QRectF& drawRect,
                                const QRectF& sourceRect,
                                int sharedGridWidth,
                                int sharedGridHeight,
                                std::vector<PendingUpload>& pendingUploads) {
    if (!window || !parentNode) {
        return;
    }
    const auto* rendererInterface = window->rendererInterface();
    const bool useIncrementalGlUploads = rendererInterface &&
        rendererInterface->graphicsApi() == QSGRendererInterface::OpenGL;

    const bool hasPending = !pendingUploads.empty();
    if (qEnvironmentVariableIsSet("SENTINEL_CHART_DEBUG")) {
        static QElapsedTimer tpoDebugTimer;
        static bool tpoDebugTimerStarted = false;
        if (!tpoDebugTimerStarted) {
            tpoDebugTimer.start();
            tpoDebugTimerStarted = true;
        }
        if (tpoDebugTimer.elapsed() > 1000) {
            sLog_Debug(QString("TPO overlay: draw=%1 pending=%2 grid=%3x%4 drawRect[w=%5 h=%6] srcRect[w=%7 h=%8]")
                           .arg(drawTpo ? 1 : 0)
                           .arg(hasPending ? 1 : 0)
                           .arg(m_gridWidth)
                           .arg(m_gridHeight)
                           .arg(drawRect.width(), 0, 'f', 1)
                           .arg(drawRect.height(), 0, 'f', 1)
                           .arg(sourceRect.width(), 0, 'f', 1)
                           .arg(sourceRect.height(), 0, 'f', 1));
            tpoDebugTimer.restart();
        }
    }

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

    if (!drawTpo && !hasPending) {
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
            for (const auto& upload : pendingUploads) {
                if (upload.gridWidth != m_gridWidth || upload.gridHeight != m_gridHeight) {
                    continue;
                }
                if (upload.x < 0 || upload.x >= m_gridWidth || upload.letters.size() != m_gridHeight) {
                    continue;
                }
                for (int y = 0; y < m_gridHeight; ++y) {
                    const char mark = upload.letters.at(y);
                    const uint16_t encoded = (mark == '\0')
                        ? qToLittleEndian<uint16_t>(0x8000u)
                        : qToLittleEndian<uint16_t>(0xFFFFu);
                    auto* row = reinterpret_cast<uint16_t*>(m_image.scanLine(y));
                    row[upload.x] = encoded;
                }
            }
            m_textureDirty = true;
        }
    }

    const bool textureMissing = !m_node->hasTexture() || m_node->textureSize().isEmpty();
    const bool needTextureRefresh = m_textureDirty || (drawTpo && textureMissing);
    if (needTextureRefresh && m_gridWidth > 0 && m_gridHeight > 0) {
        ensureImage();
        if (!m_image.isNull()) {
            auto* tpoTexture = window->createTextureFromImage(m_image);
            if (!tpoTexture) {
                const QImage fallback = m_image.convertToFormat(QImage::Format_Grayscale16);
                tpoTexture = window->createTextureFromImage(fallback);
            }
            if (tpoTexture) {
                tpoTexture->setFiltering(QSGTexture::Nearest);
                m_node->setTexture(tpoTexture);
                m_textureDirty = false;
            } else {
                m_textureDirty = true;
            }
        } else {
            m_textureDirty = true;
        }
    }

    QRectF tpoSrcRect(0, 0, m_gridWidth, m_gridHeight);
    if (m_gridWidth == sharedGridWidth && m_gridHeight == sharedGridHeight) {
        tpoSrcRect = sourceRect;
    }

    m_node->setColor(QColor(42, 50, 60, 180));
    m_node->setBidColor(QColor(102, 180, 255, 255));
    m_node->setAskColor(QColor(255, 196, 80, 255));
    m_node->setNeutralFloor(0.01f);
    m_node->setMagnitudeScale(1.0f);
    m_node->setMagnitudeGamma(0.65f);

    float tpoTimeOffset = 0.0f;
    if (!forceFull && m_gridWidth > 0 && m_lastWriteColumn >= 0 && m_lastWriteColumn < m_gridWidth) {
        const float wrapped = timeOffset * static_cast<float>(m_gridWidth);
        const float fractional = wrapped - std::floor(wrapped);
        const int oldestColumn = (m_lastWriteColumn + 1) % m_gridWidth;
        tpoTimeOffset = (static_cast<float>(oldestColumn) + fractional) /
                        static_cast<float>(m_gridWidth);
    }
    m_node->setTimeOffset(tpoTimeOffset);

    if (drawTpo) {
        m_node->setRect(drawRect);
        m_node->setSourceRect(tpoSrcRect);
    } else {
        m_node->setRect(QRectF());
    }

    if (useIncrementalGlUploads && hasPending) {
        for (auto& upload : pendingUploads) {
            if (upload.gridWidth != m_gridWidth || upload.gridHeight != m_gridHeight) {
                continue;
            }
            if (upload.x < 0 || upload.x >= m_gridWidth || upload.letters.size() != m_gridHeight) {
                continue;
            }
            QByteArray encoded;
            encoded.resize(m_gridHeight * static_cast<int>(sizeof(uint16_t)));
            auto* dst = reinterpret_cast<uint16_t*>(encoded.data());
            for (int y = 0; y < m_gridHeight; ++y) {
                const char mark = upload.letters.at(y);
                dst[y] = qToLittleEndian<uint16_t>((mark == '\0') ? 0x8000u : 0xFFFFu);
            }
            m_node->enqueueColumn(upload.x, std::move(encoded));
        }
    }
}

void TpoOverlayRenderer::ensureImage() {
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

    for (int y = 0; y < m_image.height(); ++y) {
        auto* row = reinterpret_cast<uint16_t*>(m_image.scanLine(y));
        for (int x = 0; x < m_image.width(); ++x) {
            row[x] = qToLittleEndian<uint16_t>(0x8000u);
        }
    }
}

