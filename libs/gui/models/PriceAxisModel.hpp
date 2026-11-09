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
    Q_PROPERTY(double tickSize READ tickSize WRITE setTickSize NOTIFY tickSizeChanged)
    // The chart's symbol: its PriceUnits display unit scales the labels (PEPE: 5.04, x1M).
    Q_PROPERTY(QString symbol READ symbol WRITE setSymbol NOTIFY unitChanged)
    Q_PROPERTY(QString unitLabel READ unitLabel NOTIFY unitChanged) // "PEPE x1M"; empty when plain
    Q_PROPERTY(double unitFactor READ unitFactor NOTIFY unitChanged) // shown = raw * unitFactor
    
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
    
    QString symbol() const { return m_symbol; }
    void setSymbol(const QString& symbol);
    QString unitLabel() const;
    double unitFactor() const;

signals:
    void tickSizeChanged();
    void unitChanged();

private:
    double tickSize() const { return m_tickSize; }
    void setTickSize(double size);
    double calculateNicePriceStep(double range, int targetTicks) const;
    int getDecimalPlaces(double step) const;

    double m_tickSize = 0.0;
    QString m_symbol;
    double m_effectiveMinPrice = 0.0;
    double m_effectiveMaxPrice = 0.0;
    double m_effectiveOffsetPx = 0.0;
    double m_effectiveSpanPx = 0.0;
    bool m_effectiveViewportValid = false;
    double m_lastNiceSpacing = 0.0;

    bool updateEffectiveViewport();
};
