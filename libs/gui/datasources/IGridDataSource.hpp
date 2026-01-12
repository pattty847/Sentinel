#pragma once
#include <QObject>
#include <QString>
#include <memory>
#include <vector>
#include "../../core/marketdata/model/TradeData.h"

/**
 * @brief Abstract interface for supplying market data to the grid.
 * 
 * Supports switching between:
 * - LocalGridDataSource: Direct access to DataCache (Monolith)
 * - RemoteGridDataSource: Network access via WebSocket (Client/Server)
 */
class IGridDataSource : public QObject {
    Q_OBJECT
public:
    explicit IGridDataSource(QObject* parent = nullptr) : QObject(parent) {}
    virtual ~IGridDataSource() = default;

    // Subscription
    virtual void subscribe(const QString& symbol) = 0;
    virtual void unsubscribe(const QString& symbol) = 0;

    // Direct Access (Thread-safe)
    // Returns the dense live order book for high-performance rendering/ingestion.
    virtual const LiveOrderBook& getDirectLiveOrderBook(const std::string& productId) const = 0;

    // Returns recent trades for the symbol
    virtual std::vector<Trade> getRecentTrades(const std::string& productId) const = 0;

signals:
    // Core Signals
    void tradeReceived(const Trade& trade);
    void liveOrderBookUpdated(const QString& productId, const std::vector<BookDelta>& deltas);
    void orderBookUpdated(std::shared_ptr<const OrderBook> book);
    
    // Status Signals
    void connectionStatusChanged(bool connected);
    void errorOccurred(const QString& error);
};

