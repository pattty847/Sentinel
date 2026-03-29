#include "BeastWsTransport.hpp"
#include "SentinelLogging.hpp"
#include <boost/beast/core.hpp>  // covers buffers, flat_buffer, etc.
#include <algorithm>
#include <atomic>
#include <string_view>

void BeastWsTransport::resetStream() {
    ws_ = std::make_unique<WsStream>(strand_, sslCtx_);
    buf_.consume(buf_.size());
    handshakeResponse_ = {};
    sawInboundFrame_ = false;
}

void BeastWsTransport::connect(std::string host, std::string port, std::string target) {
    net::post(strand_, [this, h = std::move(host), p = std::move(port), t = std::move(target)]() mutable {
        host_ = std::move(h);
        port_ = std::move(p);
        target_ = std::move(t);
        firstFrameTimer_.cancel();
        pingTimer_.cancel();
        resolver_.cancel();
        resetStream();

        resolver_.async_resolve(host_, port_,
            [this](beast::error_code ec, tcp::resolver::results_type results){
                onResolve(ec, results);
            });
    });
}

void BeastWsTransport::close() {
    net::post(strand_, [this]() {
        firstFrameTimer_.cancel();
        pingTimer_.cancel();
        resolver_.cancel();
        writeQueue_.clear();
        if (ws_ && stream().is_open()) {
            stream().async_close(websocket::close_code::normal, [this](beast::error_code ec){
                if (ec) { if (onError_) onError_(ec.message()); }
                resetStream();
                if (onStatus_) onStatus_(false);
            });
        } else {
            resetStream();
            if (onStatus_) onStatus_(false);
        }
    });
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
    beast::get_lowest_layer(stream()).expires_after(std::chrono::seconds(30));
    beast::get_lowest_layer(stream()).async_connect(results,
        [this](beast::error_code ec, tcp::resolver::results_type::endpoint_type ep){
            (void)ep; onConnect(ec, ep);
        });
}

void BeastWsTransport::onConnect(beast::error_code ec, tcp::resolver::results_type::endpoint_type) {
    if (ec) { if (onError_) onError_(ec.message()); if (onStatus_) onStatus_(false); return; }
    if (!SSL_set_tlsext_host_name(stream().next_layer().native_handle(), host_.c_str())) {
        beast::error_code ssl_ec(static_cast<int>(::ERR_get_error()), net::error::get_ssl_category());
        if (onError_) onError_(ssl_ec.message()); if (onStatus_) onStatus_(false); return;
    }
    if (!SSL_set1_host(stream().next_layer().native_handle(), host_.c_str())) {
        beast::error_code ssl_ec(static_cast<int>(::ERR_get_error()), net::error::get_ssl_category());
        if (onError_) onError_(ssl_ec.message()); if (onStatus_) onStatus_(false); return;
    }
    stream().next_layer().set_verify_mode(ssl::verify_peer);
    stream().next_layer().async_handshake(ssl::stream_base::client,
        [this](beast::error_code ec){ onSslHandshake(ec); });
}

void BeastWsTransport::onSslHandshake(beast::error_code ec) {
    if (ec) { if (onError_) onError_(ec.message()); if (onStatus_) onStatus_(false); return; }
    beast::get_lowest_layer(stream()).expires_never();
    stream().set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));
    stream().set_option(websocket::stream_base::decorator([](websocket::request_type& req) {
        req.set(beast::http::field::user_agent, "Sentinel/MarketDataCore");
        req.set(beast::http::field::origin, "https://advanced-trade.coinbase.com");
    }));
    handshakeResponse_ = {};
    stream().async_handshake(handshakeResponse_, host_, target_,
        [this](beast::error_code ec){ onWsHandshake(ec); });
}

void BeastWsTransport::onWsHandshake(beast::error_code ec) {
    if (ec) {
        if (onError_) {
            std::string msg = "WS handshake failed: ";
            msg += ec.message();
            if (handshakeResponse_.result_int() != 0) {
                msg += " | status=" + std::to_string(handshakeResponse_.result_int());
                msg += " reason=" + std::string(handshakeResponse_.reason());
                std::string headers;
                for (const auto& field : handshakeResponse_) {
                    headers += std::string(field.name_string()) + ": " + std::string(field.value()) + "; ";
                }
                if (!headers.empty()) {
                    msg += " headers=[" + headers + "]";
                }
            }
            onError_(msg);
        }
        resetStream();
        if (onStatus_) onStatus_(false);
        return;
    }
    stream().control_callback([this](websocket::frame_type kind, beast::string_view) {
        if (kind == websocket::frame_type::close) {
            const auto reason = stream().reason();
            if (onError_) {
                std::string msg = "WS close: ";
                msg += reason.reason.c_str();
                onError_(msg);
            }
        }
    });
    firstFrameTimer_.expires_after(std::chrono::seconds(5));
    firstFrameTimer_.async_wait([this](beast::error_code ec) {
        if (ec) {
            return;
        }
        if (!sawInboundFrame_) {
            sLog_Warning("MDC transport: no inbound WS frames within 5s of handshake");
        }
    });
    if (onStatus_) onStatus_(true);
    doRead();
    schedulePing();
}

void BeastWsTransport::doRead() {
    stream().async_read(buf_, [this](beast::error_code ec, std::size_t bytes){ onRead(ec, bytes); });
}

void BeastWsTransport::onRead(beast::error_code ec, std::size_t) {
    if (ec) {
        firstFrameTimer_.cancel();
        pingTimer_.cancel();
        if (ec == websocket::error::closed) {
            const auto reason = stream().reason();
            if (onError_) {
                std::string msg = "WS closed: ";
                msg += reason.reason.c_str();
                onError_(msg);
            }
        }
        if (onError_) onError_(ec.message());
        resetStream();
        if (onStatus_) onStatus_(false);
        return;
    }

    if (onMessage_) {
        static std::atomic<int> s_loggedFrames{0};
        auto b = buf_.data();
        std::string payload(static_cast<const char*>(b.data()), b.size());
        buf_.consume(buf_.size());
        sawInboundFrame_ = true;
        firstFrameTimer_.cancel();
        const int logged = s_loggedFrames.fetch_add(1, std::memory_order_relaxed);
        if (logged < 5) {
            const size_t previewLen = std::min<size_t>(payload.size(), 400);
            sLog_Data(std::string("MDC RX raw bytes=") +
                      std::to_string(payload.size()) +
                      " preview=" + payload.substr(0, previewLen));
        }
        onMessage_(std::move(payload));
    }

    doRead();
}

void BeastWsTransport::doWrite() {
    if (writeQueue_.empty()) return;
    const auto& front = writeQueue_.front();
    stream().async_write(net::buffer(front), [this](beast::error_code ec, std::size_t){
        if (ec) {
            pingTimer_.cancel();
            if (onError_) onError_(ec.message());
            resetStream();
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
        stream().async_ping({}, [this](beast::error_code ec2){
            if (ec2) {
                if (onError_) onError_(ec2.message());
                resetStream();
                if (onStatus_) onStatus_(false);
                return;
            }
            schedulePing();
        });
    });
}
