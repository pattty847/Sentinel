/*
Sentinel — HeatmapLabelRenderer
*/
#include "HeatmapLabelRenderer.hpp"

#include <QPainter>
#include <QFont>

#include <algorithm>

void HeatmapLabelRenderer::buildFromSnapshot(const Request& request,
                                             const HeatmapStreamState::LabelSnapshot& labelSnapshot,
                                             bool dollars) {
    if (!request.valid || request.labelSize.isEmpty()) {
        return;
    }

    const auto& snapshot = labelSnapshot.snapshot;
    if (!snapshot.liquidityAvailable || snapshot.tickSize <= 0.0 || snapshot.gridSize <= 0) {
        return;
    }

    const auto& liquidityRing = labelSnapshot.liquidityRing;
    const auto& intensityRing = labelSnapshot.intensityRing;
    const auto& liquidityScales = labelSnapshot.liquidityScales;
    const size_t expectedSize = static_cast<size_t>(snapshot.gridSize) * snapshot.gridSize;
    if (liquidityRing.size() != expectedSize) {
        return;
    }

    const int extraWidth = static_cast<int>(std::ceil(request.cellW));
    QImage labelImage(QSize(request.labelSize.width() + extraWidth, request.labelSize.height()),
                      QImage::Format_ARGB32_Premultiplied);
    labelImage.fill(Qt::transparent);

    QPainter painter(&labelImage);
    QFont font("Monospace");
    font.setStyleHint(QFont::TypeWriter);
    font.setPixelSize(request.fontPx);
    painter.setFont(font);
    const QColor askColor(255, 255, 255, 230);
    const QColor bidColor(0, 0, 0, 210);

    const int gridSize = snapshot.gridSize;
    const int cellsX = static_cast<int>(std::ceil(request.srcRect.width())) + 1;
    const int cellsY = static_cast<int>(std::ceil(request.srcRect.height()));
    const bool haveIntensity = (intensityRing.size() == expectedSize);

    for (int j = 0; j < cellsY; ++j) {
        int texY = request.startY + j;
        if (texY < 0) {
            texY = gridSize + (texY % gridSize);
        }
        texY = texY % gridSize;
        if (texY < 0 || texY >= gridSize) {
            continue;
        }
        const double price = snapshot.maxPrice - (static_cast<double>(texY) * snapshot.tickSize);
        for (int i = 0; i < cellsX; ++i) {
            int texX = request.startX + i;
            if (texX < 0) {
                texX = gridSize + (texX % gridSize);
            }
            texX = texX % gridSize;
            if (texX < 0 || texX >= gridSize) {
                continue;
            }
            const size_t ringIndex = static_cast<size_t>(texY) * gridSize + texX;
            if (haveIntensity) {
                const uint8_t encoded = intensityRing[ringIndex];
                if (encoded == 0) {
                    continue;
                }
                painter.setPen(encoded >= 128 ? askColor : bidColor);
            } else {
                painter.setPen(askColor);
            }
            const uint16_t raw = liquidityRing[ringIndex];
            if (raw == 0) {
                continue;
            }
            const double scale = (liquidityScales.size() == static_cast<size_t>(gridSize))
                ? std::max(1e-12, liquidityScales[texX])
                : 1.0;
            double value = static_cast<double>(raw) * scale;
            if (dollars) {
                value *= price;
            }
            const QString label = formatLiquidityLabel(value, dollars);
            if (label.isEmpty()) {
                continue;
            }

            const float px = static_cast<float>(i) * request.cellW;
            const float py = static_cast<float>(j) * request.cellH;
            const QRectF cellRect(px, py, request.cellW, request.cellH);
            painter.drawText(cellRect, Qt::AlignCenter, label);
        }
    }
    painter.end();

    Result result;
    result.image = labelImage;
    result.startX = request.startX;
    result.startY = request.startY;
    result.pixelSize = request.labelSize;
    result.sourceSize = request.srcRect.size();
    result.fontBucket = request.fontBucket;
    result.viewportVersion = request.viewportVersion;
    result.valid = !labelImage.isNull();

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_result = result;
    }
    m_version.fetch_add(1);
}

HeatmapLabelRenderer::Result HeatmapLabelRenderer::result() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_result;
}

QString HeatmapLabelRenderer::formatLiquidityLabel(double value, bool dollars) {
    if (value <= 0.0) {
        return QString();
    }

    const double absValue = value;
    double divisor = 1.0;
    QString suffix;
    if (absValue >= 1.0e9) {
        divisor = 1.0e9;
        suffix = "B";
    } else if (absValue >= 1.0e6) {
        divisor = 1.0e6;
        suffix = "M";
    } else if (absValue >= 1.0e3) {
        divisor = 1.0e3;
        suffix = "k";
    }

    double scaled = absValue / divisor;
    QString number;
    if (divisor > 1.0) {
        if (scaled >= 10.0) {
            scaled = std::floor(scaled);
            number = QString::number(scaled, 'f', 0);
        } else {
            scaled = std::floor(scaled * 10.0) / 10.0;
            if (scaled < 0.1) {
                return QString();
            }
            number = QString::number(scaled, 'f', 1);
            if (number.endsWith(".0")) {
                number.chop(2);
            }
        }
    } else if (absValue < 1.0) {
        scaled = std::floor(absValue * 100.0) / 100.0;
        if (scaled < 0.01) {
            return QString();
        }
        number = QString::number(scaled, 'f', 2);
        while (number.endsWith('0')) {
            number.chop(1);
        }
        if (number.endsWith('.')) {
            number.chop(1);
        }
    } else if (absValue < 10.0) {
        scaled = std::floor(absValue * 10.0) / 10.0;
        number = QString::number(scaled, 'f', 1);
        if (number.endsWith(".0")) {
            number.chop(2);
        }
    } else {
        scaled = std::floor(absValue);
        number = QString::number(scaled, 'f', 0);
    }

    if (dollars) {
        return QString("$%1%2").arg(number, suffix);
    }
    return QString("%1%2").arg(number, suffix);
}
