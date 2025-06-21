#ifndef COINBASESTREAMCLIENT_H
#define COINBASESTREAMCLIENT_H

#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/strand.hpp>
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
    void run();
    void on_resolve(boost::beast::error_code ec, boost::asio::ip::tcp::resolver::results_type results);
    void on_connect(boost::beast::error_code ec, boost::asio::ip::tcp::resolver::results_type::endpoint_type);
    void on_ssl_handshake(boost::beast::error_code ec);
    void on_handshake(boost::beast::error_code ec);
    void on_write(boost::beast::error_code ec, std::size_t bytes_transferred);
    void on_read(boost::beast::error_code ec, std::size_t bytes_transferred);
    void on_close(boost::beast::error_code ec);
    
    void do_close();
    void reconnect();
    
    std::string buildSubscribeMessage();
    std::chrono::system_clock::time_point parseTimestamp(const std::string& t);
    void logTrade(const std::string& product, const nlohmann::json& j);
    std::ofstream& getLogStream(const std::string& product);

    std::string m_host;
    std::string m_port;
    std::string m_endpoint;

    boost::asio::io_context m_ioc;
    boost::asio::ssl::context m_ssl_ctx;
    boost::asio::ip::tcp::resolver m_resolver;
    boost::beast::websocket::stream<boost::beast::ssl_stream<boost::beast::tcp_stream>> m_ws;
    boost::beast::flat_buffer m_buffer;

    std::thread m_thread;
    std::atomic<bool> m_running;
    
    std::mutex m_mutex;
    std::vector<std::string> m_symbols;
    std::string m_subscribe_message;

    std::unordered_map<std::string, std::deque<Trade>> m_tradeBuffers;
    const std::size_t m_maxTrades = 1000;
    std::unordered_map<std::string, std::ofstream> m_logStreams;
    std::filesystem::path m_logDir{"logs"};
    const std::uintmax_t m_maxLogSize = 10 * 1024 * 1024;
};

#endif // COINBASESTREAMCLIENT_H
