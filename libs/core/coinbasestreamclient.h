#ifndef COINBASESTREAMCLIENT_H
#define COINBASESTREAMCLIENT_H

#ifdef ASIO_STANDALONE
#ifndef ASIO_DISABLE_IPV6
#define ASIO_DISABLE_IPV6
#endif
#else
#ifndef BOOST_ASIO_DISABLE_IPV6
#define BOOST_ASIO_DISABLE_IPV6
#endif
#endif

#include <websocketpp/config/asio_client.hpp>
#include <websocketpp/client.hpp>
#include <nlohmann/json.hpp>
#include "tradedata.h"

#include <deque>
#include <string>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>
#include <fstream>
#include <filesystem>

class CoinbaseStreamClient {
public:
    explicit CoinbaseStreamClient(bool useSandbox = false);
    ~CoinbaseStreamClient();

    void subscribe(const std::vector<std::string>& symbols);
    void start();
    std::vector<Trade> getRecentTrades(const std::string& symbol);
    std::vector<Trade> getNewTrades(const std::string& symbol, const std::string& lastTradeId = "");

private:
    using client = websocketpp::client<websocketpp::config::asio_tls_client>;
    using connection_hdl = websocketpp::connection_hdl;

    void connect();
    void run();

    void onOpen(connection_hdl hdl);
    void onMessage(connection_hdl hdl, client::message_ptr msg);
    void onClose(connection_hdl hdl);
    void onFail(connection_hdl hdl);
    std::string buildSubscribeMessage() const;
    std::chrono::system_clock::time_point parseTimestamp(const std::string& t);
    void logTrade(const std::string& product, const nlohmann::json& j);
    std::ofstream& getLogStream(const std::string& product);

    client m_client;
    connection_hdl m_hdl;
    std::thread m_thread;
    std::atomic<bool> m_shouldReconnect;
    std::vector<std::string> m_symbols;
    std::mutex m_mutex;
    std::unordered_map<std::string, std::deque<Trade>> m_tradeBuffers;
    const std::size_t m_maxTrades = 1000;
    std::unordered_map<std::string, std::ofstream> m_logStreams;
    std::string m_endpoint;
    std::filesystem::path m_logDir{"logs"};
    const std::uintmax_t m_maxLogSize = 10 * 1024 * 1024;
};

#endif // COINBASESTREAMCLIENT_H
