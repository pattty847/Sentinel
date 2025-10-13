#include "BeastWsTransport.hpp"
#include <boost/beast/core.hpp>  // covers buffers, flat_buffer, etc.
#include <string_view>


void BeastWsTransport::connect(std::string host, std::string port, std::string target) {
    host_ = std::move(host);
    port_ = std::move(port);
    target_ = std::move(target);

    resolver_.async_resolve(host_, port_,
        [this](beast::error_code ec, tcp::resolver::results_type results){
            onResolve(ec, results);
        });
}

void BeastWsTransport::close() {
    if (ws_.is_open()) {
        ws_.async_close(websocket::close_code::normal, [this](beast::error_code ec){
            if (ec) { if (onError_) onError_(ec.message()); }
            if (onStatus_) onStatus_(false);
        });
    } else {
        if (onStatus_) onStatus_(false);
    }
}

void BeastWsTransport::send(std::string msg) {
    net::post(strand_, [this, m = std::move(msg)]() mutable {
        writeQueue_.emplace_back(std::move(m));
        if (writeQueue_.size() == 1) {
            doWrite();
        }
    });
}

void BeastWsTransport::onResolve(beast::error_code ec, tcp::resolver::results_type results) {
    if (ec) { if (onError_) onError_(ec.message()); if (onStatus_) onStatus_(false); return; }
    beast::get_lowest_layer(ws_).expires_after(std::chrono::seconds(30));
    beast::get_lowest_layer(ws_).async_connect(results,
        [this](beast::error_code ec, tcp::resolver::results_type::endpoint_type ep){
            (void)ep; onConnect(ec, ep);
        });
}

void BeastWsTransport::onConnect(beast::error_code ec, tcp::resolver::results_type::endpoint_type) {
    if (ec) { if (onError_) onError_(ec.message()); if (onStatus_) onStatus_(false); return; }
    if (!SSL_set_tlsext_host_name(ws_.next_layer().native_handle(), host_.c_str())) {
        beast::error_code ssl_ec(static_cast<int>(::ERR_get_error()), net::error::get_ssl_category());
        if (onError_) onError_(ssl_ec.message()); if (onStatus_) onStatus_(false); return;
    }
    if (!SSL_set1_host(ws_.next_layer().native_handle(), host_.c_str())) {
        beast::error_code ssl_ec(static_cast<int>(::ERR_get_error()), net::error::get_ssl_category());
        if (onError_) onError_(ssl_ec.message()); if (onStatus_) onStatus_(false); return;
    }
    ws_.next_layer().set_verify_mode(ssl::verify_peer);
    ws_.next_layer().async_handshake(ssl::stream_base::client,
        [this](beast::error_code ec){ onSslHandshake(ec); });
}

void BeastWsTransport::onSslHandshake(beast::error_code ec) {
    if (ec) { if (onError_) onError_(ec.message()); if (onStatus_) onStatus_(false); return; }
    beast::get_lowest_layer(ws_).expires_never();
    ws_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));
    ws_.async_handshake(host_, target_,
        [this](beast::error_code ec){ onWsHandshake(ec); });
}

void BeastWsTransport::onWsHandshake(beast::error_code ec) {
    if (ec) { if (onError_) onError_(ec.message()); if (onStatus_) onStatus_(false); return; }
    if (onStatus_) onStatus_(true);
    doRead();
    schedulePing();
}

void BeastWsTransport::doRead() {
    ws_.async_read(buf_, [this](beast::error_code ec, std::size_t bytes){ onRead(ec, bytes); });
}

void BeastWsTransport::onRead(beast::error_code ec, std::size_t) {
    if (ec) {
        if (onError_) onError_(ec.message());
        if (onStatus_) onStatus_(false);
        return;
    }

    if (onMessage_) {
        auto b = buf_.data();
        std::string payload(static_cast<const char*>(b.data()), b.size());
        buf_.consume(buf_.size());
        onMessage_(std::move(payload));
    }

    doRead();
}

void BeastWsTransport::doWrite() {
    if (writeQueue_.empty()) return;
    const auto& front = writeQueue_.front();
    ws_.async_write(net::buffer(front), [this](beast::error_code ec, std::size_t){
        if (ec) {
            if (onError_) onError_(ec.message());
            if (onStatus_) onStatus_(false);
            return;
        }
        writeQueue_.pop_front();
        if (!writeQueue_.empty()) doWrite();
    });
}

void BeastWsTransport::schedulePing() {
    pingTimer_.expires_after(std::chrono::seconds(25));
    pingTimer_.async_wait([this](beast::error_code ec){
        if (ec) return;
        ws_.async_ping({}, [this](beast::error_code ec2){
            if (ec2) { if (onError_) onError_(ec2.message()); if (onStatus_) onStatus_(false); return; }
            schedulePing();
        });
    });
}
