#include "HeatmapOverlayRenderer.hpp"

#include "HeatmapIntensityNode.hpp"
#include "SentinelLogging.hpp"

#include <QSGRendererInterface>
#include <QSGTexture>
#include <algorithm>
#include <cmath>
#include <cstring>

QColor HeatmapOverlayRenderer::ColorGradient::interpolate(float t) const {
    if (stops.empty()) {
        return QColor(0, 0, 0);
    }
    if (stops.size() == 1 || t <= stops.front().position) {
        return stops.front().color;
    }
    if (t >= stops.back().position) {
        return stops.back().color;
    }

    for (size_t i = 0; i < stops.size() - 1; ++i) {
        if (t >= stops[i].position && t <= stops[i + 1].position) {
            const float localT = (t - stops[i].position) / (stops[i + 1].position - stops[i].position);
            const QColor& c0 = stops[i].color;
            const QColor& c1 = stops[i + 1].color;
            return QColor(c0.red() + (c1.red() - c0.red()) * localT,
                          c0.green() + (c1.green() - c0.green()) * localT,
                          c0.blue() + (c1.blue() - c0.blue()) * localT);
        }
    }
    return stops.back().color;
}

void HeatmapOverlayRenderer::setGridDimensions(int width, int height) {
    if (width <= 0 || height <= 0) {
        return;
    }
    if (m_gridWidth == width && m_gridHeight == height) {
        return;
    }
    m_gridWidth = width;
    m_gridHeight = height;
    m_textureDirty = true;
}

void HeatmapOverlayRenderer::setIntensityBytesPerCell(int bytesPerCell) {
    if (bytesPerCell != 1 && bytesPerCell != 2) {
        return;
    }
    if (m_intensityBytesPerCell == bytesPerCell) {
        return;
    }
    m_intensityBytesPerCell = bytesPerCell;
    m_textureDirty = true;
}

void HeatmapOverlayRenderer::setBackgroundColor(const QColor& color) {
    if (m_backgroundColor == color) {
        return;
    }
    m_backgroundColor = color;
    m_textureDirty = true;
}

void HeatmapOverlayRenderer::setPaletteGamma(double gamma) {
    if (m_paletteGamma == gamma) {
        return;
    }
    m_paletteGamma = gamma;
    m_paletteDirty = true;
    m_textureDirty = true;
}

void HeatmapOverlayRenderer::setBidGradient(const std::vector<ColorStop>& stops) {
    m_bidGradient.stops = stops;
    m_paletteDirty = true;
    m_textureDirty = true;
}

void HeatmapOverlayRenderer::setAskGradient(const std::vector<ColorStop>& stops) {
    m_askGradient.stops = stops;
    m_paletteDirty = true;
    m_textureDirty = true;
}

void HeatmapOverlayRenderer::requestFullTextureRebuild() {
    m_rebuildPending.store(true, std::memory_order_release);
}

void HeatmapOverlayRenderer::onRootRebuilt() {
    m_textureDirty = true;
}

void HeatmapOverlayRenderer::applyToNode(QQuickWindow* window,
                                         HeatmapIntensityNode* node,
                                         bool drawHeatmap,
                                         float gamma,
                                         float contrast,
                                         float shaderFloor,
                                         bool forceFull,
                                         float timeOffset,
                                         const QRectF& drawRect,
                                         const QRectF& srcRect,
                                         std::vector<PendingUpload>& pendingUploads) {
    if (!window || !node) {
        return;
    }
    const auto* rendererInterface = window->rendererInterface();
    const bool useIncrementalGlUploads = rendererInterface &&
        rendererInterface->graphicsApi() == QSGRendererInterface::OpenGL;

    if (m_bidGradient.stops.empty()) {
        // Electric cyan — dark teal → mid cyan → bright cyan → white-hot (defaults)
        m_bidGradient.stops = {
            {0.00f, QColor(  0,  20,  25)},
            {0.35f, QColor(  0, 110, 130)},
            {0.70f, QColor(  0, 210, 220)},
            {1.00f, QColor(160, 255, 248)},
        };
        m_paletteDirty = true;
    }
    if (m_askGradient.stops.empty()) {
        // Hot orange — dark red → orange-red → hot orange → white-hot (defaults)
        m_askGradient.stops = {
            {0.00f, QColor( 35,   5,   0)},
            {0.30f, QColor(160,  30,  10)},
            {0.60f, QColor(230,  80,   0)},
            {0.85f, QColor(255, 160,  30)},
            {1.00f, QColor(255, 230,  80)},
        };
        m_paletteDirty = true;
    }

    if (m_rebuildPending.exchange(false, std::memory_order_acq_rel)) {
        m_heatmapImage = QImage();
        m_paletteImage = QImage();
        m_textureDirty = true;
        m_paletteDirty = true;
    }

    if (!useIncrementalGlUploads && !pendingUploads.empty()) {
        ensureHeatmapImage();
        if (!m_heatmapImage.isNull()) {
            const int width = m_heatmapImage.width();
            const int height = m_heatmapImage.height();
            const int bytesPerCell = m_intensityBytesPerCell;
            for (const auto& upload : pendingUploads) {
                if (upload.x < 0 || upload.x >= width || upload.data.size() != height * bytesPerCell) {
                    continue;
                }
                if (bytesPerCell == 1) {
                    const auto* src = reinterpret_cast<const uint8_t*>(upload.data.constData());
                    for (int y = 0; y < height; ++y) {
                        auto* row = m_heatmapImage.scanLine(y);
                        row[upload.x] = src[y];
                    }
                } else if (bytesPerCell == 2) {
                    const auto* src = reinterpret_cast<const uint8_t*>(upload.data.constData());
                    for (int y = 0; y < height; ++y) {
                        auto* row = m_heatmapImage.scanLine(y);
                        std::memcpy(row + upload.x * 2, src + y * 2, 2);
                    }
                }
            }
            m_textureDirty = true;
        }
    }

    if (m_textureDirty) {
        ensureHeatmapImage();
        ensurePaletteImage();
        auto* intensityTexture = window->createTextureFromImage(m_heatmapImage);
        if (!intensityTexture) {
            const QImage::Format fallbackFormat =
                (m_intensityBytesPerCell == 2) ? QImage::Format_Grayscale16 : QImage::Format_Grayscale8;
            QImage fallback = m_heatmapImage.convertToFormat(fallbackFormat);
            intensityTexture = window->createTextureFromImage(fallback);
        }
        auto* paletteTexture = window->createTextureFromImage(m_paletteImage);
        if (!intensityTexture || !paletteTexture) {
            delete intensityTexture;
            delete paletteTexture;
        } else {
            intensityTexture->setFiltering(QSGTexture::Nearest);
            paletteTexture->setFiltering(QSGTexture::Linear);
            node->setTextures(intensityTexture, paletteTexture);
            m_textureDirty = false;
        }
    }

    node->setGamma(gamma);
    node->setContrast(contrast);
    node->setShaderFloor(shaderFloor);
    node->setTimeOffset(forceFull ? 0.0f : timeOffset);
    if (drawHeatmap) {
        node->setRect(drawRect);
        node->setSourceRect(srcRect);
    } else {
        node->setRect(QRectF());
        node->setSourceRect(QRectF());
    }

    if (useIncrementalGlUploads) {
        for (auto& upload : pendingUploads) {
            node->enqueueColumn(upload.x, std::move(upload.data));
        }
    }

}

void HeatmapOverlayRenderer::ensureHeatmapImage() {
    if (!m_heatmapImage.isNull() && m_heatmapImage.width() == m_gridWidth &&
        m_heatmapImage.height() == m_gridHeight) {
        const QImage::Format expectedFormat =
            (m_intensityBytesPerCell == 2) ? QImage::Format_Grayscale16 : QImage::Format_Grayscale8;
        if (m_heatmapImage.format() == expectedFormat) {
            return;
        }
    }

    const QImage::Format format =
        (m_intensityBytesPerCell == 2) ? QImage::Format_Grayscale16 : QImage::Format_Grayscale8;
    m_heatmapImage = QImage(m_gridWidth, m_gridHeight, format);
    if (m_heatmapImage.isNull()) {
        return;
    }
    m_heatmapImage.fill(m_backgroundColor);
}

void HeatmapOverlayRenderer::ensurePaletteImage() {
    if (!m_paletteImage.isNull() && !m_paletteDirty) {
        return;
    }

    const int width = 512;
    m_paletteImage = QImage(width, 1, QImage::Format_ARGB32);
    if (m_paletteImage.isNull()) {
        return;
    }

    auto* row = reinterpret_cast<QRgb*>(m_paletteImage.scanLine(0));
    const float gamma = static_cast<float>(m_paletteGamma);
    for (int i = 0; i < width; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(width - 1);
        const bool isAsk = (i >= width / 2);
        const float localT = isAsk ? (t - 0.5f) * 2.0f : t * 2.0f;
        const float x = std::clamp(localT, 0.0f, 1.0f);
        const float curve = std::pow(x, gamma);
        const QColor color = isAsk ? m_askGradient.interpolate(curve) : m_bidGradient.interpolate(curve);
        row[i] = qRgba(color.red(), color.green(), color.blue(), 255);
    }

    m_paletteDirty = false;
    sLog_Render("Heatmap palette regenerated with gamma=" << gamma
                << " bid_stops=" << m_bidGradient.stops.size()
                << " ask_stops=" << m_askGradient.stops.size());
}
