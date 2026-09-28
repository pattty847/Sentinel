#pragma once
#include <QObject>
#include <cstdint>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "../marketdata/model/TradeData.h"

struct OHLCVBar {
    int64_t timestamp_ms = 0; // UTC epoch-aligned bucket start
    double open = 0.0;
    double high = 0.0;
    double low = 0.0;
    double close = 0.0;
    double volume = 0.0;
    uint32_t count = 0;
    bool is_closed = false;
};

class TimeframeAggregator : public QObject {
    Q_OBJECT
public:
    explicit TimeframeAggregator(const std::vector<int64_t>& timeframesMs = {},
                                 QObject* parent = nullptr);
    void onTrade(const Trade& trade);
    void tick(int64_t nowMs);
    static std::vector<OHLCVBar> rollupMinutes(const std::vector<OHLCVBar>& minutes,
                                                int64_t timeframeMs);
    std::vector<OHLCVBar> getHistory(const std::string& symbol, int64_t timeframeMs,
                                     size_t limit = 1000) const;

signals:
    void barClosed(const QString& symbol, int64_t timeframeMs, const OHLCVBar& bar);
    void barUpdated(const QString& symbol, int64_t timeframeMs, const OHLCVBar& bar);

private:
    struct SymbolState {
        std::unordered_map<int64_t, OHLCVBar> activeBars;
        std::unordered_map<int64_t, std::vector<OHLCVBar>> history;
    };
    mutable std::shared_mutex m_mutex;
    std::unordered_map<std::string, SymbolState> m_states;
    std::vector<int64_t> m_timeframesMs;

    static int64_t bucketStart(int64_t timestampMs, int64_t timeframeMs);
    void closeBar(SymbolState& state, const std::string& symbol, int64_t timeframeMs,
                  OHLCVBar& bar);
    void addMinuteToRollups(SymbolState& state, const std::string& symbol,
                            const OHLCVBar& minute);
    void updateTradeBar(SymbolState& state, const std::string& symbol,
                        int64_t timeframeMs, const Trade& trade, int64_t tradeTsMs);
};
