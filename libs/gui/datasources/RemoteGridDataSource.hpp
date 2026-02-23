#pragma once
#include "IGridDataSource.hpp"
#include "CandleSeriesBuffer.hpp"
#include "../../core/protocol/SentinelStreamClient.hpp"
#include "../config/GuiConfigStore.hpp"

class RemoteGridDataSource : public IGridDataSource {
    Q_OBJECT
    Q_PROPERTY(QObject* candleBuffer READ candleBuffer CONSTANT)
public:
    explicit RemoteGridDataSource(const QString& host, const QString& port, QObject* parent = nullptr);

    void subscribe(const QString& symbol) override;
    void unsubscribe(const QString& symbol) override;
    void requestHeatmapHistory(const QString& symbol,
                               int64_t timeframeMs,
                               int64_t endTimeMs,
                               int count) override;
    void requestFootprintHistory(const QString& symbol,
                                 int64_t timeframeMs,
                                 int64_t endTimeMs,
                                 int count) override;
    void requestTpoHistory(const QString& symbol,
                           int64_t timeframeMs,
                           int64_t endTimeMs,
                           int count) override;
    void requestCandleHistory(const QString& symbol,
                              int64_t timeframeSec,
                              int64_t endTimeSec,
                              int limit) override;
    
    const LiveOrderBook& getDirectLiveOrderBook(const std::string& productId) const override;
    void connectToServer();
    QObject* candleBuffer() const { return m_candleBuffer.get(); }
    SentinelStreamClient* streamClient() { return &m_client; }

private slots:
    void onSnapshotReceived(const QString& productId, const std::vector<OrderBookLevel>& bids, const std::vector<OrderBookLevel>& asks);
    void onL2UpdateReceived(const QString& productId, const std::vector<BookLevelUpdate>& updates);
    void onHeatmapSliceReceived(const HeatmapSlice& slice);
    void onFootprintSliceReceived(const FootprintSlice& slice);
    void onTpoSliceReceived(const TpoSlice& slice);
    void onHeatmapHistoryReceived(const QString& symbol,
                                  int64_t timeframeMs,
                                  int gridWidth,
                                  int gridHeight,
                                  const QVector<SentinelStreamClient::HeatmapHistoryColumn>& columns);
    void onCandleBarUpdateReceived(const QString& symbol,
                                   int64_t timeframeSec,
                                   int64_t bucketStartMs,
                                   int64_t seq,
                                   const SentinelStreamClient::CandleBar& bar);
    void onCandleBarClosedReceived(const QString& symbol,
                                   int64_t timeframeSec,
                                   int64_t bucketStartMs,
                                   int64_t seq,
                                   const SentinelStreamClient::CandleBar& bar);
    void onCandleHistoryReceived(const QString& symbol,
                                 int64_t timeframeSec,
                                 int64_t startTimeSec,
                                 int64_t endTimeSec,
                                 const QVector<SentinelStreamClient::CandleBar>& candles);
    void onServerConfigReceived(const ServerConfig& config);

private:
    SentinelStreamClient m_client;
    std::unique_ptr<CandleSeriesBuffer> m_candleBuffer;
    // We need to maintain a local LiveOrderBook replica if we want to return refs
    // Or we might change the interface to not return references? 
    // IGridDataSource::getDirectLiveOrderBook returns const ref.
    // So RemoteGridDataSource MUST maintain a local replica.
    
    mutable std::unordered_map<std::string, std::unique_ptr<LiveOrderBook>> m_replicaBooks;
    ServerConfig m_serverConfig;
};
