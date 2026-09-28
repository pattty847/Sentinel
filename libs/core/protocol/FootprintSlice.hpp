#pragma once

#include <QByteArray>
#include <QString>
#include <QMetaType>
#include <cstdint>

struct FootprintSlice {
    QString symbol;
    int64_t bucketStartMs = 0;
    int64_t bucketEndMs = 0;
    int64_t timeframeMs = 0;
    int gridWidth = 0;
    int gridHeight = 0;
    double minPrice = 0.0;
    double maxPrice = 0.0;
    double tickSize = 0.0;
    double quantScale = 1.0;
    QString format = QStringLiteral("q16_delta");
    QByteArray deltaLevelsQ16;
};

Q_DECLARE_METATYPE(FootprintSlice)
