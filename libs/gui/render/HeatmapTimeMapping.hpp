#pragma once

#include <QRectF>

struct HeatmapTimeMapping {
    QRectF drawRect;
    QRectF srcRect;
    double dataStartMs = 0.0;
    double appendMs = 0.0;
    int gridWidth = 0;
    float timeOffset = 0.0f;
    double cellW = 0.0;
    bool valid = false;
};
