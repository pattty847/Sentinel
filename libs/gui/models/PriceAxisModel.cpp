#include "PriceAxisModel.hpp"
#include "../render/GridViewState.hpp"
#include "../UnifiedGridRenderer.h"
#include <QDebug>
#include <algorithm>
#include <cmath>

PriceAxisModel::PriceAxisModel(QObject* parent)
    : AxisModel(parent) {
}

void PriceAxisModel::setTickSize(double size) {
    if (size > 0.0 && size != m_tickSize) {
        m_tickSize = size;
        emit tickSizeChanged();
        recalculateTicks();
    }
}

bool PriceAxisModel::updateEffectiveViewport() {
    m_effectiveViewportValid = false;
    if (!isViewportValid()) {
        return false;
    }

    const double viewMin = getViewportStart();
    const double viewMax = getViewportEnd();
    const double viewSpan = viewMax - viewMin;
    const double viewHeight = getViewportHeight();
    if (viewSpan <= 0.0 || viewHeight <= 0.0) {
        return false;
    }

    m_effectiveMinPrice = viewMin;
    m_effectiveMaxPrice = viewMax;
    m_effectiveOffsetPx = 0.0;
    m_effectiveSpanPx = viewHeight;
    m_effectiveViewportValid = true;

    if (qEnvironmentVariableIsSet("SENTINEL_GPU_HEATMAP_FORCE_FULL")) {
        return true;
    }

    if (auto* grid = renderer()) {
        double dataMin = 0.0;
        double dataMax = 0.0;
        if (grid->heatmapDataPriceRange(dataMin, dataMax)) {
            const double overlapMin = std::max(viewMin, dataMin);
            const double overlapMax = std::min(viewMax, dataMax);
            if (overlapMax <= overlapMin) {
                m_effectiveViewportValid = false;
                return false;
            }
            if (overlapMin > viewMin || overlapMax < viewMax) {
                const double ratioTop = (viewMax - overlapMax) / viewSpan;
                const double ratioBottom = (viewMax - overlapMin) / viewSpan;
                m_effectiveOffsetPx = viewHeight * ratioTop;
                m_effectiveSpanPx = viewHeight * (ratioBottom - ratioTop);
                m_effectiveMinPrice = overlapMin;
                m_effectiveMaxPrice = overlapMax;
                m_effectiveViewportValid = (m_effectiveSpanPx > 0.0);
            }
        }
    }

    return m_effectiveViewportValid;
}

void PriceAxisModel::recalculateTicks() {
    if (isViewportValid()) {
        beginResetModel();
        calculateTicks();
        endResetModel();
    }
}

void PriceAxisModel::calculateTicks() {
    clearTicks();
    
    if (!isViewportValid()) return;
    if (!updateEffectiveViewport()) return;
    
    double priceMin = m_effectiveMinPrice;
    double priceMax = m_effectiveMaxPrice;
    double priceRange = priceMax - priceMin;
    
    if (priceRange <= 0) return;

    // Pixel-driven stride: only skip rows when the label won't fit
    const double bucket = (m_tickSize > 0.0)
        ? m_tickSize
        : (m_viewState ? m_viewState->calculateOptimalPriceResolution() : 1.0);
    if (bucket <= 0.0) {
        return;
    }

    const double rowsVisible = priceRange / bucket;
    if (rowsVisible <= 0.0) {
        return;
    }

    const double rowHeightPx = m_effectiveSpanPx / rowsVisible;
    // Approximate label height: QML font is 10px; add a little padding.
    constexpr double kLabelPx = 12.0;
    const int stride = std::max(1, static_cast<int>(std::ceil(kLabelPx / std::max(1e-6, rowHeightPx))));

    // Align ticks to row centers. Use a base on bucket boundary.
    const double base = std::floor(priceMin / bucket) * bucket;
    const int startRow = std::max(0, static_cast<int>(std::floor((priceMin - base) / bucket)));
    const int endRow = static_cast<int>(std::ceil((priceMax - base) / bucket));

    for (int row = startRow; row <= endRow; row += stride) {
        const double price = base + (static_cast<double>(row) + 0.5) * bucket;
        if (price < priceMin || price > priceMax + bucket * 0.5) {
            continue;
        }

        const double screenY = valueToScreenPosition(price);
        if (screenY >= 0 && screenY <= getViewportHeight()) {
            const QString label = formatLabel(price);
            const bool isMajor = (row % stride == 0);
            addTick(price, screenY, label, isMajor);
        }
    }
    
    //qDebug() << "PriceAxisModel: Generated" << m_ticks.size() 
    //         << "price ticks for range $" << priceMin << "-$" << priceMax 
    //         << "step=$" << step;
}

QString PriceAxisModel::formatLabel(double value) const {
    // Determine appropriate decimal places based on the price range
    double priceRange = m_effectiveViewportValid
        ? (m_effectiveMaxPrice - m_effectiveMinPrice)
        : (getViewportEnd() - getViewportStart());
    
    if (priceRange > 1000) {
        // Large prices - no decimals
        return QString("$%1").arg(static_cast<int>(std::round(value)));
    } else if (priceRange > 100) {
        // Medium prices - 1 decimal
        return QString("$%1").arg(value, 0, 'f', 1);
    } else {
        // Small prices - 2 decimals
        return QString("$%1").arg(value, 0, 'f', 2);
    }
}

double PriceAxisModel::getViewportStart() const {
    if (!m_viewState) {
        return 0.0;
    }
    double minPrice = m_viewState->getMinPrice();
    if (m_viewState->isDragging()) {
        const QPointF pan = m_viewState->getPanVisualOffset();
        const double maxPrice = m_viewState->getMaxPrice();
        const double priceRange = maxPrice - minPrice;
        const double viewportH = getViewportHeight();
        if (!pan.isNull() && priceRange > 0.0 && viewportH > 0.0) {
            const double pricePixelsToUnits = priceRange / viewportH;
            minPrice += pan.y() * pricePixelsToUnits;
        }
    }
    return minPrice;
}

double PriceAxisModel::getViewportEnd() const {
    if (!m_viewState) {
        return 100.0;
    }
    double maxPrice = m_viewState->getMaxPrice();
    if (m_viewState->isDragging()) {
        const QPointF pan = m_viewState->getPanVisualOffset();
        const double minPrice = m_viewState->getMinPrice();
        const double priceRange = maxPrice - minPrice;
        const double viewportH = getViewportHeight();
        if (!pan.isNull() && priceRange > 0.0 && viewportH > 0.0) {
            const double pricePixelsToUnits = priceRange / viewportH;
            maxPrice += pan.y() * pricePixelsToUnits;
        }
    }
    return maxPrice;
}

double PriceAxisModel::valueToScreenPosition(double value) const {
    if (!isViewportValid()) return 0.0;

    if (m_effectiveViewportValid && m_effectiveMaxPrice > m_effectiveMinPrice) {
        double normalized = (value - m_effectiveMinPrice) / (m_effectiveMaxPrice - m_effectiveMinPrice);
        return m_effectiveOffsetPx + m_effectiveSpanPx * (1.0 - normalized);
    }

    double priceMin = getViewportStart();
    double priceMax = getViewportEnd();
    
    if (priceMax <= priceMin) return 0.0;
    
    // Price axis is vertical - higher prices at top
    double normalized = (value - priceMin) / (priceMax - priceMin);
    
    // Invert Y coordinate (0 at top, height at bottom)
    return getViewportHeight() * (1.0 - normalized);
}

double PriceAxisModel::calculateNicePriceStep(double range, int targetTicks) const {
    if (range <= 0 || targetTicks <= 0) return 1.0;
    
    double rawStep = range / targetTicks;
    double magnitude = std::pow(10.0, std::floor(std::log10(rawStep)));
    double normalizedStep = rawStep / magnitude;
    
    // Price-specific nice step sizes
    double niceStep;
    if (normalizedStep <= 1.0) {
        niceStep = 1.0;
    } else if (normalizedStep <= 2.0) {
        niceStep = 2.0;
    } else if (normalizedStep <= 2.5) {
        niceStep = 2.5;  // Common for prices
    } else if (normalizedStep <= 5.0) {
        niceStep = 5.0;
    } else {
        niceStep = 10.0;
    }
    
    double step = niceStep * magnitude;
    
    // Ensure minimum step for very small ranges
    if (step < 0.01) step = 0.01;  // Minimum penny
    
    return step;
}
