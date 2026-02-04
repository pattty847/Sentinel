#include "TimeframeAggregator.hpp"
#include "SentinelLogging.hpp"
#include <algorithm>

TimeframeAggregator::TimeframeAggregator(QObject* parent) : QObject(parent) {
}

void TimeframeAggregator::tick(int64_t nowMs) {
    std::unique_lock lock(m_mutex);

    const Timeframe frames[] = { Timeframe::OneSecond, Timeframe::OneMinute, Timeframe::FiveMinutes, Timeframe::OneHour };

    for (auto& [symbol, state] : m_states) {
        for (auto tf : frames) {
            const int64_t tfMs = static_cast<int64_t>(tf) * 1000;
            if (tfMs <= 0) {
                continue;
            }

            const int64_t currentStart = getBarStartTimestamp(nowMs, tf);
            auto& currentBar = state.activeBars[tf];
            auto& hist = state.history[tf];

            double lastClose = 0.0;
            bool hasLast = false;
            if (currentBar.count > 0) {
                lastClose = currentBar.close;
                hasLast = true;
            } else if (!hist.empty()) {
                lastClose = hist.back().close;
                hasLast = true;
            }

            if (!hasLast) {
                continue;
            }

            if (currentBar.timestamp_ms == 0) {
                currentBar.timestamp_ms = currentStart;
                currentBar.open = lastClose;
                currentBar.high = lastClose;
                currentBar.low = lastClose;
                currentBar.close = lastClose;
                currentBar.volume = 0.0;
                currentBar.count = 0;
                currentBar.is_closed = false;
                emit barUpdated(QString::fromStdString(symbol), tf, currentBar);
                continue;
            }

            if (currentStart <= currentBar.timestamp_ms) {
                continue;
            }

            if (!currentBar.is_closed) {
                OHLCVBar closedBar = currentBar;
                closedBar.is_closed = true;
                hist.push_back(closedBar);
                if (hist.size() > 10000) {
                    hist.erase(hist.begin(), hist.begin() + 1000);
                }
                emit barClosed(QString::fromStdString(symbol), tf, closedBar);
            }

            int64_t nextStart = currentBar.timestamp_ms + tfMs;
            int emptyCount = 0;
            while (nextStart < currentStart && emptyCount < 300) {
                OHLCVBar emptyBar{};
                emptyBar.timestamp_ms = nextStart;
                emptyBar.open = lastClose;
                emptyBar.high = lastClose;
                emptyBar.low = lastClose;
                emptyBar.close = lastClose;
                emptyBar.volume = 0.0;
                emptyBar.count = 0;
                emptyBar.is_closed = true;
                hist.push_back(emptyBar);
                if (hist.size() > 10000) {
                    hist.erase(hist.begin(), hist.begin() + 1000);
                }
                emit barClosed(QString::fromStdString(symbol), tf, emptyBar);
                nextStart += tfMs;
                emptyCount++;
            }

            currentBar = OHLCVBar{};
            currentBar.timestamp_ms = currentStart;
            currentBar.open = lastClose;
            currentBar.high = lastClose;
            currentBar.low = lastClose;
            currentBar.close = lastClose;
            currentBar.volume = 0.0;
            currentBar.count = 0;
            currentBar.is_closed = false;
            emit barUpdated(QString::fromStdString(symbol), tf, currentBar);
        }
    }
}

void TimeframeAggregator::onTrade(const Trade& trade) {
    std::unique_lock lock(m_mutex);
    
    int64_t tsMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        trade.timestamp.time_since_epoch()).count();
        
    SymbolState& state = m_states[trade.product_id];
    
    const Timeframe frames[] = { Timeframe::OneSecond, Timeframe::OneMinute, Timeframe::FiveMinutes, Timeframe::OneHour };
    
    for (auto tf : frames) {
        updateBar(state, trade.product_id, tf, trade, tsMs);
    }
}

void TimeframeAggregator::updateBar(SymbolState& state, const std::string& symbol, Timeframe tf, const Trade& trade, int64_t tradeTsMs) {
    int64_t durationSec = static_cast<int64_t>(tf);
    int64_t barStart = getBarStartTimestamp(tradeTsMs, tf);
    
    auto& currentBar = state.activeBars[tf];
    
    if (currentBar.count > 0 && barStart > currentBar.timestamp_ms) {
        currentBar.is_closed = true;
        
        auto& hist = state.history[tf];
        hist.push_back(currentBar);
        if (hist.size() > 10000) {
             hist.erase(hist.begin(), hist.begin() + 1000);
        }
        
        OHLCVBar closedBar = currentBar;
        emit barClosed(QString::fromStdString(symbol), tf, closedBar);
        
        currentBar = OHLCVBar{};
        currentBar.timestamp_ms = barStart;
        currentBar.open = trade.price;
        currentBar.high = trade.price;
        currentBar.low = trade.price;
        currentBar.close = trade.price;
        currentBar.volume = trade.size;
        currentBar.count = 1;
        
        emit barUpdated(QString::fromStdString(symbol), tf, currentBar);
        return;
    }
    
    if (currentBar.count == 0) {
        currentBar.timestamp_ms = barStart;
        currentBar.open = trade.price;
        currentBar.high = trade.price;
        currentBar.low = trade.price;
        currentBar.close = trade.price;
        currentBar.volume = trade.size;
        currentBar.count = 1;
    } else {
        currentBar.high = std::max(currentBar.high, trade.price);
        currentBar.low = std::min(currentBar.low, trade.price);
        currentBar.close = trade.price;
        currentBar.volume += trade.size;
        currentBar.count++;
    }
    
    emit barUpdated(QString::fromStdString(symbol), tf, currentBar);
}

int64_t TimeframeAggregator::getBarStartTimestamp(int64_t tsMs, Timeframe tf) {
    int64_t durationMs = static_cast<int64_t>(tf) * 1000;
    return (tsMs / durationMs) * durationMs;
}

std::vector<OHLCVBar> TimeframeAggregator::getHistory(const std::string& symbol, Timeframe tf, size_t limit) const {
    std::shared_lock lock(m_mutex);
    
    auto it = m_states.find(symbol);
    if (it == m_states.end()) return {};
    
    const auto& state = it->second;
    auto hit = state.history.find(tf);
    if (hit == state.history.end()) return {};
    
    const auto& hist = hit->second;
    
    if (hist.size() <= limit) {
        return hist;
    }
    
    return std::vector<OHLCVBar>(hist.end() - limit, hist.end());
}
