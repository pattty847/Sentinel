#include "CoinbaseConnection.hpp"
#include <chrono>

CoinbaseConnection::CoinbaseConnection(const std::string& host,
                                       const std::string& port,
                                       const std::string& target)
    : m_host(host)
    , m_port(port)
    , m_target(target)
{
    m_sslCtx.set_default_verify_paths();
    m_sslCtx.set_verify_mode(ssl::verify_peer);
}

CoinbaseConnection::~CoinbaseConnection() {
    stop();
}

void CoinbaseConnection::start() {
    if (!m_running.exchange(true)) {
        m_thread = std::thread(&CoinbaseConnection::run, this);
    }
}

void CoinbaseConnection::stop() {
    if (m_running.exchange(false)) {
        if (m_ws.is_open()) {
            net::post(m_ioc, [this]() { doClose(); });
        }
        m_ioc.stop();
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }
}

void CoinbaseConnection::send(const std::string& message) {
    if (!m_running) return;
    net::post(m_ioc, [this, message]() {
        m_ws.async_write(net::buffer(message),
                         beast::bind_front_handler(&CoinbaseConnection::onWrite, this));
    });
}

void CoinbaseConnection::run() {
    m_resolver.async_resolve(m_host, m_port,
        beast::bind_front_handler(&CoinbaseConnection::onResolve, this));
    m_ioc.run();
}

void CoinbaseConnection::onResolve(beast::error_code ec, tcp::resolver::results_type results) {
    if (ec) {
        if (on_error) on_error(ec);
        return;
    }
    get_lowest_layer(m_ws).expires_after(std::chrono::seconds(30));
    get_lowest_layer(m_ws).async_connect(results,
        beast::bind_front_handler(&CoinbaseConnection::onConnect, this));
}

void CoinbaseConnection::onConnect(beast::error_code ec, tcp::resolver::results_type::endpoint_type) {
    if (ec) {
        if (on_error) on_error(ec);
        return;
    }
    if (!SSL_set_tlsext_host_name(m_ws.next_layer().native_handle(), m_host.c_str())) {
        beast::error_code ssl_ec(static_cast<int>(::ERR_get_error()), net::error::get_ssl_category());
        if (on_error) on_error(ssl_ec);
        return;
    }
    get_lowest_layer(m_ws).expires_after(std::chrono::seconds(30));
    m_ws.next_layer().async_handshake(ssl::stream_base::client,
        beast::bind_front_handler(&CoinbaseConnection::onSslHandshake, this));
}

void CoinbaseConnection::onSslHandshake(beast::error_code ec) {
    if (ec) {
        if (on_error) on_error(ec);
        return;
    }
    get_lowest_layer(m_ws).expires_never();
    m_ws.set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));
    m_ws.async_handshake(m_host, m_target,
        beast::bind_front_handler(&CoinbaseConnection::onWsHandshake, this));
}

void CoinbaseConnection::onWsHandshake(beast::error_code ec) {
    if (ec) {
        if (on_error) on_error(ec);
        return;
    }
    if (on_open) on_open();
    m_ws.async_read(m_buffer,
        beast::bind_front_handler(&CoinbaseConnection::onRead, this));
}

void CoinbaseConnection::onWrite(beast::error_code ec, std::size_t) {
    if (ec && on_error) {
        on_error(ec);
    }
}

void CoinbaseConnection::onRead(beast::error_code ec, std::size_t) {
    if (ec) {
        if (on_error) on_error(ec);
        return;
    }
    std::string payload = beast::buffers_to_string(m_buffer.data());
    m_buffer.consume(m_buffer.size());
    if (on_message) on_message(payload);
    if (m_running) {
        m_ws.async_read(m_buffer,
            beast::bind_front_handler(&CoinbaseConnection::onRead, this));
    }
}

void CoinbaseConnection::doClose() {
    if (m_ws.is_open()) {
        m_ws.async_close(websocket::close_code::normal,
            beast::bind_front_handler(&CoinbaseConnection::onClose, this));
    }
}

void CoinbaseConnection::onClose(beast::error_code ec) {
    if (on_error && ec) on_error(ec);
    if (on_close) on_close();
}

