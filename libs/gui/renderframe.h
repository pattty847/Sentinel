#pragma once

#include "tradedata.h"
#include <vector>

struct RenderFrame {
    qint64 startTime_ms;
    qint64 endTime_ms;
    std::vector<Trade> trades;
    // std::vector<OrderBookUpdate> orderBookUpdates; // Add this when HeatmapBatched is available
};
