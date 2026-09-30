#pragma once
#include "WsTransport.hpp"
#include <boost/beast/websocket.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/http.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core/tcp_stream.hpp>  // ensure tcp_stream is declared
#include <boost/asio/ip/tcp.hpp>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <deque>
#include <string>
#include <openssl/ssl.h>

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

// Every connect() ends in exactly one onStatus(true) or onStatus(false) within
// connectTimeout; every close() ends in onStatus(false) within closeTimeout.
// A superseded or timed-out attempt never reports again (attempt id guard).
class BeastWsTransport : public WsTransport {
public:
    using WsStream = websocket::stream<beast::ssl_stream<beast::tcp_stream>>;
    using ResolveHandler = std::function<void(beast::error_code, tcp::resolver::results_type)>;
    // Test seam: replaces DNS. May call back from any thread, late or never, but
    // not after the transport is destroyed.
    using ResolveFn = std::function<void(const std::string& host, const std::string& port, ResolveHandler)>;
    struct Options {
        std::chrono::milliseconds connectTimeout{20000}; // resolve + TCP + TLS + WS handshake
        std::chrono::milliseconds closeTimeout{3000};    // WS close handshake with the peer
        ResolveFn resolve;
    };

    BeastWsTransport(net::io_context& ioc, ssl::context& sslCtx);
    BeastWsTransport(net::io_context& ioc, ssl::context& sslCtx, Options options);

    void connect(std::string host, std::string port, std::string target) override;
    void close() override;
    void send(std::string msg) override;

    void onMessage(MessageCb cb) override { onMessage_ = std::move(cb); }
    void onStatus(StatusCb cb) override { onStatus_ = std::move(cb); }
    void onError(ErrorCb cb) override { onError_ = std::move(cb); }

private:
    enum class Phase { Idle, Resolve, TcpConnect, TlsHandshake, WsHandshake, Open, Closing };
    static const char* phaseName(Phase phase);

    // Callbacks
    MessageCb onMessage_;
    StatusCb  onStatus_;
    ErrorCb   onError_;

    // Beast state
    net::strand<net::io_context::executor_type> strand_;
    ssl::context& sslCtx_;
    Options options_;
    tcp::resolver resolver_;
    // One per attempt. Pending handlers hold a reference, so an abandoned
    // connection (stream, buffers, queued writes) outlives its aborted ops.
    struct Connection {
        Connection(net::strand<net::io_context::executor_type> ex, ssl::context& ctx) : ws(ex, ctx) {}
        WsStream ws;
        beast::flat_buffer buf;
        websocket::response_type handshakeResponse;
        std::deque<std::string> writeQueue;
    };
    using ConnPtr = std::shared_ptr<Connection>;
    ConnPtr conn_;
    net::steady_timer deadlineTimer_;
    net::steady_timer firstFrameTimer_;
    net::steady_timer pingTimer_;

    // State
    std::string host_;
    std::string port_;
    std::string target_;
    bool sawInboundFrame_ = false;
    // Strand-owned. Bumped by connect(), close() and every terminal outcome.
    uint64_t attempt_ = 0;
    Phase phase_ = Phase::Idle;

    void cancelTimers();
    void closeSocket();
    void armDeadline(uint64_t id, std::chrono::milliseconds timeout);
    void fail(uint64_t id, const std::string& error);
    // error: reported instead of ec.message() when not empty.
    void finishClose(uint64_t id, beast::error_code ec, const std::string& error = {});

    // Handlers
    void onResolve(uint64_t id, beast::error_code ec, tcp::resolver::results_type results);
    void onConnect(uint64_t id, beast::error_code ec);
    void onSslHandshake(uint64_t id, beast::error_code ec);
    void onWsHandshake(uint64_t id, beast::error_code ec);
    void doRead(uint64_t id);
    void onRead(uint64_t id, beast::error_code ec);
    void doWrite(uint64_t id);
    void schedulePing(uint64_t id);
};
