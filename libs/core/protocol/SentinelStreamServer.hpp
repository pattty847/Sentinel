#pragma once
#include <QObject>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/thread_pool.hpp>
#include <memory>
#include <unordered_set>
#include <mutex>
#include <thread>
#include <functional>
#include <utility>
#include <string>
#include <vector>
#include <atomic>
#include <cstddef>
#include <condition_variable>
#include <unordered_map>
#include "../servermodel/ServerDataModel.hpp"
#include "../servermodel/ChunkService.hpp"
#include "../config/ConfigTypes.hpp"
#include "../trading/TradingTypes.hpp"
#include "../trading/LiveTradingSession.hpp"

class Authenticator;
class CoinbaseRestClient;
class Session;

namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

class SentinelStreamServer : public QObject {
    Q_OBJECT
public:
    explicit SentinelStreamServer(ServerDataModel& model,
                                  Authenticator& auth,
                                  const ServerConfig& config,
                                  int port,
                                  QObject* parent = nullptr);
    ~SentinelStreamServer();

    void start();
    void stop();

signals:
    void clientSubscribed(const QString& symbol);
    void clientUnsubscribed(const QString& symbol);
    void orderUpdateBroadcast(const trading::OrderUpdate& update);
    void positionUpdateBroadcast(const trading::PositionUpdate& update);
    void riskOrderUpdateBroadcast(const trading::RiskOrderUpdate& update);
    void algoOrderEventBroadcast(const trading::AlgoOrderEvent& event);
    void pnlSnapshotBroadcast(const trading::PnlSnapshot& snapshot);

public:
    enum class FeedAdmission { Accepted, CapacityExceeded, InvalidProduct, UpstreamUnavailable };
    void setFeedAdmissionHandler(std::function<FeedAdmission(const std::string&)> handler) {
        m_feedAdmissionHandler = std::move(handler);
    }
    FeedAdmission notifyClientSubscribed(const std::string& symbol);
    void registerMetrics(sentinel::metrics::MetricsRegistry& registry);
    void notifyClientUnsubscribed(const std::string& symbol);
    // Serialize a queued upstream release with admission on the server thread.
    bool releaseIfNoSubscribers(const std::string& symbol, const std::function<void()>& release);
    CoinbaseRestClient& restClient();
    void requestBookProductMetadata(const QString& symbol, uint64_t lifetime);
    const ServerConfig& serverConfig() const { return m_serverConfig; }
    void processTradeCommand(const trading::TradeCommand& command);
    void broadcastOrderUpdate(const trading::OrderUpdate& update);
    void broadcastPositionUpdate(const trading::PositionUpdate& update);
    void broadcastRiskOrderUpdate(const trading::RiskOrderUpdate& update);
    void broadcastAlgoOrderEvent(const trading::AlgoOrderEvent& event);
    void broadcastPnlSnapshot(const trading::PnlSnapshot& snapshot);
    void broadcastCoinbaseLatency(int milliseconds);
    bool startAlgo(const std::string& algoId, const std::string& symbol, const trading::AlgoParams& params);
    void stopAlgo(const std::string& algoId);
    trading::LiveTradingSession& tradingSession() { return *m_tradingSession; }
    trading::LiveTradingSession* tradingSessionPtr() const { return m_tradingSession.get(); }
    uint64_t registerLatencySender(std::function<void(int)> sendFn);
    void unregisterLatencySender(uint64_t id);
    // Open client sessions; takes the sessions mutex briefly (metrics scrape).
    size_t sessionCount();

private:
    friend class Session;
    friend struct RecordingServerStopTest;
    friend struct HeatmapChunkWireTest;
    friend struct ServerFeedAdmissionTest;

    void doAccept();
    void registerSession(const std::shared_ptr<Session>& session);
    void unregisterSession(const Session* session);
    bool submitHistoryTask(std::function<void()> task);
    std::string buildHeatmapHistoryChunk(const std::string& symbol,
                                         int64_t timeframeMs,
                                         int64_t endTimeMs,
                                         int64_t startTimeMs,
                                         int count) const;

    ServerDataModel& m_model;
    // Heatmap chunk serving; null when recording is unavailable. Shared with
    // history-worker jobs, which the server joins in stop().
    std::shared_ptr<recording::ChunkService> m_chunks;
    std::unique_ptr<CoinbaseRestClient> m_restClient;
    ServerConfig m_serverConfig;
    int m_port;

    net::io_context m_ioc;
    ssl::context m_sslCtx{ssl::context::tlsv13_server};
    std::unique_ptr<tcp::acceptor> m_acceptor;
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::unique_ptr<trading::LiveTradingSession> m_tradingSession;

    static constexpr size_t kHistoryWorkerCount = 2;
    static constexpr size_t kMaxPendingHistoryTasks = 8;
    std::mutex m_historyWorkersMutex;
    std::unique_ptr<net::thread_pool> m_historyWorkers;
    std::atomic_size_t m_pendingHistoryTasks{0};

    std::mutex m_sessionsMutex;
    std::condition_variable m_sessionsDrained;
    std::unordered_set<std::shared_ptr<Session>> m_sessions;

    std::mutex m_symbolSubscriptionsMutex;
    std::unordered_map<std::string, size_t> m_symbolSubscriptions;
    std::function<FeedAdmission(const std::string&)> m_feedAdmissionHandler;
    // Refusal diagnostics retain at most eight recent products (LRU, resets on eviction).
    std::vector<std::pair<std::string, uint64_t>> m_refusals;
    void recordRefusalLocked(const std::string& symbol, const char* code);

    std::mutex m_latencySendersMutex;
    std::vector<std::pair<uint64_t, std::function<void(int)>>> m_latencySenders;
    std::atomic<uint64_t> m_nextLatencySenderId{0};

    std::mutex m_tradingBroadcastMutex;
    std::vector<std::pair<uint64_t, std::function<void(const std::string&)>>> m_tradingBroadcasters;
    std::atomic<uint64_t> m_nextBroadcasterId{0};
public:
    uint64_t registerTradingBroadcaster(std::function<void(const std::string&)> fn);
    void unregisterTradingBroadcaster(uint64_t id);
private:
};
