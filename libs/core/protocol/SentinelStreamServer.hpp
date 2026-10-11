#pragma once
#include <QObject>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
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
#include <chrono>
#include <cstdint>
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

    // Validate before constructing the application (and starting recording/feeds).
    // Throws for an unparseable address or an address not assigned locally.
    static net::ip::address validateBindAddress(const ServerConfig& config);
    // Invalid addresses throw; other listener failures return false so recording continues.
    bool start();
    void stop();

signals:
    void orderUpdateBroadcast(const trading::OrderUpdate& update);
    void positionUpdateBroadcast(const trading::PositionUpdate& update);
    void riskOrderUpdateBroadcast(const trading::RiskOrderUpdate& update);
    void algoOrderEventBroadcast(const trading::AlgoOrderEvent& event);
    void pnlSnapshotBroadcast(const trading::PnlSnapshot& snapshot);

public:
    enum class FeedAdmission { Accepted, InvalidProduct };
    void setFeedAdmissionHandler(std::function<FeedAdmission(const std::string&)> handler) {
        m_feedAdmissionHandler = std::move(handler);
    }
    FeedAdmission notifyClientSubscribed(const std::string& symbol);
    CoinbaseRestClient& restClient();
    const ServerConfig& serverConfig() const { return m_serverConfig; }
    void processTradeCommand(const trading::TradeCommand& command);
    void broadcastOrderUpdate(const trading::OrderUpdate& update);
    void broadcastPositionUpdate(const trading::PositionUpdate& update);
    void broadcastRiskOrderUpdate(const trading::RiskOrderUpdate& update);
    void broadcastAlgoOrderEvent(const trading::AlgoOrderEvent& event);
    void broadcastPnlSnapshot(const trading::PnlSnapshot& snapshot);
    bool startAlgo(const std::string& algoId, const std::string& symbol, const trading::AlgoParams& params);
    void stopAlgo(const std::string& algoId);
    trading::LiveTradingSession& tradingSession() { return *m_tradingSession; }
    trading::LiveTradingSession* tradingSessionPtr() const { return m_tradingSession.get(); }
    // Open client sessions; takes the sessions mutex briefly (metrics scrape).
    size_t sessionCount();

private:
    friend class Session;
    friend struct RecordingServerStopTest;
    friend struct HeatmapChunkWireTest;
    friend struct ServerFeedAdmissionTest;

    // Accept chain of one start(); `generation` fences completions queued by an earlier one.
    void doAccept(uint64_t generation);
    void retryAcceptAfterError(uint64_t generation, const boost::system::error_code& ec);
    void registerSession(const std::shared_ptr<Session>& session);
    // Registers an accepted session only while this start() is current and running.
    bool admitSession(const std::shared_ptr<Session>& session, uint64_t generation);
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
    // Bumped by every start(). stop() leaves its acceptor close, the aborted accept and
    // the aborted retry wait queued in m_ioc when the I/O thread has not run them yet;
    // restart() runs them later, so each one returns unless its generation is current.
    std::atomic<uint64_t> m_acceptGeneration{0};
    std::unique_ptr<net::steady_timer> m_acceptRetryTimer; // I/O thread only while running
    uint32_t m_acceptFailures{0};                          // consecutive; I/O thread only while running
    uint64_t m_acceptAttempts{0};                          // async_accept calls; I/O thread only while running
    std::function<void()> m_beforeSessionAdmitForTest;     // test seam; set before start()
    static constexpr std::chrono::milliseconds kAcceptBackoffMin{10};
    static constexpr std::chrono::milliseconds kAcceptBackoffMax{1000};
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

    std::function<FeedAdmission(const std::string&)> m_feedAdmissionHandler;

    std::mutex m_tradingBroadcastMutex;
    std::vector<std::pair<uint64_t, std::function<void(const std::string&)>>> m_tradingBroadcasters;
    std::atomic<uint64_t> m_nextBroadcasterId{0};
public:
    uint64_t registerTradingBroadcaster(std::function<void(const std::string&)> fn);
    void unregisterTradingBroadcaster(uint64_t id);
private:
};
