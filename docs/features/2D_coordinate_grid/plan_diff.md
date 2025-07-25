// ======= BOOKMAP-STYLE LIQUIDITY TIME SERIES SYSTEM =======

// 1. OrderBookSnapshot - Capture point-in-time liquidity
struct OrderBookSnapshot {
    int64_t timestamp_ms;
    std::map<double, double> bids;  // price -> size
    std::map<double, double> asks;  // price -> size
    
    // Helper to get liquidity at specific price
    double getBidLiquidity(double price) const {
        auto it = bids.find(price);
        return it != bids.end() ? it->second : 0.0;
    }
    
    double getAskLiquidity(double price) const {
        auto it = asks.find(price);
        return it != asks.end() ? it->second : 0.0;
    }
};

// 2. LiquidityTimeSlice - Aggregated liquidity for one time bucket
struct LiquidityTimeSlice {
    int64_t startTime_ms;
    int64_t endTime_ms;
    int64_t duration_ms;
    
    // Liquidity metrics per price level
    struct PriceLevelMetrics {
        double totalLiquidity = 0.0;        // Sum of all liquidity seen
        double avgLiquidity = 0.0;           // Average liquidity during interval
        double maxLiquidity = 0.0;           // Peak liquidity seen
        double minLiquidity = 0.0;           // Minimum liquidity (could be 0)
        double restingLiquidity = 0.0;       // Liquidity that stayed for full duration
        int snapshotCount = 0;               // How many snapshots included this price
        int64_t firstSeen_ms = 0;            // When this price level first appeared
        int64_t lastSeen_ms = 0;             // When this price level last had liquidity
        
        // Spoofing detection
        bool wasConsistent() const {
            return snapshotCount > duration_ms / 200;  // Present for >50% of snapshots
        }
        
        double persistenceRatio() const {
            if (endTime_ms <= firstSeen_ms) return 0.0;
            return static_cast<double>(lastSeen_ms - firstSeen_ms) / (endTime_ms - firstSeen_ms);
        }
    };
    
    std::map<double, PriceLevelMetrics> bidMetrics;
    std::map<double, PriceLevelMetrics> askMetrics;
    
    // Visualization data
    double getDisplayValue(double price, bool isBid, LiquidityDisplayMode mode) const {
        const auto& metrics = isBid ? bidMetrics : askMetrics;
        auto it = metrics.find(price);
        if (it == metrics.end()) return 0.0;
        
        switch (mode) {
            case LiquidityDisplayMode::Average: return it->second.avgLiquidity;
            case LiquidityDisplayMode::Maximum: return it->second.maxLiquidity;
            case LiquidityDisplayMode::Resting: return it->second.restingLiquidity;
            case LiquidityDisplayMode::Total: return it->second.totalLiquidity;
            default: return it->second.avgLiquidity;
        }
    }
};

// 3. Dynamic Timeframe System
class LiquidityTimeSeriesEngine {
public:
    enum class LiquidityDisplayMode {
        Average,    // Average liquidity during interval
        Maximum,    // Peak liquidity seen
        Resting,    // Only liquidity that persisted full duration (anti-spoof)
        Total       // Sum of all liquidity seen
    };
    
private:
    // Base 100ms snapshots (raw data)
    std::deque<OrderBookSnapshot> m_snapshots;
    
    // Dynamic timeframe configuration
    std::vector<int64_t> m_timeframes = {100, 250, 500, 1000, 2000, 5000, 10000}; // ms
    
    // Aggregated time slices for each timeframe
    std::map<int64_t, std::deque<LiquidityTimeSlice>> m_timeSlices;
    
    // Real-time "current" slices being built
    std::map<int64_t, LiquidityTimeSlice> m_currentSlices;
    
    // Configuration
    int64_t m_baseTimeframe_ms = 100;           // Snapshot interval
    size_t m_maxHistorySlices = 5000;           // Keep 5000 slices per timeframe
    double m_priceResolution = 1.0;             // $1 price buckets
    LiquidityDisplayMode m_displayMode = LiquidityDisplayMode::Average;
    
public:
    // Add new order book snapshot (called every ~100ms)
    void addOrderBookSnapshot(const OrderBook& book) {
        OrderBookSnapshot snapshot;
        snapshot.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        
        // Convert OrderBook to snapshot format
        for (const auto& bid : book.bids) {
            snapshot.bids[quantizePrice(bid.price)] = bid.size;
        }
        for (const auto& ask : book.asks) {
            snapshot.asks[quantizePrice(ask.price)] = ask.size;
        }
        
        m_snapshots.push_back(snapshot);
        
        // Update all timeframes
        updateAllTimeframes(snapshot);
        
        // Cleanup old data
        cleanupOldData();
    }
    
    // Get liquidity slice for specific timeframe at specific time
    const LiquidityTimeSlice* getTimeSlice(int64_t timeframe_ms, int64_t timestamp_ms) const {
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
    
    // Get all visible slices for a timeframe within viewport
    std::vector<const LiquidityTimeSlice*> getVisibleSlices(int64_t timeframe_ms, 
                                                           int64_t viewStart_ms, 
                                                           int64_t viewEnd_ms) const {
        std::vector<const LiquidityTimeSlice*> visible;
        auto tf_it = m_timeSlices.find(timeframe_ms);
        if (tf_it == m_timeSlices.end()) return visible;
        
        for (const auto& slice : tf_it->second) {
            if (slice.endTime_ms >= viewStart_ms && slice.startTime_ms <= viewEnd_ms) {
                visible.push_back(&slice);
            }
        }
        return visible;
    }
    
    // Dynamic timeframe management
    void addTimeframe(int64_t duration_ms) {
        if (std::find(m_timeframes.begin(), m_timeframes.end(), duration_ms) == m_timeframes.end()) {
            m_timeframes.push_back(duration_ms);
            std::sort(m_timeframes.begin(), m_timeframes.end());
            
            // Initialize empty deque for this timeframe
            m_timeSlices[duration_ms] = std::deque<LiquidityTimeSlice>();
            
            // Rebuild historical data for new timeframe from base snapshots
            rebuildTimeframe(duration_ms);
        }
    }
    
    void removeTimeframe(int64_t duration_ms) {
        auto it = std::find(m_timeframes.begin(), m_timeframes.end(), duration_ms);
        if (it != m_timeframes.end()) {
            m_timeframes.erase(it);
            m_timeSlices.erase(duration_ms);
            m_currentSlices.erase(duration_ms);
        }
    }
    
    std::vector<int64_t> getAvailableTimeframes() const {
        return m_timeframes;
    }
    
    void setDisplayMode(LiquidityDisplayMode mode) {
        m_displayMode = mode;
    }
    
private:
    // Update all configured timeframes with new snapshot
    void updateAllTimeframes(const OrderBookSnapshot& snapshot) {
        for (int64_t timeframe_ms : m_timeframes) {
            updateTimeframe(timeframe_ms, snapshot);
        }
    }
    
    // Update specific timeframe with new snapshot
    void updateTimeframe(int64_t timeframe_ms, const OrderBookSnapshot& snapshot) {
        // Get or create current slice for this timeframe
        auto& currentSlice = m_currentSlices[timeframe_ms];
        
        // Check if we need to start a new slice
        int64_t sliceStart = (snapshot.timestamp_ms / timeframe_ms) * timeframe_ms;
        
        if (currentSlice.startTime_ms == 0 || sliceStart != currentSlice.startTime_ms) {
            // Finalize previous slice if it exists
            if (currentSlice.startTime_ms != 0) {
                finalizeLiquiditySlice(currentSlice);
                m_timeSlices[timeframe_ms].push_back(currentSlice);
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
    
    // Add snapshot data to a time slice
    void addSnapshotToSlice(LiquidityTimeSlice& slice, const OrderBookSnapshot& snapshot) {
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
    
    // Update metrics for a specific price level
    void updatePriceLevelMetrics(LiquidityTimeSlice::PriceLevelMetrics& metrics, 
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
    
    // Handle price levels that disappeared (important for anti-spoofing)
    void updateDisappearingLevels(LiquidityTimeSlice& slice, const OrderBookSnapshot& snapshot) {
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
    
    // Finalize slice calculations when time bucket closes
    void finalizeLiquiditySlice(LiquidityTimeSlice& slice) {
        // Final calculations for all price levels
        for (auto& [price, metrics] : slice.bidMetrics) {
            // Calculate final resting liquidity based on persistence
            if (metrics.persistenceRatio() > 0.8) {  // Present for >80% of interval
                metrics.restingLiquidity = metrics.avgLiquidity;
            } else {
                metrics.restingLiquidity = 0.0;  // Too sporadic, likely spoofing
            }
        }
        
        for (auto& [price, metrics] : slice.askMetrics) {
            if (metrics.persistenceRatio() > 0.8) {
                metrics.restingLiquidity = metrics.avgLiquidity;
            } else {
                metrics.restingLiquidity = 0.0;
            }
        }
    }
    
    // Rebuild entire timeframe from base snapshots (for newly added timeframes)
    void rebuildTimeframe(int64_t timeframe_ms) {
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
    }
    
    // Cleanup old data to prevent memory bloat
    void cleanupOldData() {
        // Keep only recent snapshots (enough to rebuild timeframes)
        size_t maxSnapshots = m_maxHistorySlices * (m_timeframes.back() / m_baseTimeframe_ms);
        while (m_snapshots.size() > maxSnapshots) {
            m_snapshots.pop_front();
        }
        
        // Cleanup old slices
        for (auto& [timeframe, slices] : m_timeSlices) {
            while (slices.size() > m_maxHistorySlices) {
                slices.pop_front();
            }
        }
    }
    
    // Quantize price to grid resolution
    double quantizePrice(double price) const {
        return std::round(price / m_priceResolution) * m_priceResolution;
    }
};

// 4. Integration with existing grid renderer
class BookmapStyleRenderer : public UnifiedGridRenderer {
    Q_OBJECT
    
private:
    std::unique_ptr<LiquidityTimeSeriesEngine> m_liquidityEngine;
    int64_t m_currentTimeframe_ms = 1000;  // Default 1 second
    LiquidityTimeSeriesEngine::LiquidityDisplayMode m_displayMode = 
        LiquidityTimeSeriesEngine::LiquidityDisplayMode::Average;
    
public:
    BookmapStyleRenderer(QQuickItem* parent = nullptr) 
        : UnifiedGridRenderer(parent)
        , m_liquidityEngine(std::make_unique<LiquidityTimeSeriesEngine>()) 
    {
        // Setup 100ms order book polling
        m_orderBookTimer = new QTimer(this);
        connect(m_orderBookTimer, &QTimer::timeout, this, &BookmapStyleRenderer::captureOrderBookSnapshot);
        m_orderBookTimer->start(100);  // 100ms base resolution
    }
    
    Q_INVOKABLE void setTimeframe(int timeframe_ms) {
        m_currentTimeframe_ms = timeframe_ms;
        m_liquidityEngine->addTimeframe(timeframe_ms);  // Ensure timeframe exists
        update();
    }
    
    Q_INVOKABLE void setDisplayMode(int mode) {
        m_displayMode = static_cast<LiquidityTimeSeriesEngine::LiquidityDisplayMode>(mode);
        m_liquidityEngine->setDisplayMode(m_displayMode);
        update();
    }
    
    Q_INVOKABLE QStringList getAvailableTimeframes() const {
        QStringList timeframes;
        for (int64_t tf : m_liquidityEngine->getAvailableTimeframes()) {
            timeframes << QString("%1ms").arg(tf);
        }
        return timeframes;
    }
    
public slots:
    void onOrderBookUpdated(const OrderBook& book) override {
        // Store latest order book for next snapshot
        m_latestOrderBook = book;
    }
    
private slots:
    void captureOrderBookSnapshot() {
        if (!m_latestOrderBook.bids.empty()) {
            m_liquidityEngine->addOrderBookSnapshot(m_latestOrderBook);
            update();  // Trigger re-render with new data
        }
    }
    
protected:
    void updateVisibleCells() override {
        if (!m_timeWindowValid) return;
        
        m_visibleCells.clear();
        
        // Get liquidity slices for current timeframe within viewport
        auto visibleSlices = m_liquidityEngine->getVisibleSlices(
            m_currentTimeframe_ms, m_visibleTimeStart_ms, m_visibleTimeEnd_ms);
        
        for (const auto* slice : visibleSlices) {
            createCellsFromLiquiditySlice(*slice);
        }
    }
    
private:
    QTimer* m_orderBookTimer;
    OrderBook m_latestOrderBook;
    
    void createCellsFromLiquiditySlice(const LiquidityTimeSlice& slice) {
        // Create cells for each price level with liquidity
        for (const auto& [price, metrics] : slice.bidMetrics) {
            double displayValue = slice.getDisplayValue(price, true, m_displayMode);
            if (displayValue > 0.0) {
                createLiquidityCell(slice, price, displayValue, true);
            }
        }
        
        for (const auto& [price, metrics] : slice.askMetrics) {
            double displayValue = slice.getDisplayValue(price, false, m_displayMode);
            if (displayValue > 0.0) {
                createLiquidityCell(slice, price, displayValue, false);
            }
        }
    }
    
    void createLiquidityCell(const LiquidityTimeSlice& slice, double price, 
                           double liquidity, bool isBid) {
        CellInstance cell;
        
        // Calculate screen position for this time/price cell
        QRectF rect;
        rect.setLeft((slice.startTime_ms - m_visibleTimeStart_ms) / 
                    double(m_visibleTimeEnd_ms - m_visibleTimeStart_ms) * width());
        rect.setRight((slice.endTime_ms - m_visibleTimeStart_ms) / 
                     double(m_visibleTimeEnd_ms - m_visibleTimeStart_ms) * width());
        rect.setTop((1.0 - (price + 1.0 - m_minPrice) / (m_maxPrice - m_minPrice)) * height());
        rect.setBottom((1.0 - (price - m_minPrice) / (m_maxPrice - m_minPrice)) * height());
        
        cell.screenRect = rect;
        cell.intensity = liquidity / 1000.0;  // Scale for color calculation
        
        // Color based on bid/ask and intensity
        if (isBid) {
            cell.color = QColor::fromHsv(120, 255, std::min(255, int(cell.intensity * 100 + 100))); // Green scale
        } else {
            cell.color = QColor::fromHsv(0, 255, std::min(255, int(cell.intensity * 100 + 100)));   // Red scale
        }
        
        m_visibleCells.push_back(cell);
    }
};