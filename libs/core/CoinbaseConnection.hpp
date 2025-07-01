#pragma once
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/strand.hpp>
#include <functional>
#include <string>
#include <atomic>
#include <thread>
#include "SentinelLogging.hpp"

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

// ----------------------------------------------------------------------------
// CoinbaseConnection
// Manages the Boost.Beast WebSocket connection lifecycle. Does not understand
// the Coinbase protocol. Emits generic callbacks for higher level components.
// ----------------------------------------------------------------------------
class CoinbaseConnection {
public:
    CoinbaseConnection(const std::string& host = "advanced-trade-ws.coinbase.com",
                       const std::string& port = "443",
                       const std::string& target = "/");
    ~CoinbaseConnection();

    void start();
    void stop();
    void send(const std::string& message);

    // Event callbacks
    std::function<void()>                   on_open;
    std::function<void(const std::string&)> on_message;
    std::function<void()>                   on_close;
    std::function<void(beast::error_code)>  on_error;

    CoinbaseConnection(const CoinbaseConnection&) = delete;
    CoinbaseConnection& operator=(const CoinbaseConnection&) = delete;

private:
    void run();
    void onResolve(beast::error_code, tcp::resolver::results_type);
    void onConnect(beast::error_code, tcp::resolver::results_type::endpoint_type);
    void onSslHandshake(beast::error_code);
    void onWsHandshake(beast::error_code);
    void onWrite(beast::error_code, std::size_t);
    void onRead(beast::error_code, std::size_t);
    void doClose();
    void onClose(beast::error_code);

    std::string m_host;
    std::string m_port;
    std::string m_target;

    net::io_context m_ioc;
    ssl::context m_sslCtx{ssl::context::tlsv12_client};
    tcp::resolver m_resolver{net::make_strand(m_ioc)};
    websocket::stream<beast::ssl_stream<beast::tcp_stream>> m_ws{net::make_strand(m_ioc), m_sslCtx};
    beast::flat_buffer m_buffer;
    std::atomic<bool> m_running{false};
    std::thread m_thread;
};
