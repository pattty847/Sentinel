#pragma once
#include <QObject>
#include <QReadWriteLock>
#include <QTimer>
#include <vector>
#include "UnifiedGridRenderer.h"
#include "CoordinateSystem.h"
#include "tradedata.h"

/**
 * 🎯 GRID INTEGRATION ADAPTER - Phase 2 Migration
 * 
 * Primary data pipeline hub that replaces GPUDataAdapter.
 * Receives data from StreamController and distributes to both:
 * - New UnifiedGridRenderer system 
 * - Legacy components (during migration only)
 * 
 * Features:
 * - Thread-safe data handling with QReadWriteLock
 * - Configurable throttling (16ms batching timer)
 * - Buffer management with automatic cleanup
 * - A/B testing support for grid vs legacy systems
 */
class GridIntegrationAdapter : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool gridModeEnabled READ gridModeEnabled WRITE setGridModeEnabled NOTIFY gridModeChanged)
    Q_PROPERTY(UnifiedGridRenderer* gridRenderer READ gridRenderer WRITE setGridRenderer)

public:
    explicit GridIntegrationAdapter(QObject* parent = nullptr);
    
    bool gridModeEnabled() const { return m_gridModeEnabled; }
    Q_INVOKABLE void setGridModeEnabled(bool enabled);
    
    UnifiedGridRenderer* gridRenderer() const { return m_gridRenderer; }
    void setGridRenderer(UnifiedGridRenderer* renderer);

public slots:
    // Primary data ingestion from StreamController
    void onTradeReceived(const Trade& trade);
    void onOrderBookUpdated(const OrderBook& orderBook);
    
    // Buffer management
    void setMaxHistorySlices(int slices) { m_maxHistorySlices = slices; }
    void trimOldData();

signals:
    void gridModeChanged(bool enabled);
    void bufferOverflow(int currentSize, int maxSize);
    
    // 🎯 PHASE 5: Legacy signals removed - pure grid-only mode

private slots:
    void processDataBuffer();

private:
    void processTradeForGrid(const Trade& trade);
    void processOrderBookForGrid(const OrderBook& orderBook);
    
    bool m_gridModeEnabled = false;
    UnifiedGridRenderer* m_gridRenderer = nullptr;
    
    // Thread safety
    mutable QReadWriteLock m_dataLock;
    
    // Buffer management  
    int m_maxHistorySlices = 1000;
    QTimer* m_bufferCleanupTimer;
    QTimer* m_processingTimer;  // 16ms batching like old GPUDataAdapter
    
    // Data buffers
    std::vector<Trade> m_tradeBuffer;
    std::vector<OrderBook> m_orderBookBuffer;
};