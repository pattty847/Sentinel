#pragma once

#include <QByteArray>
#include <QString>
#include <QMetaType>

struct HeatmapSlice {
    QString symbol;
    int64_t bucketStartMs = 0;
    int64_t bucketEndMs = 0;
    int64_t timeframeMs = 0;
    int gridWidth = 0;
    int gridHeight = 0;
    double minPrice = 0.0;
    double maxPrice = 0.0;
    double tickSize = 0.0;
    double midPrice = 0.0;
    double lastTrade = 0.0;
    QString format = QStringLiteral("u16");
    QByteArray column;
    QByteArray liquidityColumn;
    double liquidityScale = 1.0;
    bool reset = false;
};

Q_DECLARE_METATYPE(HeatmapSlice)
