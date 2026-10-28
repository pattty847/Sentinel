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
    int64_t lastTradeMs = 0; // newest accepted trade of this symbol; zero until the first trade
    void setSymbol(const QString& next) {
        symbol = next;
        bookMid = lastTrade = 0.0;
        bookMidValid = false;
        lastTradeMs = 0;
    }
};

} // namespace chart
