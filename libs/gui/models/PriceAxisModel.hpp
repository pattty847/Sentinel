#pragma once
#include "AxisModel.hpp"

/**
 * PriceAxisModel - Calculates nice price tick marks
 * 
 * Specializes AxisModel for price data. Calculates "nice" price steps
 * like $1, $2, $5, $10, $20, $50, etc. and formats labels with appropriate
 * precision (e.g., $45.50, $100.00).
 */
class PriceAxisModel : public AxisModel {
    Q_OBJECT
    
public:
    explicit PriceAxisModel(QObject* parent = nullptr);
    
    // AxisModel interface
    void recalculateTicks() override;
    
protected:
    void calculateTicks() override;
    QString formatLabel(double value) const override;
    double getViewportStart() const override;
    double getViewportEnd() const override;
    double valueToScreenPosition(double value) const override;
    
private:
    double calculateNicePriceStep(double range, int targetTicks) const;
    int getDecimalPlaces(double step) const;
};