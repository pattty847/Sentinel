#include "TimeframeAggregator.hpp"
#include "SentinelLogging.hpp"
#include <algorithm>
#include <chrono>
#include <mutex>
#include <tuple>

namespace {
constexpr int64_t kMinuteMs = 60'000;
constexpr int64_t kSecondMs = 1'000;
constexpr int64_t kHundredMs = 100;
}

TimeframeAggregator::TimeframeAggregator(const std::vector<int64_t>& timeframesMs,
                                         std::map<int64_t, int> historyBars, QObject* parent)
    : QObject(parent) {
    m_timeframesMs = timeframesMs.empty()
        ? std::vector<int64_t>{kSecondMs, kMinuteMs, 300'000, 3'600'000}
        : timeframesMs;
    m_timeframesMs.push_back(kMinuteMs); // Canonical anchor even if omitted from config.
    m_timeframesMs.erase(std::remove_if(m_timeframesMs.begin(), m_timeframesMs.end(),
        [](int64_t tf) {
            if (tf == kHundredMs || tf == kSecondMs || (tf >= kMinuteMs && tf % kMinuteMs == 0)) return false;
            sLog_Warning("TimeframeAggregator: unsupported timeframe_ms=" << tf);
            return true;
        }), m_timeframesMs.end());
    std::sort(m_timeframesMs.begin(), m_timeframesMs.end());
    m_timeframesMs.erase(std::unique(m_timeframesMs.begin(), m_timeframesMs.end()),
                         m_timeframesMs.end());
    for (size_t i = 0; i < kTradeTimeframes.size(); ++i)
        m_tradeTimeframeOn[i] = std::binary_search(m_timeframesMs.begin(), m_timeframesMs.end(),
                                                   kTradeTimeframes[i]);
    for (const auto& [tf, bars] : historyBars)
        if (bars > 0) m_historyBars[tf] = static_cast<size_t>(bars);
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
    if (const auto cap = m_historyBars.find(timeframeMs); cap != m_historyBars.end()) {
        // Guaranteed retention: [cap, cap + cap/10] once populated.
        if (hist.size() > cap->second + cap->second / 10)
            hist.erase(hist.begin(),
                       hist.begin() + static_cast<std::ptrdiff_t>(hist.size() - cap->second));
    } else {
        if (hist.size() > 10000) hist.erase(hist.begin(), hist.begin() + 1000);
    }
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

int TimeframeAggregator::tradeIndex(int64_t timeframeMs) {
    for (size_t i = 0; i < kTradeTimeframes.size(); ++i)
        if (kTradeTimeframes[i] == timeframeMs) return static_cast<int>(i);
    return -1;
}

void TimeframeAggregator::updateTradeBar(SymbolState& state, const std::string& symbol,
                                         size_t index, const Trade& trade, int64_t tradeTsMs) {
    const int64_t timeframeMs = kTradeTimeframes[index];
    const int64_t start = bucketStart(tradeTsMs, timeframeMs);
    auto& bar = state.activeBars[timeframeMs];
    // Stale trade: older than the open bucket, or in a bucket the watermark
    // already closed, which is never reopened. With no bar yet, an enabled
    // journal floor stands in for the open bucket.
    if (start < bar.timestamp_ms ||
        (state.floorOn[index] && bar.count == 0 && bar.timestamp_ms == 0 &&
         start < state.floorMs[index])) {
        ++state.lateDrops[index];
        return;
    }
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
    // The trade's own time closes the buckets before it, carrying quiet
    // ones, so bars depend only on the delivered sequence.
    closeElapsed(state, trade.product_id, tsMs);
    for (size_t i = 0; i < kTradeTimeframes.size(); ++i)
        if (m_tradeTimeframeOn[i]) updateTradeBar(state, trade.product_id, i, trade, tsMs);
}

void TimeframeAggregator::advance(const std::string& symbol, int64_t watermarkMs) {
    std::unique_lock lock(m_mutex);
    const auto state = m_states.find(symbol);
    if (state != m_states.end()) closeElapsed(state->second, symbol, watermarkMs);
}

// Closes the symbol's bars that end at or before nowMs, carrying quiet buckets.
void TimeframeAggregator::closeElapsed(SymbolState& state, const std::string& symbol, int64_t nowMs) {
    {
        for (size_t i = 0; i < kTradeTimeframes.size(); ++i) {
            if (!m_tradeTimeframeOn[i]) continue;
            const int64_t tf = kTradeTimeframes[i];
            auto& bar = state.activeBars[tf];
            if (bar.count == 0 && bar.timestamp_ms == 0) {
                if (state.floorOn[i])
                    state.floorMs[i] = std::max(state.floorMs[i], bucketStart(nowMs, tf));
                continue;
            }
            if (bucketStart(nowMs, tf) <= bar.timestamp_ms) continue;
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

TimeframeAggregator::Series TimeframeAggregator::exportSeries(const std::string& symbol,
                                                              int64_t timeframeMs) const {
    std::shared_lock lock(m_mutex);
    Series out;
    const auto state = m_states.find(symbol);
    if (state == m_states.end()) return out;
    if (const auto hist = state->second.history.find(timeframeMs);
        hist != state->second.history.end())
        out.closed = hist->second;
    if (const auto bar = state->second.activeBars.find(timeframeMs);
        bar != state->second.activeBars.end())
        out.active = bar->second;
    if (const int i = tradeIndex(timeframeMs); i >= 0 && state->second.floorOn[i])
        out.floorMs = state->second.floorMs[i];
    return out;
}

bool TimeframeAggregator::restoreSeries(const std::string& symbol, int64_t timeframeMs,
                                        Series series) {
    const int i = tradeIndex(timeframeMs);
    if (i < 0 || !m_tradeTimeframeOn[i]) return false;
    std::unique_lock lock(m_mutex);
    if (const auto found = m_states.find(symbol); found != m_states.end()) {
        const auto& existing = found->second;
        const auto hist = existing.history.find(timeframeMs);
        const auto bar = existing.activeBars.find(timeframeMs);
        if ((hist != existing.history.end() && !hist->second.empty()) ||
            (bar != existing.activeBars.end() &&
             (bar->second.count != 0 || bar->second.timestamp_ms != 0)) ||
            existing.floorOn[i])
            return false;
    }
    auto& state = m_states[symbol];
    state.history[timeframeMs] = std::move(series.closed);
    state.activeBars[timeframeMs] = series.active;
    state.floorOn[i] = series.floorMs.has_value();
    state.floorMs[i] = series.floorMs.value_or(0);
    return true;
}

void TimeframeAggregator::seedJournalFloor(const std::string& symbol, int64_t watermarkMs,
                                           const std::vector<int64_t>& timeframesMs) {
    std::unique_lock lock(m_mutex);
    auto& state = m_states[symbol];
    for (const int64_t tf : timeframesMs) {
        const int i = tradeIndex(tf);
        if (i < 0 || !m_tradeTimeframeOn[i]) continue;
        state.floorMs[i] = state.floorOn[i]
            ? std::max(state.floorMs[i], bucketStart(watermarkMs, tf))
            : bucketStart(watermarkMs, tf);
        state.floorOn[i] = true;
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

std::vector<OHLCVBar> TimeframeAggregator::getHistory(const std::string& symbol,
                                                       int64_t timeframeMs, int64_t endMs,
                                                       size_t limit) const {
    std::shared_lock lock(m_mutex);
    const auto state = m_states.find(symbol);
    if (state == m_states.end()) return {};
    const auto found = state->second.history.find(timeframeMs);
    if (found == state->second.history.end()) return {};
    const auto& hist = found->second;
    // Scan from the newest close; at most one page is copied.
    std::vector<OHLCVBar> out;
    out.reserve(std::min(limit, hist.size()));
    for (auto it = hist.rbegin(); it != hist.rend() && out.size() < limit; ++it)
        if (it->timestamp_ms <= endMs) out.push_back(*it);
    std::reverse(out.begin(), out.end());
    return out;
}

std::vector<TimeframeAggregator::LateDrops> TimeframeAggregator::lateDrops() const {
    std::shared_lock lock(m_mutex);
    std::vector<LateDrops> out;
    for (const auto& [symbol, state] : m_states)
        for (size_t i = 0; i < kTradeTimeframes.size(); ++i)
            if (m_tradeTimeframeOn[i])
                out.push_back({symbol, kTradeTimeframes[i], state.lateDrops[i]});
    std::sort(out.begin(), out.end(), [](const LateDrops& a, const LateDrops& b) {
        return std::tie(a.symbol, a.timeframeMs) < std::tie(b.symbol, b.timeframeMs);
    });
    return out;
}
