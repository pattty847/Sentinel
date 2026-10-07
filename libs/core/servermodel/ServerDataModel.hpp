#pragma once
#include "RecordingLive.hpp"
#include <map>
#include <set>
#include <unordered_map>
#include <vector>
#include <deque>
#include <shared_mutex>
#include <mutex>
#include <string>
#include <memory>
#include <atomic>
#include <filesystem>
#include <optional>
#include <nlohmann/json.hpp>
#include <QObject>
#include <QByteArray>
#include <QTimer>
#include "SymbolHotData.hpp"
#include "HeatmapTwapStreamer.hpp"
#include "IHeatmapDataSource.hpp"
#include "TickBinaryLogger.hpp"
#include "TimeframeAggregator.hpp"
#include "BookRecorder.hpp"
#include "RecorderStallMonitor.hpp"
#include "../marketdata/model/TradeData.h"
#include "../protocol/HeatmapSlice.hpp"
#include "../config/ConfigTypes.hpp"

namespace sentinel::metrics { class MetricsRegistry; }

class ServerDataModel : public QObject, public IHeatmapDataSource {
    Q_OBJECT
    friend struct TradeOverlayModelTest;
    friend struct ServerFeedAdmissionTest;
    friend struct ShadowServerTestAccess;
public:
    struct FootprintTradeSample {
        int64_t timestampMs = 0;
        double price = 0.0;
        double size = 0.0;
        AggressorSide side = AggressorSide::Unknown;
    };

    explicit ServerDataModel(const ServerConfig& config, QObject* parent = nullptr);
    ~ServerDataModel();

    SymbolHotData& ensureSymbol(const std::string& symbol) override;
    std::vector<std::string> getSymbolsSnapshot() const override;
    
    // Snapshot Accessor
    const LiveOrderBook& getLiveOrderBook(const std::string& symbol);
    // History Accessor
    std::vector<OHLCVBar> getHistory(const std::string& symbol, int64_t timeframeMs, size_t limit = 1000) const;
    bool getHeatmapHistory(const std::string& symbol,
                           int64_t timeframeMs,
                           int64_t endTimeMs,
                           int count,
                           int& outGridWidth,
                           int& outGridHeight,
                           std::vector<HeatmapTwapStreamer::HistoryColumn>& out,
                           int64_t startTimeMs = 0) const;

    // Phase 4: lowest persisted bucketStartMs for (symbol, tf), or 0 if no
    // persistence / no records. Surfaced to clients as oldest_available_ms.
    int64_t oldestHeatmapPersistedMs(const std::string& symbol,
                                     int64_t timeframeMs) const;
    bool collectOverlayTrades(const std::string& symbol, int64_t startMs, int64_t endMs,
                              size_t limit, std::vector<FootprintTradeSample>& out,
                              int64_t* retainedFromMs = nullptr) const;
    bool collectFootprintTrades(const std::string& symbol,
                                int64_t startTimeMs,
                                int64_t endTimeMs,
                                std::vector<FootprintTradeSample>& out) const;
    int64_t exchangeNowMs() const override;
    recording::LiveService* recordingLive() const { return m_recordingLive.get(); }
    // Primary recorder started, or (recording.source: roller) the roller attached.
    bool recordingAvailable() const { return m_recorder != nullptr || m_rollerAttached.load(); }
    const std::optional<std::filesystem::path>& recordingDir() const { return m_recordingDir; }
    // Per-level recorder cutoffs; zero when the series is not recorded in this process.
    recording::BookRecorder::Watermarks recordingWatermarks(const std::string& symbol,
                                                            const std::string& layer) const {
        if (m_recorder) return m_recorder->watermarks(symbol, layer);
        if (m_rollerAttached.load()) return m_rollerWatermarks(symbol, layer);
        return {};
    }
    // recording.source: roller. True when the served root and LiveService are
    // ready for the roller (recording.enabled, roller_shadow.enabled, mounted).
    bool servesRoller() const { return m_servesRoller; }
    // Hand-off for the roller workers: publish into this model's LiveService.
    std::function<void(recording::RecordPtr)> rollerPublisher();
    // Roller workers: withdraw a product's provisional live minutes.
    std::function<void(const std::string&)> rollerRetract();
    std::function<bool(const std::string&, const std::string&, recording::RecordPtr)> rollerEnsureFinal();
    // Main thread, once, before the stream server starts: the roller's
    // thread-safe watermark and running queries.
    using RollerWatermarks = std::function<recording::BookRecorder::Watermarks(const std::string&, const std::string&)>;
    void attachRoller(RollerWatermarks watermarks, std::function<bool(const std::string&)> running);

    // Recorder and upstream-connection series for GET /metrics. Call once on the
    // main thread; the samplers read main-thread state, so render on that thread.
    void registerMetrics(sentinel::metrics::MetricsRegistry& registry);
    void acquireGuiFeed(const std::string& symbol);
    void releaseGuiFeed(const std::string& symbol, int64_t releaseLocalMs = 0);
    void onProductMetadata(const std::string& symbol, uint64_t lifetime,
                           const nlohmann::json& metadata, const std::string& error);
    // recording.live_feed: journal (main thread): the journal header's product
    // JSON for a live feed (no REST), and the roller's re-seed request hook.
    bool journalFeed() const { return m_journalFeed; }
    void onFeedMetadata(const std::string& symbol, const nlohmann::json& metadata);
    // Candle closing follows the journal feed (held while it is not live).
    void onFeedLive(const std::string& symbol, bool live);
    void setReseedHandler(std::function<void(const std::string&)> handler);

public slots:
    void onTrade(const Trade& trade);
    void onLiveOrderBookLevelUpdates(const QString& productId,
                                     const std::vector<BookLevelUpdate>& updates,
                                     qint64 exchangeMs);
    void onLiveOrderBookInitialized(const QString& productId, const std::vector<OrderBookLevel>& bids, const std::vector<OrderBookLevel>& asks, qint64 envelopeMs = 0);
    // Empty productId = every symbol. The book stays invalid until its next snapshot.
    void onLiveOrderBookInvalidated(const QString& productId, const QString& reason);
    // One product's market-data connection up/down. Gates that symbol's recorder
    // stall warning; recorder alerts select only pinned (default) symbols.
    void onMarketDataConnectionChanged(const std::string& symbol, bool connected);

signals:
    // Rebroadcast signals for streaming clients
    void tradeBroadcast(const Trade& trade);
    void bookUpdateBroadcast(const QString& productId, const std::vector<BookDelta>& deltas,
                             double minPrice, double tickSize, uint64_t bookVersion);
    void bookSnapshotBroadcast(const QString& productId, const std::vector<OrderBookLevel>& bids,
                               const std::vector<OrderBookLevel>& asks, double tickSize,
                               const QString& status, uint64_t bookVersion);
    void productMetadataRequested(const QString& productId, uint64_t lifetime);
    
    // Aggregation signals (forwarded from aggregator)
    void barClosed(const QString& symbol, int64_t timeframeMs, const OHLCVBar& bar);
    void barUpdated(const QString& symbol, int64_t timeframeMs, const OHLCVBar& bar);

    void heatmapSliceReady(const HeatmapSlice& slice);

    // The recorder lost a symbol's book on its own; only a fresh upstream snapshot
    // resumes it. Main thread, at most once per symbol per 30 s (recorder-limited).
    void recordingResnapshotRequested(const QString& symbol, const QString& reason);

private:
    void updateExchangeOffsetMs(int64_t exchangeMs);

    // Slots are invoked on the main thread; subsystems assume single-threaded access.
    mutable std::shared_mutex m_mutex;
    ServerConfig m_serverConfig;
    std::unordered_map<std::string, std::unique_ptr<SymbolHotData>> m_symbols;
    std::unique_ptr<TickBinaryLogger> m_logger;
    std::unique_ptr<TimeframeAggregator> m_aggregator;
    std::unique_ptr<HeatmapTwapStreamer> m_heatmapStreamer;
    std::atomic<int64_t> m_exchangeOffsetMs{0};
    // Metrics mirrors. Written on the main thread, except live publish drops
    // (recorder worker); declared before m_recorder so they outlive its worker.
    std::atomic<uint64_t> m_livePublishDrops{0};
    struct FeedState {
        bool pinned = false, connected = false;
        uint64_t ups = 0, downs = 0, lifetime = 0;
        bool metadataPending = false;
        nlohmann::json metadata;
        int64_t nextMetadataAttemptMs = 0;
    };
    void requestProductMetadataIfNeeded(const std::string& symbol, FeedState& feed);
    void publishAggregatedBook(const QString& productId, SymbolHotData& data,
                               const FeedState& feed, qint64 envelopeMs);
    std::map<std::string, FeedState> m_feeds; // main thread, pinned + active GUI feeds only
    uint64_t m_nextFeedLifetime = 0;
    QTimer m_metadataTimer;
    mutable std::mutex m_footprintTradeMutex;
    std::unordered_map<std::string, std::deque<FootprintTradeSample>> m_recentFootprintTrades;
    int64_t m_footprintTradeRetentionMs = 300'000;
    QTimer m_candleTimer;
    // Recording v2 (null when recording.enabled is false or no usable dir).
    std::shared_ptr<recording::LiveService> m_recordingLive;
    std::unique_ptr<recording::BookRecorder> m_recorder;
    std::optional<std::filesystem::path> m_recordingDir;
    QTimer m_recorderTimer;
    int m_recorderTicks = 0;
    // Main thread: every pinned symbol x recorded layer must keep committing columns.
    std::optional<recording::RecorderStallMonitor> m_stallMonitor;
    std::vector<recording::RecorderStallMonitor::Series> m_stallSeries;
    bool m_servesRoller = false;
    std::atomic<bool> m_rollerAttached{false};
    RollerWatermarks m_rollerWatermarks;
    std::function<bool(const std::string&)> m_rollerRunning;
    const bool m_journalFeed;
    std::function<void(const std::string&)> m_reseed;
    std::map<std::string, int64_t> m_nextReseedMs;
    // Throttled requests are coalesced, never dropped: delivered at the deadline.
    std::set<std::string> m_pendingReseeds;
    QTimer m_reseedTimer;
    void deliverReseeds();
    void requestReseed(const std::string& symbol, const char* why);
    void startRecorder();
    void startRollerServing();
    void checkRecorderProgress(int64_t nowMs);
};
