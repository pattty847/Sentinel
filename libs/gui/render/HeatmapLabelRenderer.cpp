/*
Sentinel — HeatmapLabelRenderer
*/
#include "HeatmapLabelRenderer.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
bool useBlackLabel(uint16_t encoded) {
    if (encoded == 0) {
        return false;
    }
    const float magnitude = (encoded >= 0x8000u)
        ? (static_cast<float>(encoded - 0x8000u) / 32767.0f)
        : (static_cast<float>(encoded) / 32767.0f);
    return magnitude > 0.5f;
}
} // namespace

void HeatmapLabelRenderer::buildLabelQuads(const TimeAxisMapping& mapping,
                                           const HeatmapStreamState::Snapshot& snapshot,
                                           const MsdfAtlas& atlas,
                                           const std::vector<uint16_t>& liquidityRing,
                                           const std::vector<uint16_t>& intensityRing,
                                           const std::vector<double>& liquidityScales,
                                           float scale,
                                           bool dollars,
                                           std::vector<GlyphQuad>& whiteQuads,
                                           std::vector<GlyphQuad>& blackQuads,
                                           int onlyColumn) {
    whiteQuads.clear();
    blackQuads.clear();

    if (!mapping.valid || !atlas.isBuilt() ||
        snapshot.gridWidth <= 0 || snapshot.gridHeight <= 0 || snapshot.tickSize <= 0.0) {
        return;
    }

    const int gridWidth = snapshot.gridWidth;
    const int gridHeight = snapshot.gridHeight;
    const size_t expectedSize = static_cast<size_t>(gridWidth) * gridHeight;
    if (liquidityRing.size() != expectedSize) {
        return;
    }
    const bool haveIntensity = (intensityRing.size() == expectedSize);
    const bool haveScales = (liquidityScales.size() == static_cast<size_t>(gridWidth));

    // Derive iteration range from mapping srcRect (same as old code).
    // timeOffset is used ONLY for ring data lookup, NOT for screen positioning.
    const QRectF& srcRect = mapping.srcRect;
    [[maybe_unused]] const QRectF& drawRect = mapping.drawRect;
    [[maybe_unused]] const float cellW = static_cast<float>(mapping.cellW);
    [[maybe_unused]] const float cellH = static_cast<float>(mapping.cellH);

    const int cellsX = static_cast<int>(std::ceil(srcRect.width())) + 1;
    const int cellsY = static_cast<int>(std::ceil(srcRect.height()));

    // Ring offset: convert logical srcRect column to physical ring column.
    // timeOffset represents the physical ring offset for the GPU shader.
    const float baseX = static_cast<float>(srcRect.x()) + (mapping.timeOffset * gridWidth);
    const int startX = static_cast<int>(std::floor(baseX));
    [[maybe_unused]] const float fracX = baseX - static_cast<float>(startX);

    const float baseY = static_cast<float>(srcRect.y());
    const int startY = static_cast<int>(std::floor(baseY));
    [[maybe_unused]] const float fracY = baseY - static_cast<float>(startY);

    int onlyTexX = -1;
    int onlyI = -1;
    if (onlyColumn >= 0) {
        onlyTexX = onlyColumn % gridWidth;
        if (onlyTexX < 0) {
            onlyTexX += gridWidth;
        }
        onlyI = onlyTexX - startX;
        if (onlyI < 0) {
            onlyI += gridWidth;
        }
        if (onlyI >= gridWidth) {
            onlyI -= gridWidth;
        }
        if (onlyI < 0 || onlyI >= cellsX) {
            return;
        }
    }

    const float padding = static_cast<float>(atlas.paddingPx());
    for (int j = 0; j < cellsY; ++j) {
        int texY = startY + j;
        if (texY < 0) {
            texY = gridHeight + (texY % gridHeight);
        }
        texY = texY % gridHeight;
        if (texY < 0 || texY >= gridHeight) {
            continue;
        }
        const double price = snapshot.maxPrice - (static_cast<double>(texY) * snapshot.tickSize);
        const int iStart = (onlyI >= 0) ? onlyI : 0;
        const int iEnd = (onlyI >= 0) ? (onlyI + 1) : cellsX;
        for (int i = iStart; i < iEnd; ++i) {
            const int texX = (onlyI >= 0) ? onlyTexX : ((startX + i) % gridWidth + gridWidth) % gridWidth;
            if (texX < 0 || texX >= gridWidth) {
                continue;
            }
            const size_t ringIndex = static_cast<size_t>(texY) * gridWidth + texX;
            const uint16_t raw = liquidityRing[ringIndex];
            if (raw == 0) {
                continue;
            }

            const double scaleValue = haveScales
                ? std::max(1e-12, liquidityScales[texX])
                : 1.0;
            double value = static_cast<double>(raw) * scaleValue;
            if (dollars) {
                value *= price;
            }
            const QString label = formatLiquidityLabel(value, dollars);
            if (label.isEmpty()) {
                continue;
            }

            uint16_t encoded = 0xFFFFu;
            if (haveIntensity) {
                encoded = intensityRing[ringIndex];
                if (encoded == 0) {
                    continue;
                }
            }

            float penX = 0.0f;
            float minX = std::numeric_limits<float>::max();
            float maxX = std::numeric_limits<float>::lowest();
            float minY = std::numeric_limits<float>::max();
            float maxY = std::numeric_limits<float>::lowest();
            for (const QChar c : label) {
                const auto& glyph = atlas.glyph(c);
                if (glyph.advance <= 0.0f || glyph.uv.isNull()) {
                    continue;
                }
                minX = std::min(minX, penX + static_cast<float>(glyph.bounds.left()) - padding);
                maxX = std::max(maxX, penX + static_cast<float>(glyph.bounds.right()) + padding);
                minY = std::min(minY, static_cast<float>(glyph.bounds.top()) - padding);
                maxY = std::max(maxY, static_cast<float>(glyph.bounds.bottom()) + padding);
                penX += glyph.advance;
            }
            if (!std::isfinite(minX) || !std::isfinite(maxX)) {
                continue;
            }

            // Screen positioning via TimeAxisMapping — no timeOffset in screen math.
            // Cell center: world time for this texture column's center, and price for this row.
            const double bucketTimeMs = mapping.dataStartMs + (static_cast<double>(texX) * mapping.appendMs) + (mapping.appendMs * 0.5);
            const float centerX = static_cast<float>(mapping.timeToScreenX(bucketTimeMs));
            const float centerY = static_cast<float>(mapping.priceToScreenY(price));
            const float originX = centerX - (minX + maxX) * 0.5f * scale;
            const float originY = centerY - (minY + maxY) * 0.5f * scale;

            penX = 0.0f;
            for (const QChar c : label) {
                const auto& glyph = atlas.glyph(c);
                if (glyph.advance <= 0.0f || glyph.uv.isNull()) {
                    penX += glyph.advance;
                    continue;
                }

                const float x0 = originX + (penX + glyph.bounds.left() - padding) * scale;
                const float y0 = originY + (glyph.bounds.top() - padding) * scale;
                const float x1 = originX + (penX + glyph.bounds.right() + padding) * scale;
                const float y1 = originY + (glyph.bounds.bottom() + padding) * scale;

                const float u0 = static_cast<float>(glyph.uv.left());
                const float v0 = static_cast<float>(glyph.uv.top());
                const float u1 = static_cast<float>(glyph.uv.right());
                const float v1 = static_cast<float>(glyph.uv.bottom());

                GlyphQuad quad;
                quad.pos[0] = QVector2D(x0, y0);
                quad.pos[1] = QVector2D(x0, y1);
                quad.pos[2] = QVector2D(x1, y0);
                quad.pos[3] = QVector2D(x1, y0);
                quad.pos[4] = QVector2D(x0, y1);
                quad.pos[5] = QVector2D(x1, y1);

                quad.uv[0] = QVector2D(u0, v0);
                quad.uv[1] = QVector2D(u0, v1);
                quad.uv[2] = QVector2D(u1, v0);
                quad.uv[3] = QVector2D(u1, v0);
                quad.uv[4] = QVector2D(u0, v1);
                quad.uv[5] = QVector2D(u1, v1);

                if (haveIntensity && useBlackLabel(encoded)) {
                    blackQuads.push_back(quad);
                } else {
                    whiteQuads.push_back(quad);
                }
                penX += glyph.advance;
            }
        }
    }
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

    return QString("%1%2").arg(number, suffix);
}
