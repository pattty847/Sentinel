#pragma once
#include <QObject>
#include <array>
#include <cstdint>
#include <map>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
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
    // historyBars: closed bars guaranteed per timeframe (chart.history_bars).
    // A timeframe with an entry keeps [cap, cap + cap/10] once populated; one
    // without keeps at most 10,000 (trimmed by 1,000).
    explicit TimeframeAggregator(const std::vector<int64_t>& timeframesMs = {},
                                 std::map<int64_t, int> historyBars = {},
                                 QObject* parent = nullptr);
    void onTrade(const Trade& trade);
    void tick(int64_t nowMs);
    // Journal live feed. The symbol's bars close on its own journal
    // time, never the wall clock: tick() skips it; advance() closes buckets
    // ending at or before the watermark, and each trade closes those before it.
    // Bars are then a function of the delivered sequence alone, so a recovery
    // replay builds exactly what uninterrupted delivery would. Quiet buckets
    // are carried, at most 300 per timeframe (100 ms, 1 s and 1 m) per advance.
    void setJournalClock(const std::string& symbol);
    void advance(const std::string& symbol, int64_t watermarkMs);
    static std::vector<OHLCVBar> rollupMinutes(const std::vector<OHLCVBar>& minutes,
                                                int64_t timeframeMs);
    std::vector<OHLCVBar> getHistory(const std::string& symbol, int64_t timeframeMs,
                                     size_t limit = 1000) const;
    // At most `limit` newest closed bars that start at or before endMs.
    std::vector<OHLCVBar> getHistory(const std::string& symbol, int64_t timeframeMs,
                                     int64_t endMs, size_t limit) const;
    // Trades dropped as older than the open bucket, per (symbol, trade-built timeframe).
    struct LateDrops {
        std::string symbol;
        int64_t timeframeMs = 0;
        uint64_t count = 0;
    };
    std::vector<LateDrops> lateDrops() const;

signals:
    void barClosed(const QString& symbol, int64_t timeframeMs, const OHLCVBar& bar);
    void barUpdated(const QString& symbol, int64_t timeframeMs, const OHLCVBar& bar);

private:
    struct SymbolState {
        std::unordered_map<int64_t, OHLCVBar> activeBars;
        std::unordered_map<int64_t, std::vector<OHLCVBar>> history;
        std::array<uint64_t, 3> lateDrops{}; // indexed like kTradeTimeframes
    };
    // Timeframes built from trades; coarser ones roll up from the minute.
    static constexpr std::array<int64_t, 3> kTradeTimeframes{100, 1'000, 60'000};
    mutable std::shared_mutex m_mutex;
    std::unordered_map<std::string, SymbolState> m_states;
    std::vector<int64_t> m_timeframesMs;
    std::unordered_set<std::string> m_journal;
    std::map<int64_t, size_t> m_historyBars;
    std::array<bool, 3> m_tradeTimeframeOn{}; // kTradeTimeframes[i] is configured

    static int64_t bucketStart(int64_t timestampMs, int64_t timeframeMs);
    void closeBar(SymbolState& state, const std::string& symbol, int64_t timeframeMs,
                  OHLCVBar& bar);
    void addMinuteToRollups(SymbolState& state, const std::string& symbol,
                            const OHLCVBar& minute);
    void closeElapsed(SymbolState& state, const std::string& symbol, int64_t nowMs);
    void updateTradeBar(SymbolState& state, const std::string& symbol,
                        int64_t timeframeMs, const Trade& trade, int64_t tradeTsMs,
                        bool journal = false);
};
