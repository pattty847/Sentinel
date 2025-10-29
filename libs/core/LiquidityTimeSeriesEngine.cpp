/*
Sentinel — LiquidityTimeSeriesEngine
Role: Implements the logic for bucketing order book data into discrete time slices.
Inputs/Outputs: Updates liquidity metrics within a time slice based on incoming order book data.
Threading: Uses QReadWriteLock to manage concurrent access to the time-series data.
Performance: Includes a 'suggestTimeframe' method to dynamically adjust data resolution.
Integration: The concrete implementation of the renderer's core data aggregation engine.
Observability: Logs the creation of new timeframes and slices.
Related: LiquidityTimeSeriesEngine.h.
Assumptions: Time bucketing logic correctly assigns updates to their respective time slices.
*/
#include "LiquidityTimeSeriesEngine.h"
#include "SentinelLogging.hpp"
#include <algorithm>
#include <cmath>
#include <set>

// LiquidityTimeSlice implementation - O(1) tick-based access
double LiquidityTimeSlice::getDisplayValue(double price, bool isBid, int displayMode) const {
    const auto* metrics = getMetrics(price, isBid);
    if (!metrics) return 0.0;
    
    switch (static_cast<LiquidityTimeSeriesEngine::LiquidityDisplayMode>(displayMode)) {
        case LiquidityTimeSeriesEngine::LiquidityDisplayMode::Average: 
            return metrics->avgLiquidity;
        case LiquidityTimeSeriesEngine::LiquidityDisplayMode::Maximum: 
            return metrics->maxLiquidity;
        case LiquidityTimeSeriesEngine::LiquidityDisplayMode::Resting: 
            return metrics->restingLiquidity;
        case LiquidityTimeSeriesEngine::LiquidityDisplayMode::Total: 
            return metrics->totalLiquidity;
        default: 
            return metrics->avgLiquidity;
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
    
    sLog_App("🎯 LiquidityTimeSeriesEngine: Initialized with " << m_timeframes.size() << " timeframes");
    sLog_App("   Base resolution: " << m_baseTimeframe_ms << "ms");
    sLog_App("   Price resolution: $" << m_priceResolution);
    sLog_App("   Max history per timeframe: " << m_maxHistorySlices << " slices");
}

void LiquidityTimeSeriesEngine::addOrderBookSnapshot(const OrderBook& book) {
    // Default: no viewport filtering (backward compatibility)
    addOrderBookSnapshot(book, -999999.0, 999999.0);
}

// Dense ingestion path: convert compact non-zero indices into snapshot and reuse existing pipeline
void LiquidityTimeSeriesEngine::addDenseSnapshot(const LiveOrderBook::DenseBookSnapshotView& view) {
    OrderBookSnapshot snapshot;
    snapshot.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(view.timestamp.time_since_epoch()).count();

    // Quantize prices and aggregate. Use the same quantization as sparse path.
    for (const auto& [idx, qty] : view.bidLevels) {
        double price = view.minPrice + (static_cast<double>(idx) * view.tickSize);
        double qPrice = quantizePrice(price);
        snapshot.bids[qPrice] += qty;
    }
    for (const auto& [idx, qty] : view.askLevels) {
        double price = view.minPrice + (static_cast<double>(idx) * view.tickSize);
        double qPrice = quantizePrice(price);
        snapshot.asks[qPrice] += qty;
    }

    m_snapshots.push_back(snapshot);
    updateAllTimeframes(snapshot);
    cleanupOldData();
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
    
    // Convert sparse OrderBook to snapshot format with price quantization  
    for (const auto& bid : limitedBook.bids) {
        double quantizedPrice = quantizePrice(bid.price);
        snapshot.bids[quantizedPrice] += bid.size;  // Aggregate if multiple orders at same quantized price
    }
    for (const auto& ask : limitedBook.asks) {
        double quantizedPrice = quantizePrice(ask.price);
        snapshot.asks[quantizedPrice] += ask.size;
    }
    
    m_snapshots.push_back(snapshot);
    
    // Update all timeframes with new snapshot
    updateAllTimeframes(snapshot);
    
    // Cleanup old data to prevent memory bloat
    cleanupOldData();
    
    // Debug logging for first few snapshots
    static int snapshotCount = 0;
    if (++snapshotCount <= 5) {
        sLog_Data("🎯 GLOBAL SNAPSHOT #" << snapshotCount 
                 << " Bids:" << snapshot.bids.size() << " Asks:" << snapshot.asks.size()
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
        
        sLog_App("🎯 Added timeframe: " << duration_ms << "ms");
    }
}

void LiquidityTimeSeriesEngine::removeTimeframe(int64_t duration_ms) {
    auto it = std::find(m_timeframes.begin(), m_timeframes.end(), duration_ms);
    if (it != m_timeframes.end()) {
        m_timeframes.erase(it);
        m_timeSlices.erase(duration_ms);
        m_currentSlices.erase(duration_ms);
        
        sLog_App("🎯 Removed timeframe: " << duration_ms << "ms");
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
                // 🚀 ONLY LOG WHEN TIMEFRAME SUGGESTION CHANGES
                if (timeframe != m_lastSuggestedTimeframe) {
                    sLog_Render("🚀 SUGGEST TIMEFRAME: " << timeframe << "ms for span " 
                             << viewTimeSpan << "ms (" << expectedSlices << "/" << maxSlices << " slices, FINEST available)");
                    m_lastSuggestedTimeframe = timeframe;
                }
                return timeframe;
            } else {
                // Only log skipping if we haven't logged this specific timeframe being skipped before
                static std::set<int64_t> loggedSkippedTimeframes;
                if (loggedSkippedTimeframes.find(timeframe) == loggedSkippedTimeframes.end()) {
                    sLog_Render("🔍 SKIPPING TIMEFRAME: " << timeframe << "ms (no data available)");
                    loggedSkippedTimeframes.insert(timeframe);
                }
            }
        } else {
            // Only log skipping if we haven't logged this specific timeframe being skipped before
            static std::set<int64_t> loggedSkippedTimeframes;
            if (loggedSkippedTimeframes.find(timeframe) == loggedSkippedTimeframes.end()) {
                sLog_Render("🔍 SKIPPING TIMEFRAME: " << timeframe << "ms (" << expectedSlices << " > " << maxSlices << " slices)");
                loggedSkippedTimeframes.insert(timeframe);
            }
        }
    }
    
    // Fallback: use the finest available timeframe with data
    for (int64_t timeframe : m_timeframes) {
        auto tf_it = m_timeSlices.find(timeframe);
        if (tf_it != m_timeSlices.end() && !tf_it->second.empty()) {
            // 🚀 ONLY LOG WHEN TIMEFRAME SUGGESTION CHANGES
            if (timeframe != m_lastSuggestedTimeframe) {
                sLog_Render("🚀 FALLBACK TIMEFRAME: " << timeframe << "ms (finest with data)");
                m_lastSuggestedTimeframe = timeframe;
            }
            return timeframe;
        }
    }
    
    return m_baseTimeframe_ms;  // Ultimate fallback
}

void LiquidityTimeSeriesEngine::setDisplayMode(LiquidityDisplayMode mode) {
    if (m_displayMode != mode) {
        m_displayMode = mode;
        emit displayModeChanged(mode);
        
        sLog_App("🎯 Display mode changed to: " << 
                 (mode == LiquidityDisplayMode::Average ? "Average" :
                  mode == LiquidityDisplayMode::Maximum ? "Maximum" :
                  mode == LiquidityDisplayMode::Resting ? "Resting" : "Total"));
    }
}

// Private implementation methods

void LiquidityTimeSeriesEngine::updateAllTimeframes(const OrderBookSnapshot& snapshot) {    
    for (int64_t timeframe_ms : m_timeframes) {
        // Calculate the slice boundary for this timeframe
        int64_t sliceStart = (snapshot.timestamp_ms / timeframe_ms) * timeframe_ms;
        
        // Check if we've crossed into a new slice boundary for this timeframe
        auto lastUpdateIt = m_lastUpdateTimestamp.find(timeframe_ms);
        bool needsUpdate = false;
        
        if (lastUpdateIt == m_lastUpdateTimestamp.end()) {
            // First time processing this timeframe
            needsUpdate = true;
            m_lastUpdateTimestamp[timeframe_ms] = sliceStart;
        } else {
            int64_t lastSliceStart = (lastUpdateIt->second / timeframe_ms) * timeframe_ms;
            if (sliceStart != lastSliceStart) {
                // Crossed into a new slice boundary - need to update
                needsUpdate = true;
                m_lastUpdateTimestamp[timeframe_ms] = snapshot.timestamp_ms;
            }
        }
        
        // OPTIMIZATION: Only update timeframes that have crossed slice boundaries
        if (needsUpdate || timeframe_ms == m_baseTimeframe_ms) {
            // Always update base timeframe (100ms), only update others when needed
            updateTimeframe(timeframe_ms, snapshot);
        }
    }
    
    // Debug: Log efficiency gains for first few snapshots
    static int updateCount = 0;
    if (++updateCount <= 10) {
        int actualUpdates = 0;
        for (int64_t tf : m_timeframes) {
            int64_t sliceStart = (snapshot.timestamp_ms / tf) * tf;
            auto it = m_lastUpdateTimestamp.find(tf);
            if (it != m_lastUpdateTimestamp.end()) {
                int64_t lastSliceStart = (it->second / tf) * tf;
                if (sliceStart != lastSliceStart || tf == m_baseTimeframe_ms) {
                    actualUpdates++;
                }
            }
        }
        sLog_Data("🚀 Rolling optimization - " << actualUpdates << "/" << m_timeframes.size() 
                 << " timeframes updated (saved " << (m_timeframes.size() - actualUpdates) << " redundant updates)");
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
        
        // NEW SLICE LOGGING
        sLog_RenderN(50, "🧱 NEW SLICE " << timeframe_ms << "ms: [" << sliceStart
                     << "-" << (sliceStart + timeframe_ms) << "]");
        currentSlice.duration_ms = timeframe_ms;
    }
    
    // Add snapshot data to current slice
    addSnapshotToSlice(currentSlice, snapshot);
}

void LiquidityTimeSeriesEngine::addSnapshotToSlice(LiquidityTimeSlice& slice, const OrderBookSnapshot& snapshot) {
    ++m_globalSequence;
    // Guard against empty snapshots to avoid deriving invalid tick ranges
    if (snapshot.bids.empty() && snapshot.asks.empty()) {
        return; // Nothing to aggregate for this 100ms bucket
    }
    
    Tick minSnapshotTick = std::numeric_limits<Tick>::max();
    Tick maxSnapshotTick = std::numeric_limits<Tick>::min();
    
    // Find price range in snapshot
    for (const auto& [price, size] : snapshot.bids) {
        Tick tick = priceToTick(price);
        minSnapshotTick = std::min(minSnapshotTick, tick);
        maxSnapshotTick = std::max(maxSnapshotTick, tick);
    }
    for (const auto& [price, size] : snapshot.asks) {
        Tick tick = priceToTick(price);
        minSnapshotTick = std::min(minSnapshotTick, tick);
        maxSnapshotTick = std::max(maxSnapshotTick, tick);
    }
    
    // Expand slice tick range if needed
    if (slice.bidMetrics.empty() && slice.askMetrics.empty()) {
        // First snapshot in slice
        slice.minTick = minSnapshotTick;
        slice.maxTick = maxSnapshotTick;
        slice.tickSize = m_priceResolution;
        size_t range = static_cast<size_t>(maxSnapshotTick - minSnapshotTick + 1);
        slice.bidMetrics.resize(range);
        slice.askMetrics.resize(range);
    } else {
        // Expand existing range if needed
        Tick newMinTick = std::min(slice.minTick, minSnapshotTick);
        Tick newMaxTick = std::max(slice.maxTick, maxSnapshotTick);
        
        if (newMinTick < slice.minTick || newMaxTick > slice.maxTick) {
            // Need to expand vectors
            size_t newRange = static_cast<size_t>(newMaxTick - newMinTick + 1);
            std::vector<LiquidityTimeSlice::PriceLevelMetrics> newBidMetrics(newRange);
            std::vector<LiquidityTimeSlice::PriceLevelMetrics> newAskMetrics(newRange);
            
            // Copy existing data to new position
            size_t oldOffset = static_cast<size_t>(slice.minTick - newMinTick);
            for (size_t i = 0; i < slice.bidMetrics.size(); ++i) {
                newBidMetrics[oldOffset + i] = std::move(slice.bidMetrics[i]);
                newAskMetrics[oldOffset + i] = std::move(slice.askMetrics[i]);
            }
            
            slice.bidMetrics = std::move(newBidMetrics);
            slice.askMetrics = std::move(newAskMetrics);
            slice.minTick = newMinTick;
            slice.maxTick = newMaxTick;
        }
    }
    
    // Process bids
    for (const auto& [price, size] : snapshot.bids) {
        Tick tick = priceToTick(price);
        size_t index = static_cast<size_t>(tick - slice.minTick);
        if (index < slice.bidMetrics.size()) {
            updatePriceLevelMetrics(slice.bidMetrics[index], size, snapshot.timestamp_ms, slice);
            slice.bidMetrics[index].lastSeenSeq = m_globalSequence;
        }
    }
    
    // Process asks
    for (const auto& [price, size] : snapshot.asks) {
        Tick tick = priceToTick(price);
        size_t index = static_cast<size_t>(tick - slice.minTick);
        if (index < slice.askMetrics.size()) {
            updatePriceLevelMetrics(slice.askMetrics[index], size, snapshot.timestamp_ms, slice);
            slice.askMetrics[index].lastSeenSeq = m_globalSequence;
        }
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
    // No more O(L) scans - use sequence stamps for instant presence detection
    
    for (auto& metrics : slice.bidMetrics) {
        if (metrics.snapshotCount > 0 && metrics.lastSeenSeq != m_globalSequence) {
            // Price level disappeared this snapshot - update last seen time
            metrics.lastSeen_ms = snapshot.timestamp_ms;
        }
    }
    
    for (auto& metrics : slice.askMetrics) {
        if (metrics.snapshotCount > 0 && metrics.lastSeenSeq != m_globalSequence) {
            // Price level disappeared this snapshot - update last seen time
            metrics.lastSeen_ms = snapshot.timestamp_ms;
        }
    }
    
    // Debug: Log efficiency gains for first few snapshots
    static int disappearCount = 0;
    if (++disappearCount <= 5) {
        sLog_Data("🚀 O(1) version stamp disappearing level detection - sequence " << m_globalSequence);
    }
}

void LiquidityTimeSeriesEngine::finalizeLiquiditySlice(LiquidityTimeSlice& slice) {
    for (auto& metrics : slice.bidMetrics) {
        if (metrics.snapshotCount > 0) {  // Only process levels that have data
            // Calculate final resting liquidity based on persistence
            if (metrics.persistenceRatio(slice.duration_ms) > 0.8) {  // Present for >80% of interval
                metrics.restingLiquidity = metrics.avgLiquidity;
            } else {
                metrics.restingLiquidity = 0.0;  // Too sporadic, likely spoofing
            }
        }
    }
    
    for (auto& metrics : slice.askMetrics) {
        if (metrics.snapshotCount > 0) {  // Only process levels that have data
            if (metrics.persistenceRatio(slice.duration_ms) > 0.8) {
                metrics.restingLiquidity = metrics.avgLiquidity;
            } else {
                metrics.restingLiquidity = 0.0;
            }
        }
    }
    
    // Debug logging for first few slices
    static int sliceCount = 0;
    if (++sliceCount <= 5) {
        sLog_Data("🎯 FINALIZED SLICE #" << sliceCount
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
    
    sLog_App("🎯 Rebuilt timeframe " << timeframe_ms << "ms: " << slices.size() << " slices");
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