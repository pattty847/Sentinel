#pragma once

#include <QByteArray>
#include <QMetaType>
#include <QString>

// Skeleton payload for future TPO/market-profile streaming.
struct TpoSlice {
    QString symbol;
    int64_t bucketStartMs = 0;
    int64_t bucketEndMs = 0;
    int64_t timeframeMs = 0;
    int sessionType = 4;
    int gridWidth = 0;
    int gridHeight = 0;
    double minPrice = 0.0;
    double maxPrice = 0.0;
    double tickSize = 0.0;
    QString format = QStringLiteral("tpo_ascii");
    QByteArray letters;
};

Q_DECLARE_METATYPE(TpoSlice)
