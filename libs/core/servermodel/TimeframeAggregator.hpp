#pragma once
#include <QObject>
#include <string>
#include <vector>
#include <unordered_map>
#include <chrono>
#include <shared_mutex>
#include "../marketdata/model/TradeData.h"

struct OHLCVBar {
    int64_t timestamp_ms; // Start of the bar
    double open = 0.0;
    double high = 0.0;
    double low = 0.0;
    double close = 0.0;
    double volume = 0.0;
    uint32_t count = 0;   // Number of trades
    bool is_closed = false;
};

enum class Timeframe : int {
    OneSecond = 1,
    OneMinute = 60,
    FiveMinutes = 300,
    OneHour = 3600,
    OneDay = 86400
};

class TimeframeAggregator : public QObject {
    Q_OBJECT
public:
    explicit TimeframeAggregator(const std::vector<int64_t>& timeframesMs = {},
                                 QObject* parent = nullptr);
    
    void onTrade(const Trade& trade);
    void tick(int64_t nowMs);
    
    std::vector<OHLCVBar> getHistory(const std::string& symbol, Timeframe tf, size_t limit = 1000) const;

signals:
    void barClosed(const QString& symbol, Timeframe tf, const OHLCVBar& bar);
    void barUpdated(const QString& symbol, Timeframe tf, const OHLCVBar& bar);

private:
    struct SymbolState {
        std::unordered_map<Timeframe, OHLCVBar> activeBars;
        std::unordered_map<Timeframe, std::vector<OHLCVBar>> history;
    };
    
    mutable std::shared_mutex m_mutex;
    std::unordered_map<std::string, SymbolState> m_states;
    std::vector<Timeframe> m_timeframes;
    
    void updateBar(SymbolState& state, const std::string& symbol, Timeframe tf, const Trade& trade, int64_t tradeTsMs);
    int64_t getBarStartTimestamp(int64_t tsMs, Timeframe tf);
};
