#include "LiquidityTimeSeriesEngine.h"
#include "SentinelLogging.hpp"
#include <algorithm>
#include <cmath>

// LiquidityTimeSlice implementation
double LiquidityTimeSlice::getDisplayValue(double price, bool isBid, int displayMode) const {
    const auto& metrics = isBid ? bidMetrics : askMetrics;
    auto it = metrics.find(price);
    if (it == metrics.end()) return 0.0;
    
    switch (static_cast<LiquidityTimeSeriesEngine::LiquidityDisplayMode>(displayMode)) {
        case LiquidityTimeSeriesEngine::LiquidityDisplayMode::Average: 
            return it->second.avgLiquidity;
        case LiquidityTimeSeriesEngine::LiquidityDisplayMode::Maximum: 
            return it->second.maxLiquidity;
        case LiquidityTimeSeriesEngine::LiquidityDisplayMode::Resting: 
            return it->second.restingLiquidity;
        case LiquidityTimeSeriesEngine::LiquidityDisplayMode::Total: 
            return it->second.totalLiquidity;
        default: 
            return it->second.avgLiquidity;
    }
}

// LiquidityTimeSeriesEngine implementation
LiquidityTimeSeriesEngine::LiquidityTimeSeriesEngine(QObject* parent)
    : QObject(parent)
{
    // Initialize empty deques for all timeframes
    for (int64_t timeframe : m_timeframes) {
        m_timeSlices[timeframe] = std::deque<LiquidityTimeSlice>();
    }
    
    sLog_Init("🎯 LiquidityTimeSeriesEngine: Initialized with " << m_timeframes.size() << " timeframes");
    sLog_Init("   Base resolution: " << m_baseTimeframe_ms << "ms");
    sLog_Init("   Price resolution: $" << m_priceResolution);
    sLog_Init("   Max history per timeframe: " << m_maxHistorySlices << " slices");
}

void LiquidityTimeSeriesEngine::addOrderBookSnapshot(const OrderBook& book) {
    // Default: no viewport filtering (backward compatibility)
    addOrderBookSnapshot(book, -999999.0, 999999.0);
}

void LiquidityTimeSeriesEngine::addOrderBookSnapshot(const OrderBook& book, double minPrice, double maxPrice) {
    if (book.product_id.empty()) return;
    
    // Create a mutable copy to apply the depth limit
    OrderBook limitedBook = book;
    
    // 🚀 PERFORMANCE OPTIMIZATION: Apply depth limit to the raw order book data
    // This is the primary fix for CPU bottlenecking with large books.
    if (limitedBook.bids.size() > m_depthLimit) {
        limitedBook.bids.resize(m_depthLimit);
    }
    if (limitedBook.asks.size() > m_depthLimit) {
        limitedBook.asks.resize(m_depthLimit);
    }
    
    OrderBookSnapshot snapshot;
    snapshot.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    
    // 🚀 PERFORMANCE FIX: Only store price levels within viewport + buffer
    // TODO: Figure out how to do this in a more efficient way. You zoom out past the buffer, and then you get gaps in the heatmap.
    double priceBuffer = (maxPrice - minPrice) * 0.5;  // 50% buffer for smooth panning
    double filteredMinPrice = minPrice - priceBuffer;
    double filteredMaxPrice = maxPrice + priceBuffer;
    
    int originalBidCount = 0, originalAskCount = 0;
    int filteredBidCount = 0, filteredAskCount = 0;
    
    // Convert OrderBook to snapshot format with price quantization AND viewport filtering
    for (const auto& bid : limitedBook.bids) { // Use the limited book
        originalBidCount++;
        if (bid.price >= filteredMinPrice && bid.price <= filteredMaxPrice) {
            double quantizedPrice = quantizePrice(bid.price);
            snapshot.bids[quantizedPrice] += bid.size;  // Aggregate if multiple orders at same quantized price
            filteredBidCount++;
        }
    }
    for (const auto& ask : limitedBook.asks) { // Use the limited book
        originalAskCount++;
        if (ask.price >= filteredMinPrice && ask.price <= filteredMaxPrice) {
            double quantizedPrice = quantizePrice(ask.price);
            snapshot.asks[quantizedPrice] += ask.size;
            filteredAskCount++;
        }
    }
    
    m_snapshots.push_back(snapshot);
    
    // Update all timeframes with new snapshot
    updateAllTimeframes(snapshot);
    
    // Cleanup old data to prevent memory bloat
    cleanupOldData();
    
    // Debug logging for first few snapshots
    static int snapshotCount = 0;
    if (++snapshotCount <= 5) {
        sLog_Cache("🎯 VIEWPORT-FILTERED SNAPSHOT #" << snapshotCount 
                 << " OriginalBids:" << originalBidCount << " FilteredBids:" << filteredBidCount
                 << " OriginalAsks:" << originalAskCount << " FilteredAsks:" << filteredAskCount
                 << " PriceRange: $" << filteredMinPrice << "-$" << filteredMaxPrice
                 << " timestamp:" << snapshot.timestamp_ms);
    }
}

const LiquidityTimeSlice* LiquidityTimeSeriesEngine::getTimeSlice(int64_t timeframe_ms, int64_t timestamp_ms) const {
    auto tf_it = m_timeSlices.find(timeframe_ms);
    if (tf_it == m_timeSlices.end()) return nullptr;
    
    // Find slice containing this timestamp
    for (const auto& slice : tf_it->second) {
        if (timestamp_ms >= slice.startTime_ms && timestamp_ms < slice.endTime_ms) {
            return &slice;
        }
    }
    return nullptr;
}

std::vector<const LiquidityTimeSlice*> LiquidityTimeSeriesEngine::getVisibleSlices(
    int64_t timeframe_ms, int64_t viewStart_ms, int64_t viewEnd_ms) const {
    
    std::vector<const LiquidityTimeSlice*> visible;
    auto tf_it = m_timeSlices.find(timeframe_ms);
    if (tf_it == m_timeSlices.end()) return visible;
    
    for (const auto& slice : tf_it->second) {
        if (slice.endTime_ms >= viewStart_ms && slice.startTime_ms <= viewEnd_ms) {
            visible.push_back(&slice);
        }
    }
    
    // Also include current slice if it overlaps with viewport
    auto current_it = m_currentSlices.find(timeframe_ms);
    if (current_it != m_currentSlices.end()) {
        const auto& currentSlice = current_it->second;
        if (currentSlice.endTime_ms >= viewStart_ms && currentSlice.startTime_ms <= viewEnd_ms) {
            visible.push_back(&currentSlice);
        }
    }
    
    return visible;
}

void LiquidityTimeSeriesEngine::addTimeframe(int64_t duration_ms) {
    if (std::find(m_timeframes.begin(), m_timeframes.end(), duration_ms) == m_timeframes.end()) {
        m_timeframes.push_back(duration_ms);
        std::sort(m_timeframes.begin(), m_timeframes.end());
        
        // Initialize empty deque for this timeframe
        m_timeSlices[duration_ms] = std::deque<LiquidityTimeSlice>();
        
        // Rebuild historical data for new timeframe from base snapshots
        rebuildTimeframe(duration_ms);
        
        sLog_Init("🎯 Added timeframe: " << duration_ms << "ms");
    }
}

void LiquidityTimeSeriesEngine::removeTimeframe(int64_t duration_ms) {
    auto it = std::find(m_timeframes.begin(), m_timeframes.end(), duration_ms);
    if (it != m_timeframes.end()) {
        m_timeframes.erase(it);
        m_timeSlices.erase(duration_ms);
        m_currentSlices.erase(duration_ms);
        
        sLog_Init("🎯 Removed timeframe: " << duration_ms << "ms");
    }
}

std::vector<int64_t> LiquidityTimeSeriesEngine::getAvailableTimeframes() const {
    return m_timeframes;
}

int64_t LiquidityTimeSeriesEngine::suggestTimeframe(int64_t viewStart_ms, int64_t viewEnd_ms, int maxSlices) const {
    if (viewStart_ms >= viewEnd_ms || maxSlices <= 0) {
        return m_baseTimeframe_ms;  // Fallback to base timeframe
    }
    
    int64_t viewTimeSpan = viewEnd_ms - viewStart_ms;
    
    // 🐛 FIX: Find the FINEST timeframe that won't exceed maxSlices (iterate forward, not backward)
    for (int64_t timeframe : m_timeframes) {  // Forward iteration = finest to coarsest
        int64_t expectedSlices = viewTimeSpan / timeframe;
        
        if (expectedSlices <= maxSlices) {
            // Ensure this timeframe has data available
            auto tf_it = m_timeSlices.find(timeframe);
            if (tf_it != m_timeSlices.end() && !tf_it->second.empty()) {
                sLog_RenderDetail("🚀 SUGGEST TIMEFRAME: " << timeframe << "ms for span " 
                         << viewTimeSpan << "ms (" << expectedSlices << "/" << maxSlices << " slices, FINEST available)");
                return timeframe;
            } else {
                sLog_RenderDetail("🔍 SKIPPING TIMEFRAME: " << timeframe << "ms (no data available)");
            }
        } else {
            sLog_RenderDetail("🔍 SKIPPING TIMEFRAME: " << timeframe << "ms (" << expectedSlices << " > " << maxSlices << " slices)");
        }
    }
    
    // Fallback: use the finest available timeframe with data
    for (int64_t timeframe : m_timeframes) {
        auto tf_it = m_timeSlices.find(timeframe);
        if (tf_it != m_timeSlices.end() && !tf_it->second.empty()) {
            sLog_RenderDetail("🚀 FALLBACK TIMEFRAME: " << timeframe << "ms (finest with data)");
            return timeframe;
        }
    }
    
    return m_baseTimeframe_ms;  // Ultimate fallback
}

void LiquidityTimeSeriesEngine::setDisplayMode(LiquidityDisplayMode mode) {
    if (m_displayMode != mode) {
        m_displayMode = mode;
        emit displayModeChanged(mode);
        
        sLog_Init("🎯 Display mode changed to: " << 
                 (mode == LiquidityDisplayMode::Average ? "Average" :
                  mode == LiquidityDisplayMode::Maximum ? "Maximum" :
                  mode == LiquidityDisplayMode::Resting ? "Resting" : "Total"));
    }
}

// Private implementation methods

void LiquidityTimeSeriesEngine::updateAllTimeframes(const OrderBookSnapshot& snapshot) {
    for (int64_t timeframe_ms : m_timeframes) {
        updateTimeframe(timeframe_ms, snapshot);
    }
}

void LiquidityTimeSeriesEngine::updateTimeframe(int64_t timeframe_ms, const OrderBookSnapshot& snapshot) {
    // Get or create current slice for this timeframe
    auto& currentSlice = m_currentSlices[timeframe_ms];
    
    // Check if we need to start a new slice
    int64_t sliceStart = (snapshot.timestamp_ms / timeframe_ms) * timeframe_ms;
    
    if (currentSlice.startTime_ms == 0 || sliceStart != currentSlice.startTime_ms) {
        // Finalize previous slice if it exists
        if (currentSlice.startTime_ms != 0) {
            finalizeLiquiditySlice(currentSlice);
            m_timeSlices[timeframe_ms].push_back(currentSlice);
            
            emit timeSliceReady(timeframe_ms, currentSlice);
        }
        
        // Start new slice
        currentSlice = LiquidityTimeSlice();
        currentSlice.startTime_ms = sliceStart;
        currentSlice.endTime_ms = sliceStart + timeframe_ms;
        currentSlice.duration_ms = timeframe_ms;
    }
    
    // Add snapshot data to current slice
    addSnapshotToSlice(currentSlice, snapshot);
}

void LiquidityTimeSeriesEngine::addSnapshotToSlice(LiquidityTimeSlice& slice, const OrderBookSnapshot& snapshot) {
    // Process bids
    for (const auto& [price, size] : snapshot.bids) {
        auto& metrics = slice.bidMetrics[price];
        updatePriceLevelMetrics(metrics, size, snapshot.timestamp_ms, slice);
    }
    
    // Process asks
    for (const auto& [price, size] : snapshot.asks) {
        auto& metrics = slice.askMetrics[price];
        updatePriceLevelMetrics(metrics, size, snapshot.timestamp_ms, slice);
    }
    
    // Handle disappearing levels (important for resting liquidity calculation)
    updateDisappearingLevels(slice, snapshot);
}

void LiquidityTimeSeriesEngine::updatePriceLevelMetrics(
    LiquidityTimeSlice::PriceLevelMetrics& metrics, 
    double liquidity, 
    int64_t timestamp_ms,
    const LiquidityTimeSlice& slice) {
    
    if (metrics.snapshotCount == 0) {
        metrics.firstSeen_ms = timestamp_ms;
        metrics.minLiquidity = liquidity;
    }
    
    metrics.snapshotCount++;
    metrics.totalLiquidity += liquidity;
    metrics.maxLiquidity = std::max(metrics.maxLiquidity, liquidity);
    metrics.minLiquidity = std::min(metrics.minLiquidity, liquidity);
    metrics.lastSeen_ms = timestamp_ms;
    
    // Calculate running average
    metrics.avgLiquidity = metrics.totalLiquidity / metrics.snapshotCount;
    
    // Resting liquidity: only count if present for significant portion of interval
    if (metrics.wasConsistent()) {
        metrics.restingLiquidity = metrics.avgLiquidity;
    }
}

void LiquidityTimeSeriesEngine::updateDisappearingLevels(LiquidityTimeSlice& slice, const OrderBookSnapshot& snapshot) {
    // Mark existing levels that are no longer present
    for (auto& [price, metrics] : slice.bidMetrics) {
        if (snapshot.bids.find(price) == snapshot.bids.end()) {
            // Price level disappeared - update last seen time but don't add to totals
            metrics.lastSeen_ms = snapshot.timestamp_ms;
        }
    }
    
    for (auto& [price, metrics] : slice.askMetrics) {
        if (snapshot.asks.find(price) == snapshot.asks.end()) {
            metrics.lastSeen_ms = snapshot.timestamp_ms;
        }
    }
}

void LiquidityTimeSeriesEngine::finalizeLiquiditySlice(LiquidityTimeSlice& slice) {
    // Final calculations for all price levels
    for (auto& [price, metrics] : slice.bidMetrics) {
        // Calculate final resting liquidity based on persistence
        if (metrics.persistenceRatio(slice.duration_ms) > 0.8) {  // Present for >80% of interval
            metrics.restingLiquidity = metrics.avgLiquidity;
        } else {
            metrics.restingLiquidity = 0.0;  // Too sporadic, likely spoofing
        }
    }
    
    for (auto& [price, metrics] : slice.askMetrics) {
        if (metrics.persistenceRatio(slice.duration_ms) > 0.8) {
            metrics.restingLiquidity = metrics.avgLiquidity;
        } else {
            metrics.restingLiquidity = 0.0;
        }
    }
    
    // Debug logging for first few slices
    static int sliceCount = 0;
    if (++sliceCount <= 5) {
        sLog_Cache("🎯 FINALIZED SLICE #" << sliceCount
                 << " Duration:" << slice.duration_ms << "ms"
                 << " BidLevels:" << slice.bidMetrics.size()
                 << " AskLevels:" << slice.askMetrics.size()
                 << " Time:[" << slice.startTime_ms << "-" << slice.endTime_ms << "]");
    }
}

void LiquidityTimeSeriesEngine::rebuildTimeframe(int64_t timeframe_ms) {
    if (m_snapshots.empty()) return;
    
    auto& slices = m_timeSlices[timeframe_ms];
    slices.clear();
    
    // Group snapshots by time buckets
    std::map<int64_t, std::vector<OrderBookSnapshot>> buckets;
    
    for (const auto& snapshot : m_snapshots) {
        int64_t bucketStart = (snapshot.timestamp_ms / timeframe_ms) * timeframe_ms;
        buckets[bucketStart].push_back(snapshot);
    }
    
    // Create slices from buckets
    for (const auto& [bucketStart, snapshots] : buckets) {
        LiquidityTimeSlice slice;
        slice.startTime_ms = bucketStart;
        slice.endTime_ms = bucketStart + timeframe_ms;
        slice.duration_ms = timeframe_ms;
        
        for (const auto& snapshot : snapshots) {
            addSnapshotToSlice(slice, snapshot);
        }
        
        finalizeLiquiditySlice(slice);
        slices.push_back(slice);
    }
    
    sLog_Init("🎯 Rebuilt timeframe " << timeframe_ms << "ms: " << slices.size() << " slices");
}

void LiquidityTimeSeriesEngine::cleanupOldData() {
    // Keep only recent snapshots (enough to rebuild timeframes)
    if (!m_timeframes.empty()) {
        size_t maxSnapshots = m_maxHistorySlices * (m_timeframes.back() / m_baseTimeframe_ms);
        while (m_snapshots.size() > maxSnapshots) {
            m_snapshots.pop_front();
        }
    }
    
    // Cleanup old slices
    for (auto& [timeframe, slices] : m_timeSlices) {
        while (slices.size() > m_maxHistorySlices) {
            slices.pop_front();
        }
    }
}

double LiquidityTimeSeriesEngine::quantizePrice(double price) const {
    return std::round(price / m_priceResolution) * m_priceResolution;
}