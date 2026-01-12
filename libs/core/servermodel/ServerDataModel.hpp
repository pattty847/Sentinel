#pragma once
#include <unordered_map>
#include <shared_mutex>
#include <string>
#include <memory>
#include <QObject>
#include "SymbolHotData.hpp"
#include "TickBinaryLogger.hpp"
#include "TimeframeAggregator.hpp"
#include "../marketdata/model/TradeData.h"

class ServerDataModel : public QObject {
    Q_OBJECT
public:
    explicit ServerDataModel(QObject* parent = nullptr);
    ~ServerDataModel();

    SymbolHotData& ensureSymbol(const std::string& symbol);
    
    // Snapshot Accessor
    const LiveOrderBook& getLiveOrderBook(const std::string& symbol);
    // History Accessor
    std::vector<OHLCVBar> getHistory(const std::string& symbol, Timeframe tf, size_t limit = 1000) const;

public slots:
    void onTrade(const Trade& trade);
    void onLiveOrderBookUpdated(const QString& productId, const std::vector<BookDelta>& deltas);
    void onLiveOrderBookInitialized(const QString& productId, const std::vector<OrderBookLevel>& bids, const std::vector<OrderBookLevel>& asks);

signals:
    // Rebroadcast signals for streaming clients
    void tradeBroadcast(const Trade& trade);
    void bookUpdateBroadcast(const QString& productId, const std::vector<BookDelta>& deltas);
    
    // Aggregation signals (forwarded from aggregator)
    void barClosed(const QString& symbol, Timeframe tf, const OHLCVBar& bar);
    void barUpdated(const QString& symbol, Timeframe tf, const OHLCVBar& bar);

private:
    std::shared_mutex m_mutex;
    std::unordered_map<std::string, std::unique_ptr<SymbolHotData>> m_symbols;
    std::unique_ptr<TickBinaryLogger> m_logger;
    std::unique_ptr<TimeframeAggregator> m_aggregator;
};
