#pragma once
#include <QObject>
#include <QString>
#include <memory>
#include <vector>
#include <QByteArray>
#include <QVector>
#include "../../core/marketdata/model/TradeData.h"
#include "../../core/protocol/HeatmapSlice.hpp"
#include "../../core/protocol/FootprintSlice.hpp"
#include "../../core/protocol/TpoSlice.hpp"
#include "../../core/trading/TradingTypes.hpp"

// Abstract interface for supplying market data to the grid; supports remote client-server access via WebSocket.
class IGridDataSource : public QObject {
    Q_OBJECT
public:
    struct HeatmapHistoryColumn {
        int64_t bucketStartMs = 0;
        int64_t bucketEndMs = 0;
        double minPrice = 0.0;
        double maxPrice = 0.0;
        double tickSize = 0.0;
        QByteArray intensity;
        QByteArray liquidity;
        double liquidityScale = 1.0;
    };

    explicit IGridDataSource(QObject* parent = nullptr) : QObject(parent) {}
    virtual ~IGridDataSource() = default;

    virtual void subscribe(const QString& symbol) = 0;
    virtual void unsubscribe(const QString& symbol) = 0;
    virtual void requestHeatmapHistory(const QString& symbol,
                                       int64_t timeframeMs,
                                       int64_t endTimeMs,
                                       int count) = 0;
    virtual void requestFootprintHistory(const QString& symbol,
                                         int64_t timeframeMs,
                                         int64_t endTimeMs,
                                         int count) = 0;
    virtual void requestTpoHistory(const QString& symbol,
                                   int64_t timeframeMs,
                                   int64_t endTimeMs,
                                   int count) = 0;
    virtual void requestCandleHistory(const QString& symbol,
                                      int64_t timeframeSec,
                                      int64_t endTimeSec,
                                      int limit) = 0;
    virtual void sendTradeCommand(const trading::TradeCommand& command) = 0;

    // GUI-thread only: returns dense live order book for high-performance rendering/ingestion.
    virtual const LiveOrderBook& getDirectLiveOrderBook(const std::string& productId) const = 0;

signals:
    // Core Signals
    void tradeReceived(const Trade& trade);
    void liveOrderBookUpdated(const QString& productId, const std::vector<BookDelta>& deltas);
    void orderBookUpdated(std::shared_ptr<const OrderBook> book);
    void heatmapSliceReceived(const HeatmapSlice& slice);
    void footprintSliceReceived(const FootprintSlice& slice);
    void tpoSliceReceived(const TpoSlice& slice);
    void heatmapHistoryReceived(const QString& symbol,
                                int64_t timeframeMs,
                                int gridWidth,
                                int gridHeight,
                                const QVector<HeatmapHistoryColumn>& columns);

    void connectionStatusChanged(bool connected);
    void errorOccurred(const QString& error);
    void orderUpdated(const trading::OrderUpdate& update);
    void positionUpdated(const trading::PositionUpdate& update);
};

Q_DECLARE_METATYPE(IGridDataSource::HeatmapHistoryColumn)
Q_DECLARE_METATYPE(QVector<IGridDataSource::HeatmapHistoryColumn>)


Q_DECLARE_METATYPE(trading::OrderUpdate)
Q_DECLARE_METATYPE(trading::PositionUpdate)
