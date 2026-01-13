/*
Sentinel — DataProcessor
Role: Implements the background data processing loop for the V2 rendering architecture.
Inputs/Outputs: Queues incoming data and processes it in batches using LiquidityTimeSeriesEngine.
Threading: The constructor moves the object to a worker QThread; processQueue runs on that thread.
Performance: A timer-driven loop processes data in batches, trading latency for throughput.
Integration: The concrete implementation of the V2 asynchronous data processing pipeline.
Observability: Logs the number of items processed in each batch via sLog_Render.
Related: DataProcessor.hpp, LiquidityTimeSeriesEngine.h.
Assumptions: The processing interval is a good balance between latency and efficiency.
*/
#include "DataProcessor.hpp"
#include "GridViewState.hpp"
#include "SentinelLogging.hpp"
#include <QMetaObject>
#include <QMetaType>
#include <QDateTime>
#include <cmath>
#include "SentinelLogging.hpp"
// #include "../../core/marketdata/cache/DataCache.hpp" // Replaced by IGridDataSource
#include "../CoordinateSystem.h"
#include <QColor>
#include <chrono>
#include <limits>
#include <climits>
#include <cmath>
#include <algorithm>
#include <cstring>

namespace {
constexpr bool kTraceCellDebug = false;
constexpr bool kTraceCoordinateDebug = false;
}

DataProcessor::DataProcessor(QObject* parent)
    : QObject(parent) {
    
    m_snapshotTimer = new QTimer(this);
    connect(m_snapshotTimer, &QTimer::timeout, this, &DataProcessor::captureOrderBookSnapshot);
    
    m_liquidityEngine = new LiquidityTimeSeriesEngine(this);
    connect(m_liquidityEngine, &LiquidityTimeSeriesEngine::timeSliceReady,
            this, &DataProcessor::onTimeSliceReady);
    
    // Remote-only mode: disable local heatmap paths by default to prevent column size
    // conflicts between local (8192) and server (variable) grid heights.
    m_remoteHeatmapActive = true;
    
    sLog_App("DataProcessor: Initialized for V2 architecture (remote-only heatmap)");
}

DataProcessor::~DataProcessor() {
    // Log even if stopProcessing() returns early (idempotent)
    if (!m_shuttingDown.load()) {
        sLog_App("DataProcessor destructor - stopProcessing() not called yet");
    }
    stopProcessing();
    // This log will always appear, even if stopProcessing() returned early
    sLog_App("DataProcessor destructor complete");
}

void DataProcessor::startProcessing() {
    // Base 100ms sampler: ensure continuous time buckets
    // Rate at which we sample the order book with 'captureOrderBookSnapshot'
    sLog_App("DataProcessor: Starting 100ms base sampler");
    if (m_snapshotTimer && !m_snapshotTimer->isActive()) {
        m_snapshotTimer->start(100);
        sLog_App("DataProcessor: Started processing with 100ms snapshots");
    }
}

void DataProcessor::stopProcessing() {
    // Idempotent: if already shutting down, return immediately
    bool expected = false;
    if (!m_shuttingDown.compare_exchange_strong(expected, true)) {
        // Already shutting down, skip redundant work
        return;
    }
    
    sLog_App("DataProcessor::stopProcessing() - shutting down...");
    
    // Stop timer first to prevent new processing
    if (m_snapshotTimer) {
        m_snapshotTimer->stop();
    }
    
    // Disconnect all signals to prevent callbacks during shutdown
    disconnect(this, nullptr, nullptr, nullptr);
    
    // Clear data to free memory
    clearData();
    
    sLog_App("DataProcessor stopped");
}

void DataProcessor::onTradeReceived(const Trade& trade) {
    // Early return if shutting down
    if (m_shuttingDown.load()) {
        return;
    }
    
    if (trade.product_id.empty()) return;
    
    {
        std::lock_guard<std::mutex> lock(m_dataMutex);
        
        if (m_viewState && !m_viewState->isTimeWindowValid()) {
            initializeViewportFromTrade(trade);
        }
        
        // sLog_Data("DataProcessor TRADE UPDATE: Processing trade");
    }
    
    // sLog_Data("DataProcessor TRADE: $" << trade.price 
    //              << " vol:" << trade.size 
    //              << " timestamp:" << timestamp);
}

void DataProcessor::onOrderBookUpdated(std::shared_ptr<const OrderBook> book) {
    // Early return if shutting down
    if (m_shuttingDown.load()) {
        return;
    }
    
    if (!book || book->product_id.empty() || book->bids.empty() || book->asks.empty()) return;
    
    {
        std::lock_guard<std::mutex> lock(m_dataMutex);
        
        m_latestOrderBook = book;
        m_hasValidOrderBook = true;
        
        if (m_viewState && !m_viewState->isTimeWindowValid()) {
            initializeViewportFromOrderBook(*book);
        }
    }
    
    // Recompute visible cells and publish a snapshot for the renderer
    updateVisibleCells();
    
    sLog_Data("DataProcessor ORDER BOOK update"
             << " Bids:" << book->bids.size() << " Asks:" << book->asks.size());
}

const OrderBook& DataProcessor::getLatestOrderBook() const {
    std::lock_guard<std::mutex> lock(m_dataMutex);
    // This might be problematic if m_latestOrderBook is null
    // but for now we assume it's valid when called.
    static OrderBook emptyBook;
    return m_latestOrderBook ? *m_latestOrderBook : emptyBook;
}

void DataProcessor::captureOrderBookSnapshot() {
    // TODO: Optimize this function
    /*  
    This function captures the current state of an order book and aligns it to a 100ms time bucket.
    If there are skipped buckets since the last snapshot (e.g., no activity in the last 100ms),
    it carries forward the latest order book state into each bucket up to the current time.
    This ensures that order book snapshots are recorded at consistent intervals in the liquidity engine.
    The periodic snapshot promotes proper time alignment for rendering historical order flow and downstream analysis.
    Early exits occur if the system is shutting down or if no valid liquidity engine exists.
    */

    // Early return if shutting down
    if (m_shuttingDown.load()) {
        return;
    }
    
    if (!m_liquidityEngine) return;
    
    std::shared_ptr<const OrderBook> book_copy;
    {
        std::lock_guard<std::mutex> lock(m_dataMutex);
        book_copy = m_latestOrderBook;
    }
    if (!book_copy) return;

    // Align to system 100ms buckets; carry-forward if we skipped buckets
    const qint64 nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const qint64 bucketStart = (nowMs / 100) * 100;

    // Use member variable instead of static - allows reset on clearData()
    if (m_lastSnapshotBucket == 0) {
        if (useRestingHeatmap()) {
            const auto& liveBook = m_dataSource->getDirectLiveOrderBook(book_copy->product_id);
            static thread_local std::vector<std::pair<uint32_t, double>> bidBuf;
            static thread_local std::vector<std::pair<uint32_t, double>> askBuf;
            const size_t maxPerSide = 20000;
            auto view = liveBook.captureDenseNonZero(bidBuf, askBuf, maxPerSide);
            emitRestingHeatmapColumn(view, bucketStart);
        } else {
            OrderBook ob = *book_copy;
            ob.timestamp = std::chrono::system_clock::time_point(std::chrono::milliseconds(bucketStart));
            m_liquidityEngine->addOrderBookSnapshot(ob);
        }
        m_lastSnapshotBucket = bucketStart;
        if (!useRestingHeatmap()) {
            updateVisibleCells();
        }
        return;
    }

    const bool fillGaps = qEnvironmentVariableIsSet("SENTINEL_HEATMAP_FILL_GAPS")
        || qEnvironmentVariableIsSet("SENTINEL_GPU_HEATMAP_ONLY");
    if (bucketStart > m_lastSnapshotBucket) {
        if (useRestingHeatmap()) {
            const auto& liveBook = m_dataSource->getDirectLiveOrderBook(book_copy->product_id);
            static thread_local std::vector<std::pair<uint32_t, double>> bidBuf;
            static thread_local std::vector<std::pair<uint32_t, double>> askBuf;
            const size_t maxPerSide = 20000;
            auto view = liveBook.captureDenseNonZero(bidBuf, askBuf, maxPerSide);

            if (fillGaps) {
                for (qint64 bucket = m_lastSnapshotBucket + 100; bucket <= bucketStart; bucket += 100) {
                    emitRestingHeatmapColumn(view, bucket);
                    m_lastSnapshotBucket = bucket;
                }
            } else {
                emitRestingHeatmapColumn(view, bucketStart);
                m_lastSnapshotBucket = bucketStart;
            }
        } else {
            if (fillGaps) {
                for (qint64 bucket = m_lastSnapshotBucket + 100; bucket <= bucketStart; bucket += 100) {
                    OrderBook ob = *book_copy;
                    ob.timestamp = std::chrono::system_clock::time_point(std::chrono::milliseconds(bucket));
                    m_liquidityEngine->addOrderBookSnapshot(ob);
                    m_lastSnapshotBucket = bucket;
                }
            } else {
                OrderBook ob = *book_copy;
                ob.timestamp = std::chrono::system_clock::time_point(std::chrono::milliseconds(bucketStart));
                m_liquidityEngine->addOrderBookSnapshot(ob);
                m_lastSnapshotBucket = bucketStart;
            }
            updateVisibleCells();
        }
    }
}

void DataProcessor::initializeViewportFromTrade(const Trade& trade) {
    if (!m_viewState) return;
    
    auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        trade.timestamp.time_since_epoch()).count();
    
    qint64 timeStart = timestamp - 30000;
    qint64 timeEnd = timestamp + 30000;
    double minPrice = trade.price - 100.0;
    double maxPrice = trade.price + 100.0;
    
    m_viewState->setViewport(timeStart, timeEnd, minPrice, maxPrice);
    
    sLog_App("DataProcessor VIEWPORT FROM TRADE: $" << minPrice << "-$" << maxPrice << " at " << timestamp);
    
    emit viewportInitialized();
}

void DataProcessor::initializeViewportFromOrderBook(const OrderBook& book) {
    if (!m_viewState) return;
    
    double bestBidPrice = std::numeric_limits<double>::quiet_NaN();
    if (!book.bids.empty()) {
        // Find the highest bid price (bids should be sorted highest to lowest)
        bestBidPrice = book.bids[0].price;
    }

    double bestAskPrice = std::numeric_limits<double>::quiet_NaN();
    if (!book.asks.empty()) {
        // Find the lowest ask price (asks should be sorted lowest to highest)  
        bestAskPrice = book.asks[0].price;
    }

    double midPrice;
    if (!std::isnan(bestBidPrice) && !std::isnan(bestAskPrice)) {
        midPrice = (bestBidPrice + bestAskPrice) / 2.0;
    } else if (!std::isnan(bestBidPrice)) {
        midPrice = bestBidPrice;
    } else if (!std::isnan(bestAskPrice)) {
        midPrice = bestAskPrice;
    } else {
        midPrice = 100000.0; // Default fallback for BTC
    }
    
    // Use the order book timestamp instead of system time for proper alignment
    auto bookTime = std::chrono::duration_cast<std::chrono::milliseconds>(
        book.timestamp.time_since_epoch()).count();
    
    qint64 timeStart = bookTime - 30000;
    qint64 timeEnd = bookTime + 30000;
    double minPrice = midPrice - 100.0;
    double maxPrice = midPrice + 100.0;
    
    m_viewState->setViewport(timeStart, timeEnd, minPrice, maxPrice);
    
    sLog_App("DataProcessor VIEWPORT FROM ORDER BOOK:");
    sLog_App("  Mid Price: $" << midPrice);
    sLog_App("  Price Window: $" << minPrice << " - $" << maxPrice);
    
    emit viewportInitialized();
}

void DataProcessor::onLiveOrderBookUpdated(const QString& productId, const std::vector<BookDelta>& deltas) {
    // Early return if shutting down
    if (m_shuttingDown.load()) {
        return;
    }

    if (m_remoteHeatmapActive) {
        return;
    }
    
    if (!m_dataSource || !m_liquidityEngine) {
        sLog_Render("DataProcessor: DataSource or LiquidityEngine not set");
        return;
    }
    
    const auto& liveBook = m_dataSource->getDirectLiveOrderBook(productId.toStdString());

    // Phase 1: Dense ingestion path (behind feature flag)
    if (m_useDenseIngestion) {
        static thread_local std::vector<std::pair<uint32_t, double>> bidBuf;
        static thread_local std::vector<std::pair<uint32_t, double>> askBuf;
        constexpr size_t kMaxPerSide = 4000; // bounded ingestion per side

        auto view = liveBook.captureDenseNonZero(bidBuf, askBuf, kMaxPerSide);
        if (!view.bidLevels.empty() || !view.askLevels.empty()) {
            m_liquidityEngine->addDenseSnapshot(view);
            
            // Also build sparse OrderBook for m_latestOrderBook (used by timer carry-forward)
            // This ensures captureOrderBookSnapshot() has fresh data when no updates arrive
            auto sparseForCarryForward = std::make_shared<OrderBook>();
            sparseForCarryForward->product_id = productId.toStdString();
            sparseForCarryForward->timestamp = view.timestamp;
            for (const auto& [idx, qty] : view.bidLevels) {
                double price = view.minPrice + (static_cast<double>(idx) * view.tickSize);
                sparseForCarryForward->bids.push_back({price, qty});
            }
            for (const auto& [idx, qty] : view.askLevels) {
                double price = view.minPrice + (static_cast<double>(idx) * view.tickSize);
                sparseForCarryForward->asks.push_back({price, qty});
            }
            
            {
                std::lock_guard<std::mutex> lock(m_dataMutex);
                m_latestOrderBook = sparseForCarryForward;
                m_hasValidOrderBook = true;
            }
            updateVisibleCells();
            return; // Do not execute sparse-banding path when dense path is enabled
        }
    }

    sLog_Render("DataProcessor processing dense LiveOrderBook - bids:" << liveBook.getBidCount() << " asks:" << liveBook.getAskCount());
    // Build a mid-centered banded sparse snapshot from dense book
    OrderBook sparseBook;
    sparseBook.product_id = productId.toStdString();
    sparseBook.timestamp = liveBook.getLastUpdate();  // Use book's timestamp, not now()

    const auto& denseBids = liveBook.getBids();
    const auto& denseAsks = liveBook.getAsks();

    // Determine best bid/ask
    double bestBid = std::numeric_limits<double>::quiet_NaN();
    double bestAsk = std::numeric_limits<double>::quiet_NaN();
    size_t bestBidIdx = 0;
    size_t bestAskIdx = 0;
    for (size_t i = denseBids.size(); i > 0; --i) {
        size_t idx = i - 1;
        if (denseBids[idx] > 0.0) { bestBidIdx = idx; bestBid = liveBook.getMinPrice() + (static_cast<double>(idx) * liveBook.getTickSize()); break; }
    }
    for (size_t i = 0; i < denseAsks.size(); ++i) {
        if (denseAsks[i] > 0.0) { bestAskIdx = i; bestAsk = liveBook.getMinPrice() + (static_cast<double>(i) * liveBook.getTickSize()); break; }
    }
    double mid = (!std::isnan(bestBid) && !std::isnan(bestAsk)) ? (bestBid + bestAsk) * 0.5
               : (!std::isnan(bestBid) ? bestBid : (!std::isnan(bestAsk) ? bestAsk : liveBook.getMinPrice() + 0.5 * (denseBids.size() * liveBook.getTickSize())));
    
    // Compute band width according to mode
    // TODO: Make band width dynamic per-asset and user-configurable, and eventually
    //       move banding to render-time (Phase 1+) so history isn't pruned at ingest.
    double halfBand = 0.0;
    switch (m_bandMode) {
        case BandMode::FixedDollar: halfBand = std::max(1e-6, m_bandValue); break;
        case BandMode::PercentMid:  halfBand = std::max(1e-6, std::abs(mid) * m_bandValue); break;
        case BandMode::Ticks:       halfBand = std::max(1.0, m_bandValue) * liveBook.getTickSize(); break;
    }
    // Ensure a sensible default if unset
    if (halfBand <= 0.0) {
        halfBand = 100.0; // $100 default window for BTC testing; TODO: derive from asset volatility/profile
    }
    // Clamp band to available data range
    const double maxHalfBand = (std::max(denseBids.size(), denseAsks.size()) * liveBook.getTickSize()) * 0.5;
    halfBand = std::min(halfBand, maxHalfBand);
    const double bandMinPrice = mid - halfBand;
    const double bandMaxPrice = mid + halfBand;

    // Convert bids (scan from highest to lowest within band)
    if (!denseBids.empty()) {
        size_t maxIndex = static_cast<size_t>(std::min<double>(denseBids.size(), std::ceil((bandMaxPrice - liveBook.getMinPrice()) / liveBook.getTickSize()) + 1));
        size_t minIndex = static_cast<size_t>(std::max<double>(0.0, std::floor((bandMinPrice - liveBook.getMinPrice()) / liveBook.getTickSize())));
        for (size_t i = maxIndex; i > minIndex && i > 0; --i) {
            size_t idx = i - 1;
            double qty = denseBids[idx];
            if (qty > 0.0) {
                double price = liveBook.getMinPrice() + (static_cast<double>(idx) * liveBook.getTickSize());
                sparseBook.bids.push_back({price, qty});
            }
        }
    }
    // Convert asks (scan from lowest to highest within band)
    if (!denseAsks.empty()) {
        size_t minIndex = static_cast<size_t>(std::max<double>(0.0, std::floor((bandMinPrice - liveBook.getMinPrice()) / liveBook.getTickSize())));
        size_t maxIndex = static_cast<size_t>(
            std::min<double>(denseAsks.size(), std::ceil((bandMaxPrice - liveBook.getMinPrice()) / liveBook.getTickSize()) + 1));
        for (size_t i = minIndex; i < maxIndex; ++i) {
            double qty = denseAsks[i];
            if (qty > 0.0) {
                double price = liveBook.getMinPrice() + (static_cast<double>(i) * liveBook.getTickSize());
                sparseBook.asks.push_back({price, qty});
            }
        }
    }

    // Fallback: if band yielded no levels, inject top-of-book so LTSE advances time
    if (sparseBook.bids.empty() && !std::isnan(bestBid)) {
        sparseBook.bids.push_back({bestBid, denseBids[bestBidIdx]});
    }
    if (sparseBook.asks.empty() && !std::isnan(bestAsk)) {
        sparseBook.asks.push_back({bestAsk, denseAsks[bestAskIdx]});
    }

    if (!sparseBook.bids.empty() || !sparseBook.asks.empty()) {
        m_liquidityEngine->addOrderBookSnapshot(sparseBook);
        {
            std::lock_guard<std::mutex> lock(m_dataMutex);
            m_latestOrderBook = std::make_shared<OrderBook>(sparseBook);
            m_hasValidOrderBook = true;
        }
        sLog_Data("DataProcessor: Primed LTSE with banded snapshot - bids=" << sparseBook.bids.size() << " asks=" << sparseBook.asks.size()
                 << " deltas=" << deltas.size());
    }
    updateVisibleCells();
}

void DataProcessor::clearData() {
    std::lock_guard<std::mutex> lock(m_dataMutex);
    
    m_latestOrderBook = nullptr;
    m_hasValidOrderBook = false;
    m_lastSnapshotBucket = 0;  // Reset bucket tracker for gap-filling
    
    // Clear all cell state to prevent stale data on symbol switch
    m_visibleCells.clear();
    m_processedTimeRanges.clear();
    m_lastProcessedTime = 0;
    m_lastViewportVersion = 0;
    
    if (m_viewState) {
        m_viewState->resetZoom();
    }
    
    {
        std::lock_guard<std::mutex> snapLock(m_snapshotMutex);
        m_publishedCells.reset();
    }
    emit dataUpdated();
}

void DataProcessor::updateVisibleCells() {
    // Early return if shutting down
    if (m_shuttingDown.load()) {
        return;
    }

    if (m_remoteHeatmapActive) {
        return;
    }

    if (qEnvironmentVariableIsSet("SENTINEL_GPU_HEATMAP_ONLY")) {
        return;
    }

    if (!m_viewState || !m_viewState->isTimeWindowValid()) return;
    
    // Viewport version gating: full rebuild only when viewport changes
    const uint64_t currentViewportVersion = m_viewState->getViewportVersion();
    const bool viewportChanged = (currentViewportVersion != m_lastViewportVersion);
    if (viewportChanged) {
        m_visibleCells.clear();
        m_processedTimeRanges.clear();
        m_lastProcessedTime = 0;
        m_lastViewportVersion = currentViewportVersion;
    }

    int64_t activeTimeframe = m_currentTimeframe_ms;
    
    // Use manual timeframe if set, otherwise auto-suggest
    // TODO: Investigate dynamic zoom/timeframe: ensure viewportChanged triggers re-suggest and manual override timeout resets
    if (!m_manualTimeframeSet || 
        (m_manualTimeframeTimer.isValid() && m_manualTimeframeTimer.elapsed() > 10000)) {  // 10 second timeout
        
        // Auto-suggest timeframe when no manual override
        if (m_liquidityEngine) {
            int64_t optimalTimeframe = m_liquidityEngine->suggestTimeframe(
                m_viewState->getVisibleTimeStart(), m_viewState->getVisibleTimeEnd(), 2000);
            
            if (optimalTimeframe != m_currentTimeframe_ms) {
                m_currentTimeframe_ms = optimalTimeframe;
                activeTimeframe = optimalTimeframe;
                sLog_Render("AUTO-TIMEFRAME UPDATE: " << optimalTimeframe << "ms (viewport-optimized)");
            }
        }
    } else {
        sLog_Render("MANUAL TIMEFRAME: Using " << m_currentTimeframe_ms << "ms (user-selected)");
    }
    
    // Get liquidity slices for active timeframe within viewport
    if (m_liquidityEngine) {
        qint64 timeStart = m_viewState->getVisibleTimeStart();
        qint64 timeEnd = m_viewState->getVisibleTimeEnd();
        sLog_Render("LTSE QUERY: timeframe=" << activeTimeframe << "ms, window=[" << timeStart << "-" << timeEnd << "]");
        
        auto visibleSlices = m_liquidityEngine->getVisibleSlices(activeTimeframe, timeStart, timeEnd);
        sLog_Render("LTSE RESULT: Found " << visibleSlices.size() << " slices for rendering");
        
        // Auto-fix viewport only when auto-scroll is enabled; never fight user pan/zoom
        // TODO: Only works on startup; need to fix for dynamic viewport changes
        if (visibleSlices.empty()) {
            const bool canAutoFix = (m_viewState && m_viewState->isAutoScrollEnabled());
            if (!canAutoFix) {
                sLog_Render("SKIP AUTO-ADJUST: auto-scroll disabled (user interaction in progress)");
            } else {
                auto allSlices = m_liquidityEngine->getVisibleSlices(activeTimeframe, 0, LLONG_MAX);
                if (!allSlices.empty()) {
                    qint64 oldestTime = allSlices.front()->startTime_ms;
                    qint64 newestTime = allSlices.back()->endTime_ms;
                    qint64 gap = timeStart - newestTime;
                    sLog_Render("LTSE TIME MISMATCH: Have " << allSlices.size() << " slices in range [" << oldestTime << "-" << newestTime << "], but viewport is [" << timeStart << "-" << timeEnd << "]");
                    sLog_Render("TIME GAP: " << gap << "ms between newest data and viewport start");
                    
                    // AUTO-FIX: Snap viewport to actual data range
                    if (gap > 60000) { // If gap > 1 minute, auto-adjust
                        qint64 newStart = newestTime - 30000; // 30s before newest data
                        qint64 newEnd = newestTime + 30000;   // 30s after newest data
                        sLog_Render("AUTO-ADJUSTING VIEWPORT: [" << newStart << "-" << newEnd << "] to match data");
                        
                        m_viewState->setViewport(newStart, newEnd, m_viewState->getMinPrice(), m_viewState->getMaxPrice());
                        
                        // Retry query with corrected viewport
                        visibleSlices = m_liquidityEngine->getVisibleSlices(activeTimeframe, newStart, newEnd);
                        sLog_Render("VIEWPORT FIX RESULT: Found " << visibleSlices.size() << " slices after adjustment");
                    }
                }
            }
        }

        // Track processed slices and append only new data when viewport is stable
        const size_t beforeSize = m_visibleCells.size();
        int processedSlices = 0;

        if (viewportChanged || m_lastProcessedTime == 0) {
            // Full rebuild: clear all cell state and process everything
            // (viewportChanged already cleared at top, but m_lastProcessedTime==0 needs it too)
            if (!viewportChanged) {
                m_visibleCells.clear();
            }
            m_processedTimeRanges.clear();
            for (const auto* slice : visibleSlices) {
                ++processedSlices;
                createCellsFromLiquiditySlice(*slice);
                m_processedTimeRanges.insert({slice->startTime_ms, slice->endTime_ms, slice->dataVersion});
                if (slice && slice->endTime_ms > m_lastProcessedTime) {
                    m_lastProcessedTime = slice->endTime_ms;
                }
            }
        } else {
            // Append mode: process only slices with NEW time ranges OR changed data versions
            // CRITICAL: LTSE reuses slice objects in memory, so we track by (time range + dataVersion)
            for (const auto* slice : visibleSlices) {
                if (!slice) continue;

                SliceTimeRange range{slice->startTime_ms, slice->endTime_ms, slice->dataVersion};

                // Check if we've already processed this exact (time + version) combination
                if (m_processedTimeRanges.find(range) == m_processedTimeRanges.end()) {
                    // Check if we have an OLD version of this time range that needs updating
                    for (auto it = m_processedTimeRanges.begin(); it != m_processedTimeRanges.end(); ) {
                        if (it->startTime == range.startTime && it->endTime == range.endTime) {
                            // Found old version - remove stale cells and tracking entry
                            sLog_Render("SLICE REPROCESS: time=[" << range.startTime << "-" << range.endTime 
                                        << "] version " << it->dataVersion << " -> " << range.dataVersion);
                            removeCellsForTimeRange(range.startTime, range.endTime);
                            it = m_processedTimeRanges.erase(it);
                        } else {
                            ++it;
                        }
                    }
                    
                    ++processedSlices;
                    createCellsFromLiquiditySlice(*slice);
                    m_processedTimeRanges.insert(range);
                }

                // Update last processed time to newest slice
                if (slice->endTime_ms > m_lastProcessedTime) {
                    m_lastProcessedTime = slice->endTime_ms;
                }
            }
        }

        // Do NOT prune off-viewport cells here; retain history so zoom-out can
        // immediately reveal older columns without requiring a recompute.

        // Changed if: viewport changed, cell count changed, OR any slices were (re)processed
        const bool changed = viewportChanged || (m_visibleCells.size() != beforeSize) || (processedSlices > 0);

        sLog_Render("SLICE PROCESSING: Processed " << processedSlices << "/" << visibleSlices.size() << " slices ("
                    << (viewportChanged ? "rebuild" : "append") << ")");
        sLog_Render("DATA PROCESSOR COVERAGE Slices:" << visibleSlices.size()
                    << " TotalCells:" << m_visibleCells.size()
                    << " ActiveTimeframe:" << activeTimeframe << "ms"
                    << " (Manual:" << (m_manualTimeframeSet ? "YES" : "NO") << ")");

        // Publish snapshot only when something changed
        if (changed) {
            {
                std::lock_guard<std::mutex> snapLock(m_snapshotMutex);
                m_publishedCells = std::make_shared<std::vector<CellInstance>>(m_visibleCells);
            }
            emit dataUpdated();
        }
        return; // Avoid duplicate publish below
    }
    
    // Fallback: if no liquidity engine, publish current state (likely empty)
    {
        std::lock_guard<std::mutex> snapLock(m_snapshotMutex);
        m_publishedCells = std::make_shared<std::vector<CellInstance>>(m_visibleCells);
    }
    emit dataUpdated();
}

void DataProcessor::onHeatmapSliceReceived(const QString& symbol,
                                           int64_t bucketStartMs,
                                           int64_t bucketEndMs,
                                           int64_t timeframeMs,
                                           double minPrice,
                                           double maxPrice,
                                           double tickSize,
                                           double midPrice,
                                           double lastTrade,
                                           const QString& format,
                                           const QByteArray& column,
                                           bool reset) {
    Q_UNUSED(symbol);
    Q_UNUSED(midPrice);
    Q_UNUSED(lastTrade);

    if (m_shuttingDown.load()) {
        return;
    }

    m_remoteHeatmapActive = true;
    if (qEnvironmentVariableIsSet("SENTINEL_HEATMAP_SLICE_LOG")) {
        sLog_Render("HEATMAP SLICE RX: tf=" << timeframeMs
                    << " rows=" << column.size()
                    << " reset=" << reset
                    << " format=" << format
                    << " current=" << m_currentTimeframe_ms
                    << " manual=" << m_manualTimeframeSet);
    }

    const QByteArray desiredTfEnv = qgetenv("SENTINEL_HEATMAP_TF");
    bool tfOk = false;
    const int64_t desiredTf = desiredTfEnv.toLongLong(&tfOk);
    if (tfOk && desiredTf > 0 && timeframeMs > 0 && timeframeMs != desiredTf) {
        return;
    }

    if (timeframeMs > 0 && m_currentTimeframe_ms != timeframeMs) {
        m_currentTimeframe_ms = timeframeMs;
        m_manualTimeframeSet = true;
        m_manualTimeframeTimer.restart();
    }

    if (column.isEmpty()) {
        return;
    }

    const bool isU8 = (format.compare(QStringLiteral("u8"), Qt::CaseInsensitive) == 0);
    const int prevHeight = m_heatmapGridHeight;
    int height = 0;
    QByteArray expanded;

    if (isU8) {
        height = column.size();
        if (height <= 0) {
            return;
        }
        expanded = column;
    } else {
        height = column.size() / 4;
        if (height <= 0) {
            return;
        }
        expanded = column;
    }

    m_heatmapGridHeight = height;
    const double effectiveTick = tickSize;
    const bool needsReset = reset || !m_heatmapRangeValid || (prevHeight != height);

    if (needsReset) {
        emit heatmapRangeReset(minPrice, maxPrice, effectiveTick, height);
    }

    m_heatmapRangeValid = true;
    emit heatmapColumnReady(bucketStartMs,
                            bucketEndMs,
                            timeframeMs,
                            minPrice,
                            maxPrice,
                            effectiveTick,
                            expanded);
}

void DataProcessor::setHeatmapGridHeight(int height) {
    if (height > 0) {
        m_heatmapGridHeight = height;
    }
}

void DataProcessor::setHeatmapIntensityScale(double scale) {
    if (scale > 0.0) {
        m_heatmapIntensityScale = scale;
    }
}

void DataProcessor::setHeatmapRecenterFraction(double fraction) {
    m_heatmapRecenterFraction = std::clamp(fraction, 0.01, 0.45);
}

void DataProcessor::onTimeSliceReady(int64_t timeframe_ms, const LiquidityTimeSlice& slice) {
    if (m_shuttingDown.load()) {
        return;
    }

    if (useRestingHeatmap()) {
        return;
    }

    const bool debug = qEnvironmentVariableIsSet("SENTINEL_GPU_HEATMAP_DEBUG");
    if (timeframe_ms != m_currentTimeframe_ms || m_heatmapGridHeight <= 0) {
        if (debug) {
            sLog_Render("GPU HEATMAP SKIP: tf=" << timeframe_ms
                        << " expected=" << m_currentTimeframe_ms
                        << " grid=" << m_heatmapGridHeight);
        }
        return;
    }

    updateHeatmapRangeForSlice(slice);
    if (!m_heatmapRangeValid || m_heatmapTickSize <= 0.0) {
        if (debug) {
            sLog_Render("GPU HEATMAP RANGE INVALID: min=" << m_heatmapMinPrice
                        << " max=" << m_heatmapMaxPrice
                        << " tick=" << m_heatmapTickSize);
        }
        return;
    }

    QByteArray columnData;
    int nonZeroRows = 0;
    rasterizeSliceToColumn(slice, columnData, m_heatmapMinPrice, m_heatmapMaxPrice, m_heatmapTickSize, &nonZeroRows);
    if (columnData.isEmpty()) {
        if (debug) {
            sLog_Render("GPU HEATMAP COLUMN EMPTY: slice=" << slice.startTime_ms);
        }
        return;
    }

    if (nonZeroRows == 0 && m_heatmapHasLastColumn) {
        columnData = m_heatmapLastColumn;
        if (debug) {
            sLog_Render("GPU HEATMAP COLUMN FILL: slice=" << slice.startTime_ms);
        }
    }

    if (debug) {
        sLog_Render("GPU HEATMAP COLUMN READY: tf=" << timeframe_ms
                    << " slice=" << slice.startTime_ms
                    << " bytes=" << columnData.size());
    }
    emit heatmapColumnReady(slice.startTime_ms,
                            slice.endTime_ms,
                            timeframe_ms,
                            m_heatmapMinPrice,
                            m_heatmapMaxPrice,
                            m_heatmapTickSize,
                            columnData);

    m_heatmapLastColumn = columnData;
    m_heatmapHasLastColumn = true;
}

bool DataProcessor::useRestingHeatmap() const {
    return qEnvironmentVariableIsSet("SENTINEL_HEATMAP_RESTING")
        || qEnvironmentVariableIsSet("SENTINEL_GPU_HEATMAP_ONLY");
}

void DataProcessor::updateHeatmapRangeForMid(double mid, double tickSize) {
    if (m_heatmapGridHeight <= 0 || tickSize <= 0.0) {
        return;
    }
    const double range = static_cast<double>(m_heatmapGridHeight) * tickSize;
    m_heatmapTickSize = tickSize;
    m_heatmapMinPrice = mid - range * 0.5;
    m_heatmapMaxPrice = m_heatmapMinPrice + range;
    m_heatmapRangeValid = true;
    emit heatmapRangeReset(m_heatmapMinPrice, m_heatmapMaxPrice, m_heatmapTickSize, m_heatmapGridHeight);
}

void DataProcessor::emitRestingHeatmapColumn(const LiveOrderBook::DenseBookSnapshotView& view,
                                             qint64 bucketStart) {
    const bool debug = qEnvironmentVariableIsSet("SENTINEL_GPU_HEATMAP_DEBUG");
    if (view.bidLevels.empty() && view.askLevels.empty()) {
        if (debug) {
            sLog_Render("GPU HEATMAP RESTING: empty book snapshot");
        }
        return;
    }

    double bestBid = std::numeric_limits<double>::quiet_NaN();
    double bestAsk = std::numeric_limits<double>::quiet_NaN();
    if (!view.bidLevels.empty()) {
        const auto& [idx, qty] = view.bidLevels.front();
        if (qty > 0.0) {
            bestBid = view.minPrice + (static_cast<double>(idx) * view.tickSize);
        }
    }
    if (!view.askLevels.empty()) {
        const auto& [idx, qty] = view.askLevels.front();
        if (qty > 0.0) {
            bestAsk = view.minPrice + (static_cast<double>(idx) * view.tickSize);
        }
    }

    double mid = 0.0;
    if (!std::isnan(bestBid) && !std::isnan(bestAsk)) {
        mid = (bestBid + bestAsk) * 0.5;
    } else if (!std::isnan(bestBid)) {
        mid = bestBid;
    } else if (!std::isnan(bestAsk)) {
        mid = bestAsk;
    } else {
        return;
    }

    const double tickSize = 1.0;
    updateHeatmapRangeForMid(mid, tickSize);

    QByteArray columnData;
    int nonZeroRows = 0;
    rasterizeBookToColumn(view, columnData, m_heatmapMinPrice, m_heatmapMaxPrice, tickSize, &nonZeroRows);
    if (columnData.isEmpty()) {
        if (debug) {
            sLog_Render("GPU HEATMAP RESTING: empty column");
        }
        return;
    }

    if (nonZeroRows == 0 && m_heatmapHasLastColumn) {
        columnData = m_heatmapLastColumn;
    }

    emit heatmapColumnReady(bucketStart,
                            bucketStart + 100,
                            100,
                            m_heatmapMinPrice,
                            m_heatmapMaxPrice,
                            tickSize,
                            columnData);

    m_heatmapLastColumn = columnData;
    m_heatmapHasLastColumn = true;
}

void DataProcessor::updateHeatmapRangeForSlice(const LiquidityTimeSlice& slice) {
    if (m_heatmapGridHeight <= 0) {
        return;
    }

    const double tickSize = 1.0;
    if (tickSize <= 0.0) {
        return;
    }

    const double sliceMinPrice = slice.tickToPrice(slice.minTick);
    const double sliceMaxPrice = slice.tickToPrice(slice.maxTick);
    const double mid = (sliceMinPrice + sliceMaxPrice) * 0.5;

    const double range = static_cast<double>(m_heatmapGridHeight) * tickSize;
    if (range <= 0.0) {
        return;
    }

    if (!m_heatmapRangeValid || m_heatmapTickSize != tickSize) {
        updateHeatmapRangeForMid(mid, tickSize);
        return;
    }

    const double margin = range * m_heatmapRecenterFraction;
    if (mid < m_heatmapMinPrice + margin || mid > m_heatmapMaxPrice - margin) {
        updateHeatmapRangeForMid(mid, tickSize);
    }
}

void DataProcessor::rasterizeSliceToColumn(const LiquidityTimeSlice& slice,
                                           QByteArray& out,
                                           double minPrice,
                                           double maxPrice,
                                           double tickSize,
                                           int* nonZeroRows) const {
    if (m_heatmapGridHeight <= 0 || tickSize <= 0.0 || maxPrice <= minPrice) {
        return;
    }

    const int height = m_heatmapGridHeight;
    out.resize(height * 4);
    std::memset(out.data(), 0, out.size());
    auto* dst = reinterpret_cast<uchar*>(out.data());

    const int displayMode = static_cast<int>(LiquidityTimeSeriesEngine::LiquidityDisplayMode::Total);
    const bool debug = qEnvironmentVariableIsSet("SENTINEL_GPU_HEATMAP_DEBUG");
    double maxValue = 0.0;
    for (size_t i = 0; i < slice.bidMetrics.size(); ++i) {
        const auto& metrics = slice.bidMetrics[i];
        if (metrics.snapshotCount <= 0) {
            continue;
        }
        const double price = slice.minTick * slice.tickSize + (static_cast<double>(i) * slice.tickSize);
        maxValue = std::max(maxValue, slice.getDisplayValue(price, true, displayMode));
    }
    for (size_t i = 0; i < slice.askMetrics.size(); ++i) {
        const auto& metrics = slice.askMetrics[i];
        if (metrics.snapshotCount <= 0) {
            continue;
        }
        const double price = slice.minTick * slice.tickSize + (static_cast<double>(i) * slice.tickSize);
        maxValue = std::max(maxValue, slice.getDisplayValue(price, false, displayMode));
    }

    const double denom = (maxValue > 0.0) ? maxValue : 1.0;
    const double invScale = (m_heatmapIntensityScale > 0.0)
        ? (1.0 / (denom * m_heatmapIntensityScale))
        : (1.0 / denom);
    if (debug) {
        sLog_Render("GPU HEATMAP COLUMN STATS: max=" << maxValue
                    << " invScale=" << invScale
                    << " range=$" << minPrice << "-$" << maxPrice);
    }

    int rowsWritten = 0;
    const auto writeRow = [&](double price, float value) {
        const double normalized = std::clamp(static_cast<double>(value) * invScale, 0.0, 1.0);
        if (normalized <= 0.0) {
            return;
        }
        const int row = static_cast<int>(std::floor((maxPrice - price) / tickSize));
        if (row < 0 || row >= height) {
            return;
        }
        const int intensity = static_cast<int>(normalized * 255.0);
        const int idx = row * 4;
        const int prev = dst[idx];
        if (intensity > prev) {
            dst[idx + 0] = static_cast<uchar>(intensity);
            dst[idx + 1] = static_cast<uchar>(intensity);
            dst[idx + 2] = static_cast<uchar>(intensity);
            dst[idx + 3] = static_cast<uchar>(255);
            if (prev == 0) {
                ++rowsWritten;
            }
        }
    };

    for (size_t i = 0; i < slice.bidMetrics.size(); ++i) {
        const auto& metrics = slice.bidMetrics[i];
        if (metrics.snapshotCount <= 0) {
            continue;
        }
        const double price = slice.minTick * slice.tickSize + (static_cast<double>(i) * slice.tickSize);
        const float value = static_cast<float>(slice.getDisplayValue(price, true, displayMode));
        writeRow(price, value);
    }

    for (size_t i = 0; i < slice.askMetrics.size(); ++i) {
        const auto& metrics = slice.askMetrics[i];
        if (metrics.snapshotCount <= 0) {
            continue;
        }
        const double price = slice.minTick * slice.tickSize + (static_cast<double>(i) * slice.tickSize);
        const float value = static_cast<float>(slice.getDisplayValue(price, false, displayMode));
        writeRow(price, value);
    }

    if (qEnvironmentVariableIsSet("SENTINEL_GPU_HEATMAP_DEBUG_MARK")) {
        for (int row = 0; row < height; ++row) {
            const int idx = row * 4;
            dst[idx + 0] = 255;
            dst[idx + 1] = 255;
            dst[idx + 2] = 255;
            dst[idx + 3] = 255;
        }
    }

    if (nonZeroRows) {
        *nonZeroRows = rowsWritten;
    }
}

void DataProcessor::rasterizeBookToColumn(const LiveOrderBook::DenseBookSnapshotView& view,
                                          QByteArray& out,
                                          double minPrice,
                                          double maxPrice,
                                          double tickSize,
                                          int* nonZeroRows) const {
    if (m_heatmapGridHeight <= 0 || tickSize <= 0.0 || maxPrice <= minPrice) {
        return;
    }

    const int height = m_heatmapGridHeight;
    std::vector<double> rowValues(height, 0.0);

    auto accumulate = [&](double price, double qty) {
        if (qty <= 0.0) {
            return;
        }
        const int row = static_cast<int>(std::floor((maxPrice - price) / tickSize));
        if (row < 0 || row >= height) {
            return;
        }
        rowValues[static_cast<size_t>(row)] += qty;
    };

    for (const auto& [idx, qty] : view.bidLevels) {
        const double price = view.minPrice + (static_cast<double>(idx) * view.tickSize);
        accumulate(price, qty);
    }
    for (const auto& [idx, qty] : view.askLevels) {
        const double price = view.minPrice + (static_cast<double>(idx) * view.tickSize);
        accumulate(price, qty);
    }

    double maxValue = 0.0;
    int rowsWritten = 0;
    for (const double value : rowValues) {
        if (value > maxValue) {
            maxValue = value;
        }
    }
    const double denom = (maxValue > 0.0) ? maxValue : 1.0;
    const double invScale = (m_heatmapIntensityScale > 0.0)
        ? (1.0 / (denom * m_heatmapIntensityScale))
        : (1.0 / denom);

    out.resize(height * 4);
    std::memset(out.data(), 0, out.size());
    auto* dst = reinterpret_cast<uchar*>(out.data());

    for (int row = 0; row < height; ++row) {
        const double normalized = std::clamp(rowValues[static_cast<size_t>(row)] * invScale, 0.0, 1.0);
        if (normalized <= 0.0) {
            continue;
        }
        const int intensity = static_cast<int>(normalized * 255.0);
        const int idx = row * 4;
        dst[idx + 0] = static_cast<uchar>(intensity);
        dst[idx + 1] = static_cast<uchar>(intensity);
        dst[idx + 2] = static_cast<uchar>(intensity);
        dst[idx + 3] = static_cast<uchar>(255);
        ++rowsWritten;
    }

    if (nonZeroRows) {
        *nonZeroRows = rowsWritten;
    }
}

void DataProcessor::createCellsFromLiquiditySlice(const LiquidityTimeSlice& slice) {
    if (!m_viewState) return;
    
    double minPrice = m_viewState->getMinPrice();
    double maxPrice = m_viewState->getMaxPrice();
    
    // Log slice processing details
    static int sliceCounter = 0;
    if (++sliceCounter % 10 == 0) {
        sLog_Render("SLICE DEBUG #" << sliceCounter << ": time=" << slice.startTime_ms 
                    << " bids=" << slice.bidMetrics.size() << " asks=" << slice.askMetrics.size() 
                    << " priceRange=$" << minPrice << "-$" << maxPrice);
    }
    
    // Create cells for bid levels (tick-based iteration)
    for (size_t i = 0; i < slice.bidMetrics.size(); ++i) {
        const auto& metrics = slice.bidMetrics[i];
        if (metrics.snapshotCount > 0) {
            double price = slice.minTick * slice.tickSize + (static_cast<double>(i) * slice.tickSize);
            if (price >= minPrice && price <= maxPrice) {
                createLiquidityCell(slice, price, slice.getDisplayValue(price, true, 0), true);
            }
        }
    }
    
    // Create cells for ask levels (tick-based iteration)
    for (size_t i = 0; i < slice.askMetrics.size(); ++i) {
        const auto& metrics = slice.askMetrics[i];
        if (metrics.snapshotCount > 0) {
            double price = slice.minTick * slice.tickSize + (static_cast<double>(i) * slice.tickSize);
            if (price >= minPrice && price <= maxPrice) {
                createLiquidityCell(slice, price, slice.getDisplayValue(price, false, 0), false);
            }
        }
    }
}

void DataProcessor::createLiquidityCell(const LiquidityTimeSlice& slice, double price, float liquidity, bool isBid) {
    if (liquidity <= 0.0f || !m_viewState) return;
    
    // World-space culling
    if (price < m_viewState->getMinPrice() || price > m_viewState->getMaxPrice()) return;
    if (slice.endTime_ms < m_viewState->getVisibleTimeStart() || slice.startTime_ms > m_viewState->getVisibleTimeEnd()) return;

    CellInstance cell;
    cell.timeStart_ms = slice.startTime_ms;
    cell.timeEnd_ms = (slice.endTime_ms > slice.startTime_ms) ? slice.endTime_ms : (slice.startTime_ms + std::max<int64_t>(m_currentTimeframe_ms, 1));
    const double halfTick = slice.tickSize * 0.5;
    cell.priceMin = price - halfTick;
    cell.priceMax = price + halfTick;
    cell.liquidity = liquidity;
    cell.isBid = isBid;

    m_visibleCells.push_back(cell);
    
    if constexpr (kTraceCellDebug) {
        static int cellCounter = 0;
        if (++cellCounter % 50 == 0) {
            sLog_Render("CELL DEBUG: Created cell #" << cellCounter << " world=[" 
                        << cell.timeStart_ms << "," << cell.timeEnd_ms << "] $[" 
                        << cell.priceMin << "," << cell.priceMax << "] liquidity=" << cell.liquidity);
        }
    }
}

void DataProcessor::removeCellsForTimeRange(int64_t startTime, int64_t endTime) {
    // Remove all cells that belong to a specific time slice (for reprocessing)
    const size_t beforeSize = m_visibleCells.size();
    m_visibleCells.erase(
        std::remove_if(m_visibleCells.begin(), m_visibleCells.end(),
            [startTime, endTime](const CellInstance& cell) {
                return cell.timeStart_ms == startTime && cell.timeEnd_ms == endTime;
            }),
        m_visibleCells.end()
    );
    const size_t removed = beforeSize - m_visibleCells.size();
    if (removed > 0) {
        sLog_Render("CELLS REMOVED: " << removed << " cells for time range [" << startTime << "-" << endTime << "]");
    }
}

QRectF DataProcessor::timeSliceToScreenRect(const LiquidityTimeSlice& slice, double price) const {
    if (!m_viewState) return QRectF();
    
    // Use CoordinateSystem for proper world-to-screen transformation
    Viewport viewport{
        m_viewState->getVisibleTimeStart(), m_viewState->getVisibleTimeEnd(),
        m_viewState->getMinPrice(), m_viewState->getMaxPrice(),
        m_viewState->getViewportWidth(), m_viewState->getViewportHeight()
    };

    // Use slice's tick size for vertical sizing to match aggregation
    const double halfTick = slice.tickSize * 0.5;

    // Ensure a non-zero time extent
    int64_t timeStart = slice.startTime_ms;
    int64_t timeEnd = slice.endTime_ms;
    if (timeEnd <= timeStart) {
        int64_t span = slice.duration_ms > 0 ? slice.duration_ms : std::max<int64_t>(m_currentTimeframe_ms, 1);
        timeEnd = timeStart + span;
    }

    // Use CoordinateSystem for proper transformation like old renderer
    QPointF topLeft = CoordinateSystem::worldToScreen(timeStart, price + halfTick, viewport);
    QPointF bottomRight = CoordinateSystem::worldToScreen(timeEnd, price - halfTick, viewport);
    
    QRectF result(topLeft, bottomRight);
    
    if constexpr (kTraceCoordinateDebug) {
        static int debugCounter = 0;
        if (++debugCounter % 100 == 0) {
            sLog_Render("COORD DEBUG: World("<< timeStart << "," << price << ") -> Screen("
                        << topLeft.x() << "," << topLeft.y() << ") Viewport[" 
                        << viewport.timeStart_ms << "-" << viewport.timeEnd_ms << ", $" 
                        << viewport.priceMin << "-$" << viewport.priceMax << "] Size[" 
                        << viewport.width << "x" << viewport.height << "]");
        }
    }
    
    return result;
}

void DataProcessor::setPriceResolution(double resolution) {
    if (m_liquidityEngine && resolution > 0) {
        m_liquidityEngine->setPriceResolution(resolution);
        emit dataUpdated();
    }
}

double DataProcessor::getPriceResolution() const {
    return m_liquidityEngine ? m_liquidityEngine->getPriceResolution() : 1.0;
}

void DataProcessor::addTimeframe(int timeframe_ms) {
    if (m_liquidityEngine) {
        m_liquidityEngine->addTimeframe(timeframe_ms);
    }
}

int64_t DataProcessor::suggestTimeframe(qint64 timeStart, qint64 timeEnd, int maxCells) const {
    return m_liquidityEngine ? m_liquidityEngine->suggestTimeframe(timeStart, timeEnd, maxCells) : 100;
}

std::vector<LiquidityTimeSlice> DataProcessor::getVisibleSlices(qint64 timeStart, qint64 timeEnd, double minPrice, double maxPrice) const {
    if (m_liquidityEngine) {
        auto slicePtrs = m_liquidityEngine->getVisibleSlices(m_currentTimeframe_ms, timeStart, timeEnd);
        std::vector<LiquidityTimeSlice> slices;
        slices.reserve(slicePtrs.size());
        for (const auto* ptr : slicePtrs) {
            if (ptr) slices.push_back(*ptr);
        }
        return slices;
    }
    return {};
}

int DataProcessor::getDisplayMode() const {
    return m_liquidityEngine ? static_cast<int>(m_liquidityEngine->getDisplayMode()) : 0;
}

void DataProcessor::setTimeframe(int timeframe_ms) {
    if (timeframe_ms > 0) {
        m_currentTimeframe_ms = timeframe_ms;
        m_manualTimeframeSet = true;
        m_manualTimeframeTimer.restart();
        
        if (m_liquidityEngine) {
            m_liquidityEngine->addTimeframe(timeframe_ms);
        }
        
        sLog_Render("MANUAL TIMEFRAME SET: " << timeframe_ms << "ms");
        emit dataUpdated();
    }
}

bool DataProcessor::isManualTimeframeSet() const {
    return m_manualTimeframeSet;
}
