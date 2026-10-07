#include "TimeframeAggregator.hpp"
#include "SentinelLogging.hpp"
#include <algorithm>
#include <chrono>
#include <mutex>

namespace {
constexpr int64_t kMinuteMs = 60'000;
constexpr int64_t kSecondMs = 1'000;
}

TimeframeAggregator::TimeframeAggregator(const std::vector<int64_t>& timeframesMs, QObject* parent)
    : QObject(parent) {
    m_timeframesMs = timeframesMs.empty()
        ? std::vector<int64_t>{kSecondMs, kMinuteMs, 300'000, 3'600'000}
        : timeframesMs;
    m_timeframesMs.push_back(kMinuteMs); // Canonical anchor even if omitted from config.
    m_timeframesMs.erase(std::remove_if(m_timeframesMs.begin(), m_timeframesMs.end(),
        [](int64_t tf) {
            if (tf == kSecondMs || (tf >= kMinuteMs && tf % kMinuteMs == 0)) return false;
            sLog_Warning("TimeframeAggregator: unsupported timeframe_ms=" << tf);
            return true;
        }), m_timeframesMs.end());
    std::sort(m_timeframesMs.begin(), m_timeframesMs.end());
    m_timeframesMs.erase(std::unique(m_timeframesMs.begin(), m_timeframesMs.end()),
                         m_timeframesMs.end());
}

int64_t TimeframeAggregator::bucketStart(int64_t timestampMs, int64_t timeframeMs) {
    const int64_t quotient = timestampMs / timeframeMs;
    return (timestampMs < 0 && timestampMs % timeframeMs != 0 ? quotient - 1 : quotient)
           * timeframeMs;
}

void TimeframeAggregator::closeBar(SymbolState& state, const std::string& symbol,
                                   int64_t timeframeMs, OHLCVBar& bar) {
    bar.is_closed = true;
    auto& hist = state.history[timeframeMs];
    hist.push_back(bar);
    if (hist.size() > 10000) hist.erase(hist.begin(), hist.begin() + 1000);
    emit barClosed(QString::fromStdString(symbol), timeframeMs, bar);
    if (timeframeMs == kMinuteMs) addMinuteToRollups(state, symbol, bar);
}

void TimeframeAggregator::addMinuteToRollups(SymbolState& state, const std::string& symbol,
                                             const OHLCVBar& minute) {
    for (const int64_t tf : m_timeframesMs) {
        if (tf <= kMinuteMs) continue;
        const int64_t start = bucketStart(minute.timestamp_ms, tf);
        auto [it, inserted] = state.activeBars.try_emplace(tf);
        auto& bar = it->second;
        if (!inserted && bar.timestamp_ms != start) {
            closeBar(state, symbol, tf, bar);
            bar = {};
            inserted = true;
        }
        if (inserted) {
            bar = minute;
            bar.timestamp_ms = start;
            bar.is_closed = false;
        } else {
            bar.high = std::max(bar.high, minute.high);
            bar.low = std::min(bar.low, minute.low);
            bar.close = minute.close;
            bar.volume += minute.volume;
            bar.count += minute.count;
        }
        emit barUpdated(QString::fromStdString(symbol), tf, bar);
    }
}

void TimeframeAggregator::updateTradeBar(SymbolState& state, const std::string& symbol,
                                         int64_t timeframeMs, const Trade& trade,
                                         int64_t tradeTsMs) {
    const int64_t start = bucketStart(tradeTsMs, timeframeMs);
    auto& bar = state.activeBars[timeframeMs];
    if (bar.count > 0 && start < bar.timestamp_ms) return; // stale trade
    if ((bar.count > 0 || bar.timestamp_ms != 0) && start > bar.timestamp_ms) {
        closeBar(state, symbol, timeframeMs, bar);
        bar = {};
    }
    if (bar.count == 0) {
        bar.timestamp_ms = start;
        bar.open = bar.high = bar.low = bar.close = trade.price;
        bar.volume = trade.size;
        bar.count = 1;
        bar.is_closed = false;
    } else {
        bar.high = std::max(bar.high, trade.price);
        bar.low = std::min(bar.low, trade.price);
        bar.close = trade.price;
        bar.volume += trade.size;
        ++bar.count;
    }
    emit barUpdated(QString::fromStdString(symbol), timeframeMs, bar);
}

void TimeframeAggregator::onTrade(const Trade& trade) {
    std::unique_lock lock(m_mutex);
    const int64_t tsMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        trade.timestamp.time_since_epoch()).count();
    auto& state = m_states[trade.product_id];
    // Held (journal recovery replay): the trade's own time drives closing, as
    // the timer would have at that time, so bars match uninterrupted delivery.
    if (m_held.contains(trade.product_id)) closeElapsed(state, trade.product_id, tsMs);
    if (std::binary_search(m_timeframesMs.begin(), m_timeframesMs.end(), kSecondMs))
        updateTradeBar(state, trade.product_id, kSecondMs, trade, tsMs);
    updateTradeBar(state, trade.product_id, kMinuteMs, trade, tsMs);
}

void TimeframeAggregator::setHeld(const std::string& symbol, bool held) {
    std::unique_lock lock(m_mutex);
    if (held) m_held.insert(symbol);
    else m_held.erase(symbol);
}

void TimeframeAggregator::tick(int64_t nowMs) {
    std::unique_lock lock(m_mutex);
    for (auto& [symbol, state] : m_states)
        if (!m_held.contains(symbol)) closeElapsed(state, symbol, nowMs);
}

// Closes the symbol's bars that end at or before nowMs, carrying quiet buckets.
void TimeframeAggregator::closeElapsed(SymbolState& state, const std::string& symbol, int64_t nowMs) {
    {
        for (const int64_t tf : {kSecondMs, kMinuteMs}) {
            if (!std::binary_search(m_timeframesMs.begin(), m_timeframesMs.end(), tf)) continue;
            auto& bar = state.activeBars[tf];
            if ((bar.count == 0 && bar.timestamp_ms == 0) ||
                bucketStart(nowMs, tf) <= bar.timestamp_ms) continue;
            const double lastClose = bar.close;
            int64_t nextStart = bar.timestamp_ms + tf;
            closeBar(state, symbol, tf, bar);
            // Carry short bars across quiet buckets; only synthesized 1m
            // bars feed the coarser rollups, keeping their OHLCV consistent.
            const int64_t currentStart = bucketStart(nowMs, tf);
            int emptyCount = 0;
            while (nextStart < currentStart && emptyCount < 300) {
                OHLCVBar empty{};
                empty.timestamp_ms = nextStart;
                empty.open = empty.high = empty.low = empty.close = lastClose;
                closeBar(state, symbol, tf, empty);
                nextStart += tf;
                ++emptyCount;
            }
            bar = {};
            bar.timestamp_ms = currentStart;
            bar.open = bar.high = bar.low = bar.close = lastClose;
            emit barUpdated(QString::fromStdString(symbol), tf, bar);
        }
        // Close complete rolled groups at the UTC boundary, even if no trade
        // arrives in the first minute of the next group.
        for (const int64_t tf : m_timeframesMs) {
            if (tf <= kMinuteMs) continue;
            auto found = state.activeBars.find(tf);
            if (found != state.activeBars.end() &&
                bucketStart(nowMs, tf) > found->second.timestamp_ms) {
                closeBar(state, symbol, tf, found->second);
                state.activeBars.erase(found);
            }
        }
    }
}

std::vector<OHLCVBar> TimeframeAggregator::rollupMinutes(
    const std::vector<OHLCVBar>& minutes, int64_t timeframeMs) {
    if (timeframeMs < kMinuteMs || timeframeMs % kMinuteMs != 0) return {};
    std::vector<OHLCVBar> output;
    for (const auto& minute : minutes) {
        const int64_t start = bucketStart(minute.timestamp_ms, timeframeMs);
        if (output.empty() || output.back().timestamp_ms != start) {
            output.push_back(minute);
            output.back().timestamp_ms = start;
        } else {
            auto& bar = output.back();
            bar.high = std::max(bar.high, minute.high);
            bar.low = std::min(bar.low, minute.low);
            bar.close = minute.close;
            bar.volume += minute.volume;
            bar.count += minute.count;
            bar.is_closed = bar.is_closed && minute.is_closed;
        }
    }
    return output;
}

std::vector<OHLCVBar> TimeframeAggregator::getHistory(const std::string& symbol,
                                                       int64_t timeframeMs, size_t limit) const {
    std::shared_lock lock(m_mutex);
    const auto state = m_states.find(symbol);
    if (state == m_states.end()) return {};
    const auto found = state->second.history.find(timeframeMs);
    if (found == state->second.history.end()) return {};
    const auto& hist = found->second;
    if (hist.size() <= limit) return hist;
    return {hist.end() - static_cast<std::ptrdiff_t>(limit), hist.end()};
}
