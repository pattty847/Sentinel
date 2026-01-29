#pragma once
#include <unordered_map>
#include <vector>
#include <shared_mutex>
#include <string>
#include <memory>
#include <QObject>
#include <QByteArray>
#include "SymbolHotData.hpp"
#include "HeatmapTwapStreamer.hpp"
#include "TickBinaryLogger.hpp"
#include "TimeframeAggregator.hpp"
#include "../marketdata/model/TradeData.h"

class ServerDataModel : public QObject {
    Q_OBJECT
public:
    explicit ServerDataModel(QObject* parent = nullptr);
    ~ServerDataModel();

    SymbolHotData& ensureSymbol(const std::string& symbol);
    std::vector<std::string> getSymbolsSnapshot() const;
    
    // Snapshot Accessor
    const LiveOrderBook& getLiveOrderBook(const std::string& symbol);
    // History Accessor
    std::vector<OHLCVBar> getHistory(const std::string& symbol, Timeframe tf, size_t limit = 1000) const;
    bool getHeatmapHistory(const std::string& symbol,
                           int64_t timeframeMs,
                           int64_t endTimeMs,
                           int count,
                           int& outGridWidth,
                           int& outGridHeight,
                           std::vector<HeatmapTwapStreamer::HistoryColumn>& out) const;

public slots:
    void onTrade(const Trade& trade);
    void onLiveOrderBookUpdated(const QString& productId, const std::vector<BookDelta>& deltas);
    void onLiveOrderBookLevelUpdates(const QString& productId,
                                     const std::vector<BookLevelUpdate>& updates,
                                     qint64 exchangeMs);
    void onLiveOrderBookInitialized(const QString& productId, const std::vector<OrderBookLevel>& bids, const std::vector<OrderBookLevel>& asks);

signals:
    // Rebroadcast signals for streaming clients
    void tradeBroadcast(const Trade& trade);
    void bookUpdateBroadcast(const QString& productId, const std::vector<BookDelta>& deltas);
    
    // Aggregation signals (forwarded from aggregator)
    void barClosed(const QString& symbol, Timeframe tf, const OHLCVBar& bar);
    void barUpdated(const QString& symbol, Timeframe tf, const OHLCVBar& bar);

    void heatmapSliceReady(const QString& symbol,
                           int64_t bucketStartMs,
                           int64_t bucketEndMs,
                           int64_t timeframeMs,
                           int gridWidth,
                           int gridHeight,
                           double minPrice,
                           double maxPrice,
                           double tickSize,
                           double midPrice,
                           double lastTrade,
                           const QByteArray& column,
                           const QByteArray& liquidityColumn,
                           double liquidityScale,
                           bool reset);

private:
    mutable std::shared_mutex m_mutex;
    std::unordered_map<std::string, std::unique_ptr<SymbolHotData>> m_symbols;
    std::unique_ptr<TickBinaryLogger> m_logger;
    std::unique_ptr<TimeframeAggregator> m_aggregator;
    std::unique_ptr<HeatmapTwapStreamer> m_heatmapStreamer;
};
