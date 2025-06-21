#include "coinbasestreamclient.h"
#include "tradedata.h"
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
using tcp = boost::asio::ip::tcp;

CoinbaseStreamClient::CoinbaseStreamClient(bool useSandbox)
    : m_ioc(),
      m_ssl_ctx(ssl::context::tlsv12_client),
      m_resolver(net::make_strand(m_ioc)),
      m_ws(net::make_strand(m_ioc), m_ssl_ctx),
      m_running(false) {

    if (useSandbox) {
        m_host = "ws-feed-public.sandbox.exchange.coinbase.com";
    } else {
        m_host = "ws-feed.exchange.coinbase.com";
    }
    m_port = "443";
    m_endpoint = "/";
}

CoinbaseStreamClient::~CoinbaseStreamClient() {
    if (m_running.exchange(false)) {
        net::post(m_ioc, [this]() { do_close(); });
        m_thread.join();
    }
}

void CoinbaseStreamClient::subscribe(const std::vector<std::string>& symbols) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_symbols = symbols;
    m_subscribe_message = buildSubscribeMessage();
    
    net::post(m_ws.get_executor(), [this]() {
        if (m_ws.is_open()) {
            m_ws.async_write(net::buffer(m_subscribe_message),
                beast::bind_front_handler(&CoinbaseStreamClient::on_write, this));
        }
    });
}

void CoinbaseStreamClient::start() {
    if (!m_running.exchange(true)) {
        m_thread = std::thread(&CoinbaseStreamClient::run, this);
    }
}

void CoinbaseStreamClient::run() {
    m_resolver.async_resolve(m_host, m_port,
        beast::bind_front_handler(&CoinbaseStreamClient::on_resolve, this));
    m_ioc.run();
}

void CoinbaseStreamClient::on_resolve(beast::error_code ec, tcp::resolver::results_type results) {
    if (ec) {
        std::cerr << "resolve: " << ec.message() << std::endl;
        return reconnect();
    }
    
    get_lowest_layer(m_ws).expires_after(std::chrono::seconds(30));
    beast::get_lowest_layer(m_ws).async_connect(results,
        beast::bind_front_handler(&CoinbaseStreamClient::on_connect, this));
}

void CoinbaseStreamClient::on_connect(beast::error_code ec, tcp::resolver::results_type::endpoint_type) {
    if (ec) {
        std::cerr << "connect: " << ec.message() << std::endl;
        return reconnect();
    }

    if (!SSL_set_tlsext_host_name(m_ws.next_layer().native_handle(), m_host.c_str())) {
        ec = beast::error_code(static_cast<int>(::ERR_get_error()), net::error::get_ssl_category());
        std::cerr << "SSL_set_tlsext_host_name: " << ec.message() << std::endl;
        return reconnect();
    }

    get_lowest_layer(m_ws).expires_after(std::chrono::seconds(30));
    m_ws.next_layer().async_handshake(ssl::stream_base::client,
        beast::bind_front_handler(&CoinbaseStreamClient::on_ssl_handshake, this));
}

void CoinbaseStreamClient::on_ssl_handshake(beast::error_code ec) {
    if (ec) {
        std::cerr << "ssl_handshake: " << ec.message() << std::endl;
        return reconnect();
    }
    
    get_lowest_layer(m_ws).expires_never();
    m_ws.set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));

    m_ws.async_handshake(m_host, m_endpoint,
        beast::bind_front_handler(&CoinbaseStreamClient::on_handshake, this));
}

void CoinbaseStreamClient::on_handshake(beast::error_code ec) {
    if (ec) {
        std::cerr << "handshake: " << ec.message() << std::endl;
        return reconnect();
    }
    
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_subscribe_message.empty()) {
        m_ws.async_write(net::buffer(m_subscribe_message),
            beast::bind_front_handler(&CoinbaseStreamClient::on_write, this));
    }

    m_ws.async_read(m_buffer,
        beast::bind_front_handler(&CoinbaseStreamClient::on_read, this));
}

void CoinbaseStreamClient::on_write(beast::error_code ec, std::size_t) {
    if (ec) {
        std::cerr << "write: " << ec.message() << std::endl;
        return reconnect();
    }
}

void CoinbaseStreamClient::on_read(beast::error_code ec, std::size_t) {
    // If the connection is closed, we need to reconnect
    if (ec) {
        std::cerr << "read: " << ec.message() << std::endl;
        return reconnect();
    }

    // Read the payload from the buffer
    auto payload = beast::buffers_to_string(m_buffer.data());
    m_buffer.consume(m_buffer.size());
    
    auto j = nlohmann::json::parse(payload, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return;

    if (j.value("type", "") == "match" || j.value("type", "") == "last_match") {
        Trade trade;
        trade.timestamp = parseTimestamp(j.value("time", ""));
        if (j.contains("trade_id")) {
             trade.trade_id = std::to_string(j["trade_id"].get<long long>());
        }
        auto side_str = j.value("side", "");
        if (side_str == "buy") trade.side = AggressorSide::Sell;
        else if (side_str == "sell") trade.side = AggressorSide::Buy;
        else trade.side = AggressorSide::Unknown;
        
        trade.price = std::stod(j.value("price", "0.0"));
        trade.size = std::stod(j.value("last_size", j.value("size", "0.0")));
        auto product = j.value("product_id", "");
        trade.product_id = product;

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto& buffer = m_tradeBuffers[product];
            if (buffer.size() >= m_maxTrades) {
                buffer.pop_front();
            }
            buffer.push_back(trade);
        }
        logTrade(product, j);
    }
    
    m_ws.async_read(m_buffer,
        beast::bind_front_handler(&CoinbaseStreamClient::on_read, this));
}

void CoinbaseStreamClient::do_close() {
    if (m_ws.is_open()) {
        m_ws.async_close(websocket::close_code::normal,
            beast::bind_front_handler(&CoinbaseStreamClient::on_close, this));
    }
}

void CoinbaseStreamClient::on_close(beast::error_code ec) {
    if (ec) {
        std::cerr << "close: " << ec.message() << std::endl;
    }
}

void CoinbaseStreamClient::reconnect() {
    if (m_running) {
        do_close();
        net::post(m_ioc, [this](){
            std::this_thread::sleep_for(std::chrono::seconds(1));
            m_resolver.async_resolve(m_host, m_port,
                beast::bind_front_handler(&CoinbaseStreamClient::on_resolve, this));
        });
    }
}

std::vector<Trade> CoinbaseStreamClient::getRecentTrades(const std::string& symbol) {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<Trade> trades;
    auto it = m_tradeBuffers.find(symbol);
    if (it != m_tradeBuffers.end()) {
        trades.assign(it->second.begin(), it->second.end());
    }
    return trades;
}

std::vector<Trade> CoinbaseStreamClient::getNewTrades(const std::string& symbol, const std::string& lastTradeId) {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<Trade> newTrades;
    auto it = m_tradeBuffers.find(symbol);
    if (it != m_tradeBuffers.end()) {
        bool foundLastTrade = lastTradeId.empty();
        for (const auto& trade : it->second) {
            if (foundLastTrade) {
                newTrades.push_back(trade);
            } else if (trade.trade_id == lastTradeId) {
                foundLastTrade = true;
            }
        }
    }
    return newTrades;
}

std::string CoinbaseStreamClient::buildSubscribeMessage() {
    nlohmann::json j;
    j["type"] = "subscribe";
    j["product_ids"] = m_symbols;
    j["channels"] = {"matches"};
    return j.dump();
}

std::chrono::system_clock::time_point CoinbaseStreamClient::parseTimestamp(const std::string& t) {
    if (t.empty()) {
        return std::chrono::system_clock::now();
    }
    std::tm tm{};
    std::istringstream ss(t);
    ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
    if (ss.fail()) {
        return std::chrono::system_clock::now();
    }
#ifdef _WIN32
    std::time_t tt = _mkgmtime(&tm);
#else
    std::time_t tt = timegm(&tm);
#endif
    auto tp = std::chrono::system_clock::from_time_t(tt);
    auto pos = t.find('.');
    if (pos != std::string::npos) {
        auto end = t.find('Z', pos);
        auto sub = t.substr(pos + 1, end - pos - 1);
        while (sub.size() < 6) sub.push_back('0');
        int micros = std::stoi(sub.substr(0,6));
        tp += std::chrono::microseconds(micros);
    }
    return tp;
}

void CoinbaseStreamClient::logTrade(const std::string& product, const nlohmann::json& j) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto& out = getLogStream(product);
    out << j.dump() << std::endl;
}

std::ofstream& CoinbaseStreamClient::getLogStream(const std::string& product) {
    auto it = m_logStreams.find(product);
    if (it == m_logStreams.end()) {
        std::filesystem::create_directories(m_logDir);
        auto path = m_logDir / (product + ".log");
        it = m_logStreams.emplace(std::piecewise_construct, std::forward_as_tuple(product), std::forward_as_tuple(path, std::ios::app)).first;
    }
    
    auto& out = it->second;
    if (out.tellp() >= static_cast<std::streampos>(m_maxLogSize)) {
        out.close();
        auto path = m_logDir / (product + ".log");
        auto rotated = path;
        rotated += ".1";
        if(std::filesystem::exists(rotated)) {
            std::filesystem::remove(rotated);
        }
        std::filesystem::rename(path, rotated);
        out.open(path, std::ios::app);
    }
    return out;
}



