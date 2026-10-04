#pragma once
#include <QObject>
#include <QString>
#include <memory>
#include <optional>
#include <vector>
#include <QByteArray>
#include <QVector>
#include "../../core/marketdata/model/TradeData.h"
#include "../../core/protocol/HeatmapSlice.hpp"
#include "../../core/protocol/SentinelStreamClient.hpp"
#include "../../core/protocol/FootprintSlice.hpp"
#include "../../core/protocol/TpoSlice.hpp"
#include "../../core/protocol/VolumeProfileSlice.hpp"
#include "../../core/trading/TradingTypes.hpp"
#include "../../core/trading/AlgoEngine.hpp"

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

    explicit IGridDataSource(QObject* parent = nullptr) : QObject(parent) {
        connect(this, &IGridDataSource::connectionStatusChanged, this,
                [this](bool connected) { m_connectionState = connected; });
    }
    // GUI-thread read-only state for late-created / symbol-switching consumers.
    std::optional<bool> connectionState() const { return m_connectionState; }
    virtual bool isBookSnapshotStale(const QString&) const { return false; }
    virtual ~IGridDataSource() = default;

    virtual void subscribe(const QString& symbol) = 0;
    virtual void unsubscribe(const QString& symbol) = 0;
    virtual void requestHeatmapHistory(const QString& symbol,
                                       int64_t timeframeMs,
                                       int64_t endTimeMs,
                                       int count) = 0;
    virtual void registerRecordingView(const recording::LiveView& view) = 0;
    virtual void releaseRecordingView(const recording::LiveView& view) { Q_UNUSED(view); }
    virtual void requestRecordingHeatmapHistory(const protocol::recordingwire::Request& request) = 0;
    virtual void requestFootprintHistory(const QString& symbol,
                                         int64_t timeframeMs,
                                         int64_t endTimeMs,
                                         int count) = 0;
    virtual void requestTpoHistory(const QString& symbol,
                                   int64_t timeframeMs,
                                   int sessionType,
                                   int64_t endTimeMs,
                                   int count,
                                   const QString& requestId) = 0;
    virtual void cancelTpoHistory(const QString& symbol, const QString& requestId) = 0;
    virtual void setCandleHistoryViewport(const QString& symbol, int64_t timeframeSec,
                                         qint64 startMs, qint64 endMs) = 0;
    virtual void sendTradeCommand(const trading::TradeCommand& command) = 0;
    virtual void sendAlgoCommand(const std::string& algoId, const std::string& action, const std::string& symbol, const trading::AlgoParams& params) = 0;
    // GUI-thread snapshot for widgets attached after the last connection signal.
    virtual bool isConnectionActive() const { return false; }

    // GUI-thread only: returns dense live order book for high-performance rendering/ingestion.
    virtual const LiveOrderBook& getDirectLiveOrderBook(const std::string& productId) const = 0;

private:
    std::optional<bool> m_connectionState;

signals:
    // Core Signals
    void tradeReceived(const Trade& trade);
    void liveOrderBookUpdated(const QString& productId, const std::vector<BookDelta>& deltas);
    void orderBookUpdated(std::shared_ptr<const OrderBook> book);
    void heatmapSliceReceived(const HeatmapSlice& slice);
    void footprintSliceReceived(const FootprintSlice& slice);
    void tpoSliceReceived(const TpoSlice& slice);
    void tpoHistoryChunkReceived(const QString& symbol, const QString& requestId, qint64 timeframeMs,
                                 int sessionType, qint64 lastEndMs, int columns);
    void tpoHistoryFailed(const QString& symbol, const QString& requestId, const QString& message);
    void volumeProfileSliceReceived(const VolumeProfileSlice& slice);
    void heatmapHistoryReceived(const QString& symbol,
                                int64_t timeframeMs,
                                int gridWidth,
                                int gridHeight,
                                int64_t requestEndMs,
                                int64_t oldestAvailableMs,
                                const QVector<HeatmapHistoryColumn>& columns);
    void recordingViewError(const QString& symbol, uint64_t generation, const QString& code,
                            const QString& message, int retryMs);
    void recordingHeatmapLiveReceived(const SentinelStreamClient::RecordingHistoryPage& page);
    void recordingHeatmapHistoryReceived(const SentinelStreamClient::RecordingHistoryPage& page);
    void recordingHeatmapHistoryError(const QString& symbol, const QString& requestId,
                                      uint64_t bandGeneration, const QString& message);
    
    void connectionStatusChanged(bool connected);
    void errorOccurred(const QString& error);
    void bookSnapshotStaleChanged(const QString& symbol, bool stale);
    void subscriptionRefused(const QString& symbol, int maxConnections, const QString& message);
    void subscriptionAcknowledged(const QString& symbol);
    void orderUpdated(const trading::OrderUpdate& update);
    void positionUpdated(const trading::PositionUpdate& update);
    void riskOrderUpdated(const trading::RiskOrderUpdate& update);
    void algoOrderEventReceived(const trading::AlgoOrderEvent& event);
    void pnlSnapshotReceived(const trading::PnlSnapshot& snapshot);
};

Q_DECLARE_METATYPE(IGridDataSource::HeatmapHistoryColumn)
Q_DECLARE_METATYPE(QVector<IGridDataSource::HeatmapHistoryColumn>)
Q_DECLARE_METATYPE(trading::OrderUpdate)
Q_DECLARE_METATYPE(trading::PositionUpdate)
Q_DECLARE_METATYPE(trading::RiskOrderUpdate)
Q_DECLARE_METATYPE(trading::AlgoOrderEvent)
Q_DECLARE_METATYPE(trading::PnlSnapshot)
