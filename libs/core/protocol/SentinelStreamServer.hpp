#pragma once
#include <QObject>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <memory>
#include <unordered_set>
#include <mutex>
#include <thread>
#include <functional>
#include <vector>
#include <atomic>
#include "../servermodel/ServerDataModel.hpp"
#include "../config/ConfigTypes.hpp"
#include "../trading/TradingTypes.hpp"
#include "../trading/AlgoEngine.hpp"

namespace trading { class TradingEngine; }

class Authenticator;
class CoinbaseRestClient;

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
    void algoOrderEventBroadcast(const trading::AlgoOrderEvent& event);
    void pnlSnapshotBroadcast(const trading::PnlSnapshot& snapshot);

public:
    void notifyClientSubscribed(const std::string& symbol);
    void notifyClientUnsubscribed(const std::string& symbol);
    CoinbaseRestClient& restClient();
    const ServerConfig& serverConfig() const { return m_serverConfig; }
    void processTradeCommand(const trading::TradeCommand& command);
    void broadcastOrderUpdate(const trading::OrderUpdate& update);
    void broadcastPositionUpdate(const trading::PositionUpdate& update);
    void broadcastAlgoOrderEvent(const trading::AlgoOrderEvent& event);
    void broadcastPnlSnapshot(const trading::PnlSnapshot& snapshot);
    void broadcastCoinbaseLatency(int milliseconds);
    trading::AlgoEngine& algoEngine() { return *m_algoEngine; }
    trading::AlgoEngine* algoEnginePtr() const { return m_algoEngine.get(); }
    trading::TradingEngine* tradingEnginePtr() const { return m_tradingEngine.get(); }
    uint64_t registerLatencySender(std::function<void(int)> sendFn);
    void unregisterLatencySender(uint64_t id);

private:
    void doAccept();

    ServerDataModel& m_model;
    std::unique_ptr<CoinbaseRestClient> m_restClient;
    ServerConfig m_serverConfig;
    int m_port;

    net::io_context m_ioc;
    ssl::context m_sslCtx{ssl::context::tlsv13_server};
    std::unique_ptr<tcp::acceptor> m_acceptor;
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::unique_ptr<trading::TradingEngine> m_tradingEngine;
    std::unique_ptr<trading::AlgoEngine> m_algoEngine;

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

