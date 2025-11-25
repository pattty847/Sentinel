## Proposed Strategy Data Access Architecture

### Current Problems:
- Trade strategies get unused heatmap cell data
- Heatmap strategy gets unused trade data  
- Data bundled unnecessarily in GridSliceBatch
- Future extensibility issues (orderbook, level2 data, etc.)

### Proposed Solution: Data Access Interface

```cpp
// New interface for strategies to access data directly
class IDataAccessor {
public:
    virtual ~IDataAccessor() = default;
    
    // Trade data access
    virtual std::vector<Trade> getRecentTrades(const std::string& symbol, size_t maxCount = 1000) = 0;
    virtual std::vector<Trade> getTradesInTimeRange(const std::string& symbol, int64_t startMs, int64_t endMs) = 0;
    
    // Heatmap/cell data access  
    virtual std::shared_ptr<const std::vector<CellInstance>> getVisibleCells() = 0;
    
    // Future extensibility
    virtual std::shared_ptr<const OrderBook> getOrderBook(const std::string& symbol) = 0;
    virtual std::vector<BookLevel> getOrderBookLevels(const std::string& symbol, int maxLevels = 20) = 0;
    
    // Viewport for coordinate conversion
    virtual Viewport getViewport() const = 0;
};

// Updated strategy interface
class IRenderStrategy {
public:
    virtual ~IRenderStrategy() = default;
    virtual QSGNode* buildNode(IDataAccessor* dataAccess, const std::string& symbol) = 0;
};

// UGR implementation of data accessor
class UGRDataAccessor : public IDataAccessor {
private:
    UnifiedGridRenderer* m_ugr;
    DataCache* m_dataCache;
    
public:
    std::vector<Trade> getRecentTrades(const std::string& symbol, size_t maxCount) override {
        // Can access both UGR's m_recentTrades AND DataCache::recentTrades()
        if (m_dataCache) {
            return m_dataCache->recentTrades(symbol);  // More comprehensive
        }
        // Fallback to UGR's limited cache
        return m_ugr->getRecentTrades();
    }
    
    std::shared_ptr<const std::vector<CellInstance>> getVisibleCells() override {
        return m_ugr->getVisibleCells();
    }
    
    std::shared_ptr<const OrderBook> getOrderBook(const std::string& symbol) override {
        return m_dataCache ? m_dataCache->getOrderBook(symbol) : nullptr;
    }
    
    // etc...
};
```

### Usage in Strategies:

```cpp
// HeatmapStrategy.cpp
QSGNode* HeatmapStrategy::buildNode(IDataAccessor* data, const std::string& symbol) {
    auto cells = data->getVisibleCells();
    if (!cells || cells->empty()) return nullptr;
    
    // Only access heatmap data, ignore trades
    // Build heatmap from cells...
}

// TradeBubbleStrategy.cpp  
QSGNode* TradeBubbleStrategy::buildNode(IDataAccessor* data, const std::string& symbol) {
    auto trades = data->getRecentTrades(symbol, 1000);
    if (trades.empty()) return nullptr;
    
    // If needed, could also access orderbook for context:
    // auto orderbook = data->getOrderBook(symbol);
    
    // Build bubbles from trades...
}
```

### Benefits:

1. **No Data Bundling**: Each strategy gets exactly what it needs
2. **Future Proof**: Easy to add new data types (L2, orderbook, etc.)
3. **Lazy Loading**: Data only fetched when strategy needs it
4. **Multiple Sources**: Strategies can choose between UGR cache vs DataCache
5. **Cross-Strategy Data**: Trade strategy could access orderbook if needed

### Migration Path:

1. Keep current GridSliceBatch temporarily for compatibility
2. Add IDataAccessor interface alongside  
3. Update strategies one-by-one to use new interface
4. Remove GridSliceBatch when all strategies migrated