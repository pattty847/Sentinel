#include "SentinelStreamServer.hpp"
#include "SentinelLogging.hpp"
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/strand.hpp>
#include <algorithm>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <unordered_set>
#include <nlohmann/json.hpp>
#include "../marketdata/model/TradeData.h"
#include "Cpp20Utils.hpp"

namespace beast = boost::beast;         // from <boost/beast.hpp>
namespace http = beast::http;           // from <boost/beast/http.hpp>
namespace websocket = beast::websocket; // from <boost/beast/websocket.hpp>
namespace net = boost::asio;            // from <boost/asio.hpp>
using tcp = boost::asio::ip::tcp;       // from <boost/asio/ip/tcp.hpp>

// Echoes back all received WebSocket messages
class Session : public std::enable_shared_from_this<Session> {
    websocket::stream<beast::tcp_stream> ws_;
    beast::flat_buffer buffer_;
    ServerDataModel& model_;
    std::unordered_set<std::string> subscriptions_;
    std::vector<std::string> write_queue_;
    std::mutex queue_mutex_;
    
    // Connection handles for signal disconnection (if needed)
    QMetaObject::Connection tradeConn_;
    QMetaObject::Connection bookConn_;

public:
    // Take ownership of the socket
    explicit Session(tcp::socket&& socket, ServerDataModel& model)
        : ws_(std::move(socket))
        , model_(model)
    {
    }

    ~Session() {
        QObject::disconnect(tradeConn_);
        QObject::disconnect(bookConn_);
    }

    // Get on the correct executor
    void run() {
        // We need to be executing within a strand to perform async operations
        // on the I/O objects in this session.
        net::dispatch(ws_.get_executor(),
            beast::bind_front_handler(
                &Session::on_run,
                shared_from_this()));
    }

    // Start the asynchronous operation
    void on_run() {
        // Set suggested timeout settings for the websocket
        ws_.set_option(
            websocket::stream_base::timeout::suggested(
                beast::role_type::server));

        // Set a decorator to change the Server of the handshake
        ws_.set_option(websocket::stream_base::decorator(
            [](websocket::response_type& res) {
                res.set(http::field::server,
                    std::string(BOOST_BEAST_VERSION_STRING) +
                        " sentinel-server");
            }));

        // Accept the websocket handshake
        ws_.async_accept(
            beast::bind_front_handler(
                &Session::on_accept,
                shared_from_this()));
    }

    void on_accept(beast::error_code ec) {
        if(ec)
            return fail(ec, "accept");

        sLog_App("Session: Client connected");
        
        auto self = shared_from_this();
        
        tradeConn_ = QObject::connect(&model_, &ServerDataModel::tradeBroadcast, 
            [self](const Trade& trade) {
                self->on_trade(trade);
            });
            
        bookConn_ = QObject::connect(&model_, &ServerDataModel::bookUpdateBroadcast,
            [self](const QString& productId, const std::vector<BookDelta>& deltas) {
                self->on_book_update(productId, deltas);
            });
            
        do_read();
    }

    void do_read() {
        // Read a message into our buffer
        ws_.async_read(
            buffer_,
            beast::bind_front_handler(
                &Session::on_read,
                shared_from_this()));
    }

    void on_read(beast::error_code ec, std::size_t bytes_transferred) {
        boost::ignore_unused(bytes_transferred);

        // This indicates that the session was closed
        if(ec == websocket::error::closed)
            return;

        if(ec)
            return fail(ec, "read");

        // Handle message
        std::string msg = beast::buffers_to_string(buffer_.data());
        handle_message(msg);

        // Clear the buffer
        buffer_.consume(buffer_.size());

        // Do another read
        do_read();
    }

    void handle_message(const std::string& msg) {
        try {
            auto j = nlohmann::json::parse(msg);
            std::string type = j.value("type", "");
            
            if (type == "subscribe") {
                std::string symbol = j.value("symbol", "");
                if (!symbol.empty()) {
                    sLog_App("Client subscribed to: " << QString::fromStdString(symbol));
                    subscriptions_.insert(symbol);
                    
                    // Send ACK
                    nlohmann::json ack;
                    ack["type"] = "ack";
                    ack["symbol"] = symbol;
                    do_write(ack.dump());

                    // Send Initial Snapshot
                    auto& hotData = model_.ensureSymbol(symbol);
                    // Use a helper to serialize the snapshot
                    // We need to capture the dense book state
                    
                    // Capture dense view
                    // auto view = hotData.liveBook.captureDenseSnapshot();
                    // Or simpler: iterate non-zero levels
                    // We can use getBids/getAsks which are dense vectors, but they are raw.
                    // We need price/quantity pairs for the client (JSON format).
                    // This is heavy for JSON, but okay for MVP.
                    
                    nlohmann::json snapshot;
                    snapshot["type"] = "snapshot";
                    snapshot["symbol"] = symbol;
                    
                    std::vector<nlohmann::json> bidsJson;
                    const auto& bids = hotData.liveBook.getBids();
                    for (size_t i = 0; i < bids.size(); ++i) {
                        if (bids[i] > 0) {
                             bidsJson.push_back({
                                 {"p", hotData.liveBook.index_to_price(i)},
                                 {"q", bids[i]}
                             });
                        }
                    }
                    snapshot["bids"] = bidsJson;

                    std::vector<nlohmann::json> asksJson;
                    const auto& asks = hotData.liveBook.getAsks();
                    for (size_t i = 0; i < asks.size(); ++i) {
                        if (asks[i] > 0) {
                             asksJson.push_back({
                                 {"p", hotData.liveBook.index_to_price(i)},
                                 {"q", asks[i]}
                             });
                        }
                    }
                    snapshot["asks"] = asksJson;
                    
                    do_write(snapshot.dump());
                    
                }
            } else if (type == "unsubscribe") {
                 std::string symbol = j.value("symbol", "");
                 subscriptions_.erase(symbol);
            }
        } catch (const std::exception& e) {
            sLog_Error("Server message parse error: " << e.what());
        }
    }
    
    // ------------------------------------------------------------------------
    // DATA HANDLERS (Called from ServerDataModel thread)
    // ------------------------------------------------------------------------
    
    void on_trade(const Trade& trade) {
        if (subscriptions_.find(trade.product_id) == subscriptions_.end()) return;
        
        nlohmann::json j;
        j["type"] = "trade";
        j["product_id"] = trade.product_id;
        j["price"] = trade.price;
        j["size"] = trade.size;
        j["side"] = (trade.side == AggressorSide::Buy) ? "buy" : "sell";
        j["time"] = Cpp20Utils::formatExchangeTimestamp(trade.timestamp);
        
        do_write(j.dump());
    }
    
    void on_book_update(const QString& productId, const std::vector<BookDelta>& deltas) {
        std::string pid = productId.toStdString();
        if (subscriptions_.find(pid) == subscriptions_.end()) return;
        
        // Retrieve price conversion helper from the model's SymbolHotData
        auto& symbolData = model_.ensureSymbol(pid);
        const auto& book = symbolData.liveBook;

        nlohmann::json j;
        j["type"] = "l2update";
        j["product_id"] = pid;
        
        std::vector<nlohmann::json> deltaJson;
        deltaJson.reserve(deltas.size());
        for (const auto& d : deltas) {
            deltaJson.push_back({
                {"side", d.isBid ? "bid" : "ask"},
                {"price", book.index_to_price(d.idx)},
                {"size", d.qty}
            });
        }
        j["deltas"] = deltaJson;
        
        do_write(j.dump());
    }

    // Thread-safe write
    void do_write(std::string payload) {
        // Post to the strand to ensure serialization
        net::post(ws_.get_executor(),
            beast::bind_front_handler(
                &Session::on_write_post,
                shared_from_this(),
                std::move(payload)));
    }
    
    void on_write_post(std::string payload) {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        write_queue_.push_back(std::move(payload));
        
        if (write_queue_.size() > 1) {
            // Write already in progress
            return;
        }
        
        internal_async_write();
    }
    
    void internal_async_write() {
        // Assumes queue is not empty and we are on the strand
        ws_.async_write(
            net::buffer(write_queue_.front()),
            beast::bind_front_handler(
                &Session::on_write_complete,
                shared_from_this()));
    }
    
    void on_write_complete(beast::error_code ec, std::size_t) {
        if (ec) return fail(ec, "write");
        
        std::lock_guard<std::mutex> lock(queue_mutex_);
        write_queue_.erase(write_queue_.begin());
        
        if (!write_queue_.empty()) {
            internal_async_write();
        }
    }

    void fail(beast::error_code ec, char const* what) {
        // Don't log "End of file" or "Connection reset" as errors during shutdown
        if (ec != websocket::error::closed && ec != net::error::operation_aborted) {
             sLog_Error("Session error: " << what << ": " << ec.message().c_str());
        }
    }
};

// ============================================================================

SentinelStreamServer::SentinelStreamServer(ServerDataModel& model, int port, QObject* parent)
    : QObject(parent)
    , m_model(model)
    , m_port(port)
{
}

SentinelStreamServer::~SentinelStreamServer() {
    stop();
}

void SentinelStreamServer::start() {
    if (m_running) return;
    
    try {
        m_running = true;
        
        // Setup acceptor
        tcp::endpoint endpoint(tcp::v4(), m_port);
        m_acceptor = std::make_unique<tcp::acceptor>(m_ioc);
        m_acceptor->open(endpoint.protocol());
        m_acceptor->set_option(net::socket_base::reuse_address(true));
        m_acceptor->bind(endpoint);
        m_acceptor->listen();
        
        sLog_App("SentinelStreamServer: Listening on port " << m_port);
        
        doAccept();
        
        m_thread = std::thread([this] {
            while (m_running) {
                try {
                    m_ioc.run();
                } catch (const std::exception& e) {
                    sLog_Error("SentinelStreamServer I/O error: " << e.what());
                    m_ioc.restart();
                }
            }
        });
        
    } catch (const std::exception& e) {
        sLog_Error("SentinelStreamServer start failed: " << e.what());
        m_running = false;
    }
}

void SentinelStreamServer::stop() {
    m_running = false;
    if (m_acceptor) {
        m_acceptor->close();
    }
    m_ioc.stop();
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void SentinelStreamServer::doAccept() {
    m_acceptor->async_accept(
        net::make_strand(m_ioc),
        [this](beast::error_code ec, tcp::socket socket) {
            if (!ec) {
                std::make_shared<Session>(std::move(socket), m_model)->run();
            } else {
                sLog_Error("Accept error: " << ec.message().c_str());
            }
            
            // Accept the next connection
            if (m_running) {
                doAccept();
            }
        });
}
