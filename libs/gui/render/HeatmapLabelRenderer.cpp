/*
Sentinel — HeatmapLabelRenderer
*/
#include "HeatmapLabelRenderer.hpp"
#include "HeatmapRowGrouping.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <QtGlobal>


void HeatmapLabelRenderer::buildLabelGlyphs(const TimeAxisMapping& mapping,
                                            const HeatmapStreamState::Snapshot& snapshot,
                                            const ChartTextAtlas& atlas,
                                            const std::vector<uint16_t>& liquidityRing,
                                            const std::vector<uint16_t>& intensityRing,
                                            const std::vector<double>& liquidityScales,
                                            float scale,
                                            bool dollars,
                                            std::vector<ChartGlyphInstance>& glyphs,
                                            int onlyColumn,
                                            int rowGroup,
                                            int rowPhase) {
    glyphs.clear();

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

    const QRectF& srcRect = mapping.srcRect;
    const int cellsX = static_cast<int>(std::ceil(srcRect.width())) + 1;
    const int cellsY = static_cast<int>(std::ceil(srcRect.height()));

    const float baseX = static_cast<float>(srcRect.x()) + (mapping.timeOffset * gridWidth);
    const int startX = static_cast<int>(std::floor(baseX));
    const float baseY = static_cast<float>(srcRect.y());
    const int startY = static_cast<int>(std::floor(baseY));

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

    // Display tick: one label per group of rowGroup base rows, showing the
    // group's total liquidity (HeatmapRowGrouping.hpp).
    const int group = std::max(1, rowGroup);
    int lastGroupFirst = std::numeric_limits<int>::min();
    for (int j = 0; j < cellsY; ++j) {
        const int texY = startY + j;
        if (texY < 0 || texY >= gridHeight) {
            continue;
        }
        const int groupFirst = heatmap_rows::groupFirstRow(texY, group, rowPhase);
        if (groupFirst == lastGroupFirst) {
            continue;
        }
        lastGroupFirst = groupFirst;
        const int rowLo = std::max(groupFirst, 0);
        const int rowHi = std::min(groupFirst + group - 1, gridHeight - 1);
        const double price = snapshot.maxPrice - (static_cast<double>(groupFirst) * snapshot.tickSize);
        const int iStart = (onlyI >= 0) ? onlyI : 0;
        const int iEnd = (onlyI >= 0) ? (onlyI + 1) : cellsX;
        for (int i = iStart; i < iEnd; ++i) {
            const int texX = (onlyI >= 0) ? onlyTexX : ((startX + i) % gridWidth + gridWidth) % gridWidth;
            if (texX < 0 || texX >= gridWidth) {
                continue;
            }
            const double scaleValue = haveScales
                ? std::max(1e-12, liquidityScales[texX])
                : 1.0;
            double value = 0.0;
            for (int row = rowLo; row <= rowHi; ++row) {
                const size_t ringIndex = static_cast<size_t>(row) * gridWidth + texX;
                const uint16_t raw = liquidityRing[ringIndex];
                // Intensity 0 means the level was filtered out (below threshold).
                if (raw == 0 || (haveIntensity && intensityRing[ringIndex] == 0)) {
                    continue;
                }
                double rowValue = static_cast<double>(raw) * scaleValue;
                if (dollars) {
                    rowValue *= snapshot.maxPrice - static_cast<double>(row) * snapshot.tickSize;
                }
                value += rowValue;
            }
            if (value <= 0.0) {
                continue;
            }
            const QString label = formatLiquidityLabel(value, dollars);
            if (label.isEmpty()) {
                continue;
            }

            float penX = 0.0f;
            float minX = std::numeric_limits<float>::max();
            float maxX = std::numeric_limits<float>::lowest();
            float minY = std::numeric_limits<float>::max();
            float maxY = std::numeric_limits<float>::lowest();
            for (const QChar c : label) {
                const auto& glyph = atlas.glyph(c);
                if (glyph.advance <= 0.0f || glyph.uv.isNull()) {
                    penX += glyph.advance;
                    continue;
                }
                minX = std::min(minX, penX + static_cast<float>(glyph.bounds.left()));
                maxX = std::max(maxX, penX + static_cast<float>(glyph.bounds.right()));
                minY = std::min(minY, static_cast<float>(glyph.bounds.top()));
                maxY = std::max(maxY, static_cast<float>(glyph.bounds.bottom()));
                penX += glyph.advance;
            }
            if (!std::isfinite(minX) || !std::isfinite(maxX)) {
                continue;
            }

            const double cellW = mapping.drawRect.width() / mapping.srcRect.width();
            const double cellH = group * mapping.drawRect.height() / mapping.srcRect.height();
            const double colFracOffset = static_cast<double>(baseX) - static_cast<double>(startX);
            
            const float centerX = static_cast<float>(mapping.drawRect.x() + (static_cast<double>(i) - colFracOffset + 0.5) * cellW);
            const float centerY = static_cast<float>(mapping.priceToScreenY(price) + cellH * 0.5);
            
            const float originX = centerX - (minX + maxX) * 0.5f * scale;
            const float originY = centerY - (minY + maxY) * 0.5f * scale;
            
            if (centerY < mapping.drawRect.top() || centerY > mapping.drawRect.bottom()) {
                continue;
            }
            // Clip labels that overflow past the draw rect (e.g. into the price axis)
            if (originX > mapping.drawRect.right() || originX + (maxX - minX) * scale > mapping.drawRect.right() + 4.0f) {
                continue;
            }

            const QColor color = Qt::white;

            penX = 0.0f;
            for (const QChar c : label) {
                const auto& glyph = atlas.glyph(c);
                if (glyph.advance <= 0.0f || glyph.uv.isNull()) {
                    penX += glyph.advance;
                    continue;
                }

                ChartGlyphInstance instance;
                instance.rect = QRectF(originX + (penX + glyph.bounds.left()) * scale,
                                       originY + glyph.bounds.top() * scale,
                                       glyph.bounds.width() * scale,
                                       glyph.bounds.height() * scale);
                instance.uv = glyph.uv;
                instance.color = color;
                glyphs.push_back(instance);
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

    Q_UNUSED(dollars);
    return QString("%1%2").arg(number, suffix);
}
