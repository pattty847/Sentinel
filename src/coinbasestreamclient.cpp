#include "coinbasestreamclient.h"
#include "tradedata.h"
#include <websocketpp/config/asio_client.hpp>
#include <websocketpp/client.hpp>
#include <websocketpp/common/thread.hpp>
#include <boost/asio/ssl/error.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <iomanip>
#include <sstream>
#include <ctime>

using websocketpp::lib::placeholders::_1;
using websocketpp::lib::placeholders::_2;
using websocketpp::lib::bind;

// Constructor
CoinbaseStreamClient::CoinbaseStreamClient(bool useSandbox)
    : m_shouldReconnect(true) {
    if (useSandbox) {
        m_endpoint = "wss://ws-feed-public.sandbox.exchange.coinbase.com";
    } else {
        m_endpoint = "wss://ws-feed.exchange.coinbase.com";
    }
    m_client.clear_access_channels(websocketpp::log::alevel::all);
    m_client.init_asio();
    m_client.start_perpetual();
    m_client.set_open_handler(bind(&CoinbaseStreamClient::onOpen, this, _1));
    m_client.set_message_handler(bind(&CoinbaseStreamClient::onMessage, this, _1, _2));
    m_client.set_close_handler(bind(&CoinbaseStreamClient::onClose, this, _1));
    m_client.set_fail_handler(bind(&CoinbaseStreamClient::onFail, this, _1));

    m_client.set_tls_init_handler([](connection_hdl){
        return websocketpp::lib::make_shared<boost::asio::ssl::context>(boost::asio::ssl::context::tlsv12);
    });
}

CoinbaseStreamClient::~CoinbaseStreamClient() {
    m_shouldReconnect = false;
    m_client.stop_perpetual();
    if (m_thread.joinable()) {
        m_thread.join();
    }
    for (auto& [_, stream] : m_logStreams) {
        if (stream.is_open()) {
            stream.close();
        }
    }
}

void CoinbaseStreamClient::subscribe(const std::vector<std::string>& symbols) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_symbols = symbols;
    if (!m_hdl.expired()) {
        m_client.send(m_hdl, buildSubscribeMessage(), websocketpp::frame::opcode::text);
    }
}

void CoinbaseStreamClient::start() {
    m_thread = std::thread(&CoinbaseStreamClient::run, this);
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

void CoinbaseStreamClient::run() {
    while (m_shouldReconnect) {
        connect();
        m_client.run();
        if (m_shouldReconnect) {
            m_client.reset();
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
}

void CoinbaseStreamClient::connect() {
    websocketpp::lib::error_code ec;
    auto con = m_client.get_connection(m_endpoint, ec);
    if (ec) {
        return;
    }
    m_hdl = con->get_handle();
    m_client.connect(con);
}

std::string CoinbaseStreamClient::buildSubscribeMessage() const {
    nlohmann::json j;
    j["type"] = "subscribe";
    j["product_ids"] = m_symbols;
    j["channels"] = {"matches"};
    return j.dump();
}

void CoinbaseStreamClient::onOpen(connection_hdl hdl) {
    std::cout << "[Connection to Coinbase Stream opened]\n" << std::endl;
    m_client.send(hdl, buildSubscribeMessage(), websocketpp::frame::opcode::text);
}

void CoinbaseStreamClient::onMessage(connection_hdl, client::message_ptr msg) {
    auto payload = msg->get_payload();
    auto j = nlohmann::json::parse(payload, nullptr, false);
    if (!j.is_object()) {
        return;
    }

    // Only log non-matches here
    if (j.value("type", "") != "match") {
        std::cout << "[Non-match type: " << j.value("type", "") << "]" << std::endl;
        return;
    }

    // Real match processing
    Trade trade;
    trade.timestamp = parseTimestamp(j.value("time", ""));
    
    // trade_id is a number in Coinbase JSON, convert to string
    if (j.contains("trade_id")) {
        if (j["trade_id"].is_string()) {
            trade.trade_id = j["trade_id"].get<std::string>();
        } else if (j["trade_id"].is_number()) {
            trade.trade_id = std::to_string(j["trade_id"].get<uint64_t>());
        } else {
            trade.trade_id = "unknown";
        }
    } else {
        trade.trade_id = "unknown";
    }
    
    auto side_str = j.value("side", "");
    if (side_str == "buy") {
        trade.side = Side::Buy;
    } else if (side_str == "sell") {
        trade.side = Side::Sell;
    } else {
        trade.side = Side::Unknown;
    }
    trade.price = std::stod(j.value("price", "0"));
    trade.size = std::stod(j.value("size", "0"));
    auto product = j.value("product_id", "");

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto& buffer = m_tradeBuffers[product];
        if (buffer.size() >= m_maxTrades) {
            buffer.pop_front();
        }
        buffer.push_back(trade);
    }

    // Log the legit trade
    logTrade(product, j);
}

void CoinbaseStreamClient::onClose(connection_hdl) {
    std::cout << "[Connection to Coinbase Stream closed\n]" << std::endl;
    m_shouldReconnect = true;
    m_client.reset();
}

void CoinbaseStreamClient::onFail(connection_hdl) {
    std::cout << "[Connection to Coinbase Stream failed\n]" << std::endl;
    m_shouldReconnect = true;
    m_client.reset();
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
    if (out.tellp() >= static_cast<std::streampos>(m_maxLogSize)) {
        out.close();
        auto path = std::filesystem::path(m_logDir) / (product + ".log");
        auto rotated = path;
        rotated += ".1";
        std::filesystem::rename(path, rotated);
        out.open(path, std::ios::app);
    }
}

std::ofstream& CoinbaseStreamClient::getLogStream(const std::string& product) {
    auto& out = m_logStreams[product];
    if (!out.is_open()) {
        std::filesystem::create_directories(m_logDir);
        auto path = std::filesystem::path(m_logDir) / (product + ".log");
        out.open(path, std::ios::app);
    }
    return out;
}



