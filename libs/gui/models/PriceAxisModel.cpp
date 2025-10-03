#include "PriceAxisModel.hpp"
#include "../render/GridViewState.hpp"
#include <QDebug>
#include <cmath>

PriceAxisModel::PriceAxisModel(QObject* parent)
    : AxisModel(parent) {
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
    
    double priceMin = getViewportStart();
    double priceMax = getViewportEnd();
    double priceRange = priceMax - priceMin;
    
    if (priceRange <= 0) return;
    
    // Target 8-12 ticks for good spacing
    int targetTicks = static_cast<int>(getViewportHeight() / 60.0); // ~60 pixels per tick
    targetTicks = std::max(4, std::min(15, targetTicks));
    
    double step = calculateNicePriceStep(priceRange, targetTicks);
    // Snap axis ticks to current bucket size hint to align grid with cells
    if (m_viewState) {
        double bucket = m_viewState->calculateOptimalPriceResolution();
        if (bucket > 0.0) {
            double multiples = std::max(1.0, std::round(step / bucket));
            step = multiples * bucket;
        }
    }
    if (step <= 0) return;
    
    // Find first tick at or below priceMin
    double firstTick = std::floor(priceMin / step) * step;
    
    // Generate ticks
    for (double price = firstTick; price <= priceMax + step * 0.1; price += step) {
        if (price < priceMin - step * 0.1) continue;
        
        double screenY = valueToScreenPosition(price);
        
        // Check if tick is within visible area
        if (screenY >= 0 && screenY <= getViewportHeight()) {
            QString label = formatLabel(price);
            bool isMajor = true; // All price ticks are major for now
            
            addTick(price, screenY, label, isMajor);
        }
    }
    
    qDebug() << "PriceAxisModel: Generated" << m_ticks.size() 
             << "price ticks for range $" << priceMin << "-$" << priceMax 
             << "step=$" << step;
}

QString PriceAxisModel::formatLabel(double value) const {
    // Determine appropriate decimal places based on the price range
    double priceRange = getViewportEnd() - getViewportStart();
    
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
    return m_viewState ? m_viewState->getMinPrice() : 0.0;
}

double PriceAxisModel::getViewportEnd() const {
    return m_viewState ? m_viewState->getMaxPrice() : 100.0;
}

double PriceAxisModel::valueToScreenPosition(double value) const {
    if (!isViewportValid()) return 0.0;
    
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