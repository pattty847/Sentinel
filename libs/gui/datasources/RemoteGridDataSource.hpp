#pragma once
#include "IGridDataSource.hpp"
#include "CandleSeriesBuffer.hpp"
#include "CandleBackfillState.hpp"
#include "flow/FlowChunkClient.hpp"
#include "FlowTimingProbe.hpp"
#include <QElapsedTimer>
#include "../models/MarketHealth.hpp"
#include <QDateTime>
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
    ~RemoteGridDataSource() override;
    void setFlowViewport(const QString& symbol, int64_t timeframeMs, int64_t startMs, int64_t endMs, int64_t resetAnchorMs = -1, bool rows = false) override;
    void requestTradeHistory(const QString& symbol, int64_t startMs, int64_t endMs, quint64 req) override;
    void subscribeFlowLive(const QString& symbol, int64_t timeframeMs) override;
    void unsubscribeFlowLive() override;
    const FlowSeriesBuffer& flowBuffer(bool summary = false) const {
        return summary && !m_flowSummary.symbol().isEmpty() ? m_flowSummary : m_flowBuffer;
    }
    void requestTpoHistory(const QString& symbol,
                           int64_t timeframeMs,
                           int sessionType,
                           int64_t endTimeMs,
                           int count,
                           const QString& requestId) override;
    void cancelTpoHistory(const QString& symbol, const QString& requestId) override;
    void setCandleHistoryViewport(const QString& symbol, int64_t timeframeMs,
                                 qint64 startMs, qint64 endMs) override;
    void sendTradeCommand(const trading::TradeCommand& command) override;
    void sendAlgoCommand(const std::string& algoId, const std::string& action, const std::string& symbol,
                         const trading::AlgoParams& params, const std::string& commandId) override;
    bool isConnectionActive() const override { return m_connectionActive; }

    const LiveOrderBook& getDirectLiveOrderBook(const std::string& productId) const override;
    void connectToServer();
    QObject* candleBuffer() const { return m_candleBuffer.get(); }
    MarketHealth* marketHealth() { return &m_marketHealth; }
    SentinelStreamClient* streamClient() { return &m_client; }
    Q_INVOKABLE bool isBookSnapshotStale(const QString& symbol) const override;

private slots:
    void onSnapshotReceived(const QString& productId, const std::vector<OrderBookLevel>& bids,
                            const std::vector<OrderBookLevel>& asks, double tickSize,
                            quint64 deliveryGeneration, const QString& status, uint64_t bookVersion);
    void onL2UpdateReceived(const QString& productId, const std::vector<BookLevelUpdate>& updates,
                            double tickSize, quint64 deliveryGeneration, uint64_t bookVersion = 0);
    void onTpoSliceReceived(const TpoSlice& slice);
    void onVolumeProfileSliceReceived(const VolumeProfileSlice& slice);
    void onCandleBarUpdateReceived(const QString &symbol, int64_t timeframeMs, int64_t bucketStartMs, int64_t seq,
                                   const SentinelStreamClient::CandleBar &bar, quint64 deliveryGeneration);
    void onCandleBarClosedReceived(const QString &symbol, int64_t timeframeMs, int64_t bucketStartMs, int64_t seq,
                                   const SentinelStreamClient::CandleBar &bar, quint64 deliveryGeneration);
    void onCandleHistoryReceived(const QString &symbol, int64_t timeframeMs, int64_t startTimeMs, int64_t endTimeMs,
                                 const QVector<SentinelStreamClient::CandleBar> &candles);
    void onServerConfigReceived(const ServerConfig& config);
    void onAlgoOrderEventReceived(const trading::AlgoOrderEvent& event);
    void onPnlSnapshotReceived(const trading::PnlSnapshot& snapshot);

private:
    friend struct CandleDataSourceTest;
    friend struct FlowDataSourceTest;
    void initializeFlow();
    void startFlowLive();
    void refreshFlowView();
    void onFlowBarsReceived(SentinelStreamClient::FlowBatchPtr batch);
    FlowSeriesBuffer m_flowBuffer, m_flowSummary;
    FlowTimingProbe m_flowProbe;
    QTimer m_flowTimer, m_flowViewTimer;
    QElapsedTimer m_flowClock;
    QString m_flowSymbol;
    int64_t m_flowTimeframeMs = 0, m_flowFrom = 0, m_flowTo = 0, m_flowAnchor = -1;
    quint64 m_flowSubscription = 0, m_flowGeneration = 0;
    bool m_flowConfigReady = false, m_flowAvailabilityReady = false, m_flowRows = false;
    int m_flowPan = 0, m_flowRetryMs = 100;
    bool m_flowComposing = false, m_flowDirty = false;
    struct FlowWork;
    std::shared_ptr<FlowWork> m_flowWork;
    // Only retirement metadata survives selection changes; live bars are reset.
    std::map<std::pair<QString, int64_t>, std::shared_ptr<FlowWork>> m_flowOwnership;
    std::deque<std::pair<QString, int64_t>> m_flowOwnershipOrder;
    void processBookSnapshotDeadlines(qint64 nowMs);
    void onL2UpdateReceivedAt(const QString& productId, const std::vector<BookLevelUpdate>& updates,
                              double tickSize, quint64 deliveryGeneration, qint64 nowMs,
                              uint64_t bookVersion = 0);
    void requestNextCandlePage();
    CandleBackfillState::Loaded candleLoaded() const;
    qint64 candleNow() const { return QDateTime::currentMSecsSinceEpoch() + m_candleClockOffsetMs; }
    void advanceCandleDeliveryGeneration();
    MarketHealth m_marketHealth;
    SentinelStreamClient m_client;
    // Destroy/join this service before the adapter's stream client can die.
    std::unique_ptr<sentinel::flow::FlowChunkClient> m_flowChunks;
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
    std::unordered_map<std::string, uint64_t> m_bookVersions;
    std::unordered_set<std::string> m_activeBookSymbols;
    QString m_candleSymbol;
    int64_t m_candleTimeframeMs = 0;
    bool m_candleHistoryReady = false;
    bool m_connectionActive = false; // GUI-thread mirror of connected/disconnected signals
    quint64 m_candleDeliveryGeneration = 0;
    qint64 m_candleClockOffsetMs = 0; // tests place the live edge next to their viewport
    // We need to maintain a local LiveOrderBook replica if we want to return refs
    // Or we might change the interface to not return references?
    // IGridDataSource::getDirectLiveOrderBook returns const ref.
    // So RemoteGridDataSource MUST maintain a local replica.

    mutable std::unordered_map<std::string, std::unique_ptr<LiveOrderBook>> m_replicaBooks;
    ServerConfig m_serverConfig;
};
