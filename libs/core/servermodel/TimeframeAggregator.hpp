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

// Enum for timeframe resolution in seconds
enum class Timeframe : int {
    OneSecond = 1,
    OneMinute = 60,
    FiveMinutes = 300,
    OneHour = 3600
};

class TimeframeAggregator : public QObject {
    Q_OBJECT
public:
    explicit TimeframeAggregator(QObject* parent = nullptr);
    
    // Core input
    void onTrade(const Trade& trade);
    
    // Accessors
    std::vector<OHLCVBar> getHistory(const std::string& symbol, Timeframe tf, size_t limit = 1000) const;

signals:
    // Emitted when a bar is completed (time rolled over)
    void barClosed(const QString& symbol, Timeframe tf, const OHLCVBar& bar);
    // Emitted on every update (for real-time candle drawing)
    void barUpdated(const QString& symbol, Timeframe tf, const OHLCVBar& bar);

private:
    struct SymbolState {
        // Current forming bar for each timeframe
        std::unordered_map<Timeframe, OHLCVBar> activeBars;
        // History storage (ring buffer concept, but vector for now)
        std::unordered_map<Timeframe, std::vector<OHLCVBar>> history;
    };
    
    mutable std::shared_mutex m_mutex;
    std::unordered_map<std::string, SymbolState> m_states;
    
    void updateBar(SymbolState& state, const std::string& symbol, Timeframe tf, const Trade& trade, int64_t tradeTsMs);
    int64_t getBarStartTimestamp(int64_t tsMs, Timeframe tf);
};
