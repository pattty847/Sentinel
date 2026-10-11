#pragma once

#include <QString>
#include <cstdint>

namespace chart {

struct ChartSession {
    QString symbol;
    int64_t timeframeMs = 100;
    double bookMid = 0.0;
    double lastTrade = 0.0;
};

} // namespace chart
