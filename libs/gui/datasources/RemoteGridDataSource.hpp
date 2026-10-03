#pragma once
#include "IGridDataSource.hpp"
#include "CandleSeriesBuffer.hpp"
#include "CandleBackfillState.hpp"
#include <QTimer>
#include <unordered_set>
#include "../../core/protocol/SentinelStreamClient.hpp"
#include "../config/GuiConfigStore.hpp"

class RemoteGridDataSource : public IGridDataSource {
    Q_OBJECT
    Q_PROPERTY(QObject* candleBuffer READ candleBuffer CONSTANT)
public:
    explicit RemoteGridDataSource(const QString& host, const QString& port,
                                  const QString& caFile = {}, QObject* parent = nullptr);

    void subscribe(const QString& symbol) override;
    void unsubscribe(const QString& symbol) override;
    void requestHeatmapHistory(const QString& symbol,
                               int64_t timeframeMs,
                               int64_t endTimeMs,
                               int count) override;
    void registerRecordingView(const recording::LiveView& view) override;
    void releaseRecordingView(const recording::LiveView& view) override;
    void requestRecordingHeatmapHistory(const protocol::recordingwire::Request& request) override;
    void requestFootprintHistory(const QString& symbol,
                                 int64_t timeframeMs,
                                 int64_t endTimeMs,
                                 int count) override;
    void requestTpoHistory(const QString& symbol,
                           int64_t timeframeMs,
                           int sessionType,
                           int64_t endTimeMs,
                           int count,
                           const QString& requestId) override;
    void cancelTpoHistory(const QString& symbol, const QString& requestId) override;
    void setCandleHistoryViewport(const QString& symbol, int64_t timeframeSec,
                                 qint64 startMs, qint64 endMs) override;
    void sendTradeCommand(const trading::TradeCommand& command) override;
    void sendAlgoCommand(const std::string& algoId, const std::string& action, const std::string& symbol, const trading::AlgoParams& params) override;

    const LiveOrderBook& getDirectLiveOrderBook(const std::string& productId) const override;
    void connectToServer();
    QObject* candleBuffer() const { return m_candleBuffer.get(); }
    SentinelStreamClient* streamClient() { return &m_client; }
    Q_INVOKABLE bool isBookSnapshotStale(const QString& symbol) const;

private slots:
    void onSnapshotReceived(const QString& productId, const std::vector<OrderBookLevel>& bids,
                            const std::vector<OrderBookLevel>& asks, quint64 deliveryGeneration);
    void onL2UpdateReceived(const QString& productId, const std::vector<BookLevelUpdate>& updates,
                            quint64 deliveryGeneration);
    void onHeatmapSliceReceived(const HeatmapSlice& slice);
    void onFootprintSliceReceived(const FootprintSlice& slice);
    void onTpoSliceReceived(const TpoSlice& slice);
    void onVolumeProfileSliceReceived(const VolumeProfileSlice& slice);
    void onHeatmapHistoryReceived(const QString& symbol,
                                  int64_t timeframeMs,
                                  int gridWidth,
                                  int gridHeight,
                                  int64_t requestEndMs,
                                  int64_t oldestAvailableMs,
                                  const QVector<SentinelStreamClient::HeatmapHistoryColumn>& columns);
    void onCandleBarUpdateReceived(const QString& symbol,
                                   int64_t timeframeSec,
                                   int64_t bucketStartMs,
                                   int64_t seq,
                                   const SentinelStreamClient::CandleBar& bar, quint64 deliveryGeneration);
    void onCandleBarClosedReceived(const QString& symbol,
                                   int64_t timeframeSec,
                                   int64_t bucketStartMs,
                                   int64_t seq,
                                   const SentinelStreamClient::CandleBar& bar, quint64 deliveryGeneration);
    void onCandleHistoryReceived(const QString& symbol,
                                 int64_t timeframeSec,
                                 int64_t startTimeSec,
                                 int64_t endTimeSec,
                                 const QVector<SentinelStreamClient::CandleBar>& candles);
    void onServerConfigReceived(const ServerConfig& config);
    void onAlgoOrderEventReceived(const trading::AlgoOrderEvent& event);
    void onPnlSnapshotReceived(const trading::PnlSnapshot& snapshot);

private:
    friend struct CandleDataSourceTest;
    void processBookSnapshotDeadlines(qint64 nowMs);
    void onL2UpdateReceivedAt(const QString& productId, const std::vector<BookLevelUpdate>& updates,
                              quint64 deliveryGeneration, qint64 nowMs);
    void requestNextCandlePage();
    void advanceCandleDeliveryGeneration();
    SentinelStreamClient m_client;
    std::unique_ptr<CandleSeriesBuffer> m_candleBuffer;
    CandleBackfillState m_candleBackfill;
    QTimer m_candleBackfillTimer;
    QTimer m_bookSnapshotTimer;
    struct PendingBookSnapshot {
        qint64 deadlineMs = 0;
        bool retried = false;
        bool stale = false;
        qint64 nextStaleRetryMs = 0;
        qint64 staleRetryBackoffMs = 5000;
    };
    std::unordered_map<std::string, PendingBookSnapshot> m_pendingBookSnapshots;
    std::unordered_set<std::string> m_activeBookSymbols;
    QString m_candleSymbol;
    int64_t m_candleTimeframeSec = 0;
    bool m_candleHistoryReady = false;
    quint64 m_candleDeliveryGeneration = 0;
    // We need to maintain a local LiveOrderBook replica if we want to return refs
    // Or we might change the interface to not return references?
    // IGridDataSource::getDirectLiveOrderBook returns const ref.
    // So RemoteGridDataSource MUST maintain a local replica.

    mutable std::unordered_map<std::string, std::unique_ptr<LiveOrderBook>> m_replicaBooks;
    ServerConfig m_serverConfig;
};
