#include "BeastWsTransport.hpp"
#include "SentinelLogging.hpp"
#include <boost/beast/core.hpp>  // covers buffers, flat_buffer, etc.
#include <algorithm>
#include <atomic>
#include <string_view>

// Every async handler captures the attempt id it was started under and the
// connection it runs on. connect(), close() and each terminal outcome bump
// attempt_, so a callback from a superseded or timed-out attempt is a no-op and
// its connection object stays alive until that callback has run.

BeastWsTransport::BeastWsTransport(net::io_context& ioc, ssl::context& sslCtx)
    : BeastWsTransport(ioc, sslCtx, Options{}) {}

BeastWsTransport::BeastWsTransport(net::io_context& ioc, ssl::context& sslCtx, Options options)
    : strand_(ioc.get_executor())
    , sslCtx_(sslCtx)
    , options_(std::move(options))
    , resolver_(strand_)
    , conn_(std::make_shared<Connection>(strand_, sslCtx_))
    , deadlineTimer_(strand_)
    , firstFrameTimer_(strand_)
    , pingTimer_(strand_)
{}

const char* BeastWsTransport::phaseName(Phase phase) {
    switch (phase) {
    case Phase::Idle: return "idle";
    case Phase::Resolve: return "resolve";
    case Phase::TcpConnect: return "tcp-connect";
    case Phase::TlsHandshake: return "tls-handshake";
    case Phase::WsHandshake: return "ws-handshake";
    case Phase::Open: return "open";
    case Phase::Closing: return "closing";
    }
    return "?";
}

void BeastWsTransport::cancelTimers() {
    deadlineTimer_.cancel();
    firstFrameTimer_.cancel();
    pingTimer_.cancel();
}

// Aborts every pending op on the current connection; their handlers see a stale id.
void BeastWsTransport::closeSocket() {
    resolver_.cancel();
    beast::error_code ignored;
    beast::get_lowest_layer(conn_->ws).socket().close(ignored);
}

void BeastWsTransport::armDeadline(uint64_t id, std::chrono::milliseconds timeout) {
    deadlineTimer_.expires_after(timeout);
    deadlineTimer_.async_wait([this, id, timeout](beast::error_code ec) {
        if (ec || id != attempt_) return;
        if (phase_ == Phase::Closing) {
            sLog_Warning("MDC transport close timed out, dropping socket: timeoutMs=" << timeout.count()
                         << " host=" << host_);
            // Explicit text: net::error::timed_out's message is platform prose
            // (WSAETIMEDOUT on Windows never says "timed out").
            finishClose(id, net::error::timed_out,
                        "close timed out after " + std::to_string(timeout.count()) + "ms");
            return;
        }
        // The handshake can succeed while this expiry is already queued; cancel()
        // cannot retract it, and the success path keeps the attempt id.
        if (phase_ == Phase::Open || phase_ == Phase::Idle) return;
        sLog_Warning("MDC transport connect timed out: phase=" << phaseName(phase_)
                     << " timeoutMs=" << timeout.count() << " host=" << host_);
        fail(id, std::string("connect timed out in ") + phaseName(phase_) + " after "
                 + std::to_string(timeout.count()) + "ms");
    });
}

// Terminal failure of the current attempt or open connection: exactly one down.
void BeastWsTransport::fail(uint64_t id, const std::string& error) {
    if (id != attempt_) return;
    ++attempt_;
    phase_ = Phase::Idle;
    cancelTimers();
    closeSocket();
    if (onError_) onError_(error);
    if (onStatus_) onStatus_(false);
}

void BeastWsTransport::finishClose(uint64_t id, beast::error_code ec, const std::string& error) {
    if (id != attempt_) return;
    ++attempt_;
    phase_ = Phase::Idle;
    cancelTimers();
    closeSocket();
    if (ec && onError_) onError_(error.empty() ? ec.message() : error);
    if (onStatus_) onStatus_(false);
}

void BeastWsTransport::connect(std::string host, std::string port, std::string target) {
    net::post(strand_, [this, h = std::move(host), p = std::move(port), t = std::move(target)]() mutable {
        host_ = std::move(h);
        port_ = std::move(p);
        target_ = std::move(t);
        const uint64_t id = ++attempt_; // supersedes any attempt still in flight
        cancelTimers();
        closeSocket();
        conn_ = std::make_shared<Connection>(strand_, sslCtx_);
        sawInboundFrame_ = false;
        phase_ = Phase::Resolve;
        armDeadline(id, options_.connectTimeout);
        sLog_Data("MDC transport connecting: host=" << host_ << " port=" << port_
                  << " target=" << target_ << " attempt=" << id
                  << " timeoutMs=" << options_.connectTimeout.count());

        if (options_.resolve) {
            options_.resolve(host_, port_, [this, id](beast::error_code ec, tcp::resolver::results_type results) {
                net::post(strand_, [this, id, ec, results = std::move(results)] { onResolve(id, ec, results); });
            });
            return;
        }
        resolver_.async_resolve(host_, port_,
            [this, id](beast::error_code ec, tcp::resolver::results_type results) {
                onResolve(id, ec, results);
            });
    });
}

void BeastWsTransport::close() {
    net::post(strand_, [this]() {
        const uint64_t id = ++attempt_; // an in-flight connect attempt never reports
        firstFrameTimer_.cancel();
        pingTimer_.cancel();
        resolver_.cancel();
        if (phase_ == Phase::Open && conn_->ws.is_open()) {
            // Bounded: a dead peer never answers the close frame, and Beast's own
            // close timeout is 30 s.
            phase_ = Phase::Closing;
            armDeadline(id, options_.closeTimeout);
            conn_->ws.async_close(websocket::close_code::normal, [this, id, c = conn_](beast::error_code ec) {
                finishClose(id, ec);
            });
        } else {
            finishClose(id, {});
        }
    });
}

void BeastWsTransport::send(std::string msg) {
    net::post(strand_, [this, m = std::move(msg)]() mutable {
        if (phase_ != Phase::Open) {
            sLog_Data("MDC transport dropping send while not open: phase=" << phaseName(phase_)
                      << " bytes=" << m.size());
            return;
        }
        conn_->writeQueue.emplace_back(std::move(m));
        if (conn_->writeQueue.size() == 1) {
            doWrite(attempt_);
        }
    });
}

void BeastWsTransport::onResolve(uint64_t id, beast::error_code ec, tcp::resolver::results_type results) {
    if (id != attempt_) return;
    if (ec) { fail(id, "resolve failed: " + ec.message()); return; }
    phase_ = Phase::TcpConnect;
    beast::get_lowest_layer(conn_->ws).async_connect(results,
        [this, id, c = conn_](beast::error_code ec, tcp::resolver::results_type::endpoint_type) {
            onConnect(id, ec);
        });
}

void BeastWsTransport::onConnect(uint64_t id, beast::error_code ec) {
    if (id != attempt_) return;
    if (ec) { fail(id, "tcp connect failed: " + ec.message()); return; }
    auto& tls = conn_->ws.next_layer();
    if (!SSL_set_tlsext_host_name(tls.native_handle(), host_.c_str()) ||
        !SSL_set1_host(tls.native_handle(), host_.c_str())) {
        beast::error_code ssl_ec(static_cast<int>(::ERR_get_error()), net::error::get_ssl_category());
        fail(id, ssl_ec.message());
        return;
    }
    tls.set_verify_mode(ssl::verify_peer);
    phase_ = Phase::TlsHandshake;
    tls.async_handshake(ssl::stream_base::client,
        [this, id, c = conn_](beast::error_code ec) { onSslHandshake(id, ec); });
}

void BeastWsTransport::onSslHandshake(uint64_t id, beast::error_code ec) {
    if (id != attempt_) return;
    if (ec) { fail(id, "tls handshake failed: " + ec.message()); return; }
    auto& ws = conn_->ws;
    ws.set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));
    ws.set_option(websocket::stream_base::decorator([](websocket::request_type& req) {
        req.set(beast::http::field::user_agent, "Sentinel/MarketDataCore");
        req.set(beast::http::field::origin, "https://advanced-trade.coinbase.com");
    }));
    phase_ = Phase::WsHandshake;
    ws.async_handshake(conn_->handshakeResponse, host_, target_,
        [this, id, c = conn_](beast::error_code ec) { onWsHandshake(id, ec); });
}

void BeastWsTransport::onWsHandshake(uint64_t id, beast::error_code ec) {
    if (id != attempt_) return;
    const auto& response = conn_->handshakeResponse;
    if (ec) {
        std::string msg = "WS handshake failed: ";
        msg += ec.message();
        if (response.result_int() != 0) {
            msg += " | status=" + std::to_string(response.result_int());
            msg += " reason=" + std::string(response.reason());
            std::string headers;
            for (const auto& field : response) {
                headers += std::string(field.name_string()) + ": " + std::string(field.value()) + "; ";
            }
            if (!headers.empty()) {
                msg += " headers=[" + headers + "]";
            }
        }
        fail(id, msg);
        return;
    }
    deadlineTimer_.cancel();
    phase_ = Phase::Open;
    // Raw pointer: the callback is stored inside the stream it inspects.
    conn_->ws.control_callback([this, ws = &conn_->ws](websocket::frame_type kind, beast::string_view) {
        if (kind == websocket::frame_type::close && onError_) {
            onError_(std::string("WS close: ") + ws->reason().reason.c_str());
        }
    });
    sLog_Data("MDC transport WS handshake ok: host=" << host_ << " target=" << target_
              << " status=" << response.result_int() << " attempt=" << id);
    firstFrameTimer_.expires_after(std::chrono::seconds(5));
    firstFrameTimer_.async_wait([this, id](beast::error_code ec) {
        if (ec || id != attempt_) return;
        if (!sawInboundFrame_) {
            sLog_Warning("MDC transport: no inbound WS frames within 5s of handshake: host=" << host_
                         << " target=" << target_);
        }
    });
    if (onStatus_) onStatus_(true);
    // onStatus(true) may call close() or connect(); those are posted, so this
    // attempt is still current here.
    doRead(id);
    schedulePing(id);
}

void BeastWsTransport::doRead(uint64_t id) {
    conn_->ws.async_read(conn_->buf, [this, id, c = conn_](beast::error_code ec, std::size_t) { onRead(id, ec); });
}

void BeastWsTransport::onRead(uint64_t id, beast::error_code ec) {
    if (id != attempt_) return;
    if (ec) {
        if (ec == websocket::error::closed && onError_) {
            onError_(std::string("WS closed: ") + conn_->ws.reason().reason.c_str());
        }
        fail(id, ec.message());
        return;
    }

    auto& buf = conn_->buf;
    if (onMessage_) {
        static std::atomic<int> s_loggedFrames{0};
        auto b = buf.data();
        std::string payload(static_cast<const char*>(b.data()), b.size());
        buf.consume(buf.size());
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
        // The message callback may have closed or replaced this connection.
        if (id != attempt_) return;
    } else {
        buf.consume(buf.size());
    }

    doRead(id);
}

void BeastWsTransport::doWrite(uint64_t id) {
    if (conn_->writeQueue.empty()) return;
    conn_->ws.async_write(net::buffer(conn_->writeQueue.front()),
        [this, id, c = conn_](beast::error_code ec, std::size_t) {
            if (id != attempt_) return;
            if (ec) { fail(id, ec.message()); return; }
            c->writeQueue.pop_front();
            if (!c->writeQueue.empty()) doWrite(id);
        });
}

void BeastWsTransport::schedulePing(uint64_t id) {
    pingTimer_.expires_after(std::chrono::seconds(25));
    pingTimer_.async_wait([this, id](beast::error_code ec) {
        if (ec || id != attempt_) return;
        conn_->ws.async_ping({}, [this, id, c = conn_](beast::error_code ec2) {
            if (id != attempt_) return;
            if (ec2) { fail(id, ec2.message()); return; }
            schedulePing(id);
        });
    });
}
