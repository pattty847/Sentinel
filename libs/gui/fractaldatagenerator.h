#pragma once

#include <QObject>
#include <vector>
#include "orderbook.h"

class FractalDataGenerator : public QObject {
    Q_OBJECT
public:
    explicit FractalDataGenerator(QObject* parent = nullptr);

    Q_INVOKABLE void generateData(int numFrames, int levelsPerFrame);

signals:
    void orderBookUpdated(const OrderBook& book);

private:
    double m_midPrice = 107283.0;
    double m_priceIncrement = 5.0;
    double m_sizeIncrement = 0.1;
};
