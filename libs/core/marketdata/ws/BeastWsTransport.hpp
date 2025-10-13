#pragma once
#include "WsTransport.hpp"
#include <boost/beast/websocket.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core/tcp_stream.hpp>  // ensure tcp_stream is declared
#include <boost/asio/ip/tcp.hpp>
#include <deque>
#include <string>
#include <openssl/ssl.h>

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

class BeastWsTransport : public WsTransport {
public:
    explicit BeastWsTransport(net::io_context& ioc, ssl::context& sslCtx)
        : strand_(ioc.get_executor())
        , resolver_(strand_)
        , ws_(strand_, sslCtx)
        , pingTimer_(strand_)
    {}

    void connect(std::string host, std::string port, std::string target) override;
    void close() override;
    void send(std::string msg) override;

    void onMessage(MessageCb cb) override { onMessage_ = std::move(cb); }
    void onStatus(StatusCb cb) override { onStatus_ = std::move(cb); }
    void onError(ErrorCb cb) override { onError_ = std::move(cb); }

private:
    // Callbacks
    MessageCb onMessage_;
    StatusCb  onStatus_;
    ErrorCb   onError_;

    // Beast state
    net::strand<net::io_context::executor_type> strand_;
    tcp::resolver resolver_;
    websocket::stream<beast::ssl_stream<beast::tcp_stream>> ws_;
    beast::flat_buffer buf_;
    net::steady_timer pingTimer_;
    std::deque<std::string> writeQueue_;

    // State
    std::string host_;
    std::string port_;
    std::string target_;

    // Handlers
    void onResolve(beast::error_code ec, tcp::resolver::results_type results);
    void onConnect(beast::error_code ec, tcp::resolver::results_type::endpoint_type);
    void onSslHandshake(beast::error_code ec);
    void onWsHandshake(beast::error_code ec);
    void doRead();
    void onRead(beast::error_code ec, std::size_t bytes);
    void doWrite();
    void schedulePing();
};


