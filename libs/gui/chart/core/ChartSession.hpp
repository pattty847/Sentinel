#pragma once

#include <QString>
#include <cstdint>

namespace chart {

struct ChartSession {
    QString symbol;
    int64_t timeframeMs = 100;
    double bookMid = 0.0;
    bool bookMidValid = false; // the last book top was two-sided (bookMid keeps the last valid one)
    double lastTrade = 0.0;
};

} // namespace chart
