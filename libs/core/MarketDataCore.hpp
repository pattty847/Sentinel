#pragma once
// ─────────────────────────────────────────────────────────────
// MarketDataCore – owns Boost-Beast websocket & IO thread.
// Responsibilities: network I/O, reconnection, back-pressure.
// Emits fully‐parsed domain objects to DataCache.
// ─────────────────────────────────────────────────────────────
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/strand.hpp>
#include <nlohmann/json.hpp>
#include <atomic>
#include <thread>
#include "Authenticator.hpp"
#include "DataCache.hpp"

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

class MarketDataCore {
public:
    MarketDataCore(const std::vector<std::string>& products,
                   Authenticator& auth,
                   DataCache& cache);

    ~MarketDataCore();                   // RAII shutdown
    void start();
    void stop();

    // Non-copyable, non-movable (manages thread)
    MarketDataCore(const MarketDataCore&) = delete;
    MarketDataCore& operator=(const MarketDataCore&) = delete;
    MarketDataCore(MarketDataCore&&) = delete;
    MarketDataCore& operator=(MarketDataCore&&) = delete;

private:
    // Connection/handshake chain
    void run();
    void onResolve(beast::error_code, tcp::resolver::results_type);
    void onConnect(beast::error_code, tcp::resolver::results_type::endpoint_type);
    void onSslHandshake(beast::error_code);
    void onWsHandshake(beast::error_code);
    void onWrite(beast::error_code, std::size_t);
    void onRead(beast::error_code, std::size_t);
    void doClose();
    void onClose(beast::error_code);
    void scheduleReconnect();

    // Helpers
    [[nodiscard]] std::string buildSubscribe(const std::string& channel) const;
    void dispatch(const nlohmann::json&);

    // Members
    const std::string               m_host   = "advanced-trade-ws.coinbase.com";
    const std::string               m_port   = "443";
    const std::string               m_target = "/";
    std::vector<std::string>        m_products;

    Authenticator&                  m_auth;
    DataCache&                      m_cache;

    net::io_context                 m_ioc;
    ssl::context                    m_sslCtx{ssl::context::tlsv12_client};
    tcp::resolver                   m_resolver{net::make_strand(m_ioc)};
    websocket::stream<beast::ssl_stream<beast::tcp_stream>>
                                    m_ws{net::make_strand(m_ioc), m_sslCtx};
    beast::flat_buffer              m_buf;
    std::atomic<bool>               m_running{false};
    std::thread                     m_ioThread;
}; 