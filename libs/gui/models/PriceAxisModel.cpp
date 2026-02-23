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
    updateTicksAndNotify();
}

void PriceAxisModel::calculateTicks() {
    clearTicks();
    
    if (!isViewportValid()) return;
    if (!updateEffectiveViewport()) return;
    
    const double priceMin = m_effectiveMinPrice;
    const double priceMax = m_effectiveMaxPrice;
    double priceRange = priceMax - priceMin;
    if (priceRange <= 0.0) {
        priceRange = 1.0;
    }

    const double viewportPx = std::max(1.0, m_effectiveSpanPx);
    constexpr double kMinLabelGapPx = 60.0;
    int maxLabelCount = static_cast<int>(std::floor(viewportPx / kMinLabelGapPx));
    maxLabelCount = std::max(2, maxLabelCount);

    const double rawSpacing = priceRange / static_cast<double>(maxLabelCount - 1);
    const double tickSize = (m_tickSize > 0.0)
        ? m_tickSize
        : (m_viewState ? m_viewState->calculateOptimalPriceResolution() : 1.0);
    if (tickSize <= 0.0) {
        return;
    }

    auto niceNumber = [](double value, bool roundDown) -> double {
        if (value <= 0.0) return 1.0;
        const double exponent = std::floor(std::log10(value));
        const double magnitude = std::pow(10.0, exponent);
        const double fraction = value / magnitude;
        double niceFraction = 1.0;
        if (roundDown) {
            if (fraction < 1.5) {
                niceFraction = 1.0;
            } else if (fraction < 3.0) {
                niceFraction = 2.0;
            } else if (fraction < 7.0) {
                niceFraction = 5.0;
            } else {
                niceFraction = 10.0;
            }
        } else {
            if (fraction <= 1.0) {
                niceFraction = 1.0;
            } else if (fraction <= 2.0) {
                niceFraction = 2.0;
            } else if (fraction <= 5.0) {
                niceFraction = 5.0;
            } else {
                niceFraction = 10.0;
            }
        }
        return niceFraction * magnitude;
    };

    double niceSpacing = niceNumber(rawSpacing, true);
    niceSpacing = std::max(niceSpacing, tickSize);
    niceSpacing = std::round(niceSpacing / tickSize) * tickSize;
    if (niceSpacing <= 0.0) {
        return;
    }

    if (m_lastNiceSpacing > 0.0) {
        const double ratio = niceSpacing / m_lastNiceSpacing;
        if (ratio <= 1.2 && ratio >= 0.83) {
            niceSpacing = m_lastNiceSpacing;
        }
    }
    m_lastNiceSpacing = niceSpacing;

    const double firstTick = std::floor(priceMin / niceSpacing) * niceSpacing;
    const double lastTick = std::ceil(priceMax / niceSpacing) * niceSpacing;

    int safeCount = 0;
    for (double price = firstTick; price <= lastTick + (tickSize * 0.5); price += niceSpacing) {
        const double screenY = valueToScreenPosition(price);
        if (screenY >= 0 && screenY <= getViewportHeight()) {
            const QString label = formatLabel(price);
            addTick(price, screenY, label, true);
        }
        if (++safeCount > 1000) {
            break;
        }
    }
    
}

QString PriceAxisModel::formatLabel(double value) const {
    double priceRange = m_effectiveViewportValid
        ? (m_effectiveMaxPrice - m_effectiveMinPrice)
        : (getViewportEnd() - getViewportStart());
    
    if (priceRange > 1000) {
        return QString("$%1").arg(static_cast<int>(std::round(value)));
    } else if (priceRange > 100) {
        return QString("$%1").arg(value, 0, 'f', 1);
    } else {
        return QString("$%1").arg(value, 0, 'f', 2);
    }
}

double PriceAxisModel::getViewportStart() const {
    if (!m_viewState) {
        return 0.0;
    }
    double minPrice = m_viewState->getMinPrice();
    if (m_viewState->isDragging()) {
        const double viewHeight = getViewportHeight();
        const double priceRange = m_viewState->getMaxPrice() - m_viewState->getMinPrice();
        if (viewHeight > 0.0 && priceRange > 0.0) {
            const double pricePixelsToUnits = priceRange / viewHeight;
            minPrice += (m_viewState->getPanVisualOffset().y() * pricePixelsToUnits);
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
        const double viewHeight = getViewportHeight();
        const double priceRange = m_viewState->getMaxPrice() - m_viewState->getMinPrice();
        if (viewHeight > 0.0 && priceRange > 0.0) {
            const double pricePixelsToUnits = priceRange / viewHeight;
            maxPrice += (m_viewState->getPanVisualOffset().y() * pricePixelsToUnits);
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

    double normalized = (value - priceMin) / (priceMax - priceMin);
    return getViewportHeight() * (1.0 - normalized);
}

double PriceAxisModel::calculateNicePriceStep(double range, int targetTicks) const {
    if (range <= 0 || targetTicks <= 0) return 1.0;
    
    double rawStep = range / targetTicks;
    double magnitude = std::pow(10.0, std::floor(std::log10(rawStep)));
    double normalizedStep = rawStep / magnitude;
    double niceStep;
    if (normalizedStep <= 1.0) {
        niceStep = 1.0;
    } else if (normalizedStep <= 2.0) {
        niceStep = 2.0;
    } else if (normalizedStep <= 2.5) {
        niceStep = 2.5;
    } else if (normalizedStep <= 5.0) {
        niceStep = 5.0;
    } else {
        niceStep = 10.0;
    }
    
    double step = niceStep * magnitude;
    if (step < 0.01) step = 0.01;
    return step;
}
