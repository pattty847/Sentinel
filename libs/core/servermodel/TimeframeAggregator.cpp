#include "TimeframeAggregator.hpp"
#include "SentinelLogging.hpp"
#include <algorithm>

TimeframeAggregator::TimeframeAggregator(QObject* parent) : QObject(parent) {
}

void TimeframeAggregator::onTrade(const Trade& trade) {
    std::unique_lock lock(m_mutex);
    
    // Convert to ms
    int64_t tsMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        trade.timestamp.time_since_epoch()).count();
        
    SymbolState& state = m_states[trade.product_id];
    
    // Update all tracked timeframes
    const Timeframe frames[] = { Timeframe::OneSecond, Timeframe::OneMinute, Timeframe::FiveMinutes, Timeframe::OneHour };
    
    for (auto tf : frames) {
        updateBar(state, trade.product_id, tf, trade, tsMs);
    }
}

void TimeframeAggregator::updateBar(SymbolState& state, const std::string& symbol, Timeframe tf, const Trade& trade, int64_t tradeTsMs) {
    int64_t durationSec = static_cast<int64_t>(tf);
    int64_t barStart = getBarStartTimestamp(tradeTsMs, tf);
    
    auto& currentBar = state.activeBars[tf];
    
    // Check if we need to close the previous bar
    if (currentBar.count > 0 && barStart > currentBar.timestamp_ms) {
        // Bar closed
        currentBar.is_closed = true;
        
        // Save to history
        auto& hist = state.history[tf];
        hist.push_back(currentBar);
        // Keep history bounded? (e.g. 5000 bars)
        if (hist.size() > 10000) {
             hist.erase(hist.begin(), hist.begin() + 1000); // Batch delete
        }
        
        // Emit closed signal
        // We are under lock, so use queued connection implicitly or be careful
        // Since we are QObject, emit is safe but might block if direct connection.
        // Copy bar to avoid reference issues
        OHLCVBar closedBar = currentBar;
        emit barClosed(QString::fromStdString(symbol), tf, closedBar);
        
        // Reset for new bar
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
    
    // First trade ever for this timeframe
    if (currentBar.count == 0) {
        currentBar.timestamp_ms = barStart;
        currentBar.open = trade.price;
        currentBar.high = trade.price;
        currentBar.low = trade.price;
        currentBar.close = trade.price;
        currentBar.volume = trade.size;
        currentBar.count = 1;
    } else {
        // Update existing bar
        currentBar.high = std::max(currentBar.high, trade.price);
        currentBar.low = std::min(currentBar.low, trade.price);
        currentBar.close = trade.price;
        currentBar.volume += trade.size;
        currentBar.count++;
    }
    
    // Emit update signal
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
    
    // Return last 'limit' items
    return std::vector<OHLCVBar>(hist.end() - limit, hist.end());
}
