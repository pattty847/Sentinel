#include "SentinelStreamServer.hpp"
#include "HeatmapSlice.hpp"
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
#include <QByteArray>
#include "../marketdata/auth/Authenticator.hpp"
#include "../marketdata/rest/CoinbaseRestClient.hpp"
#include "../marketdata/model/TradeData.h"
#include "Cpp20Utils.hpp"

namespace beast = boost::beast;         // from <boost/beast.hpp>
namespace http = beast::http;           // from <boost/beast/http.hpp>
namespace websocket = beast::websocket; // from <boost/beast/websocket.hpp>
namespace net = boost::asio;            // from <boost/asio.hpp>
using tcp = boost::asio::ip::tcp;       // from <boost/asio/ip/tcp.hpp>

namespace {

nlohmann::json buildServerConfigPayload(const ServerConfig& cfg) {
    nlohmann::json payload;
    payload["type"] = "server_config";
    payload["schema_version"] = 1;
    payload["timeframes_ms"] = cfg.heatmap.timeframesMs;
    payload["heatmap"] = {
        {"grid_width", cfg.heatmap.gridWidth},
        {"grid_height", cfg.heatmap.gridHeight},
        {"tick_size", cfg.heatmap.tickSize},
        {"recenter_delta", cfg.heatmap.recenterDelta},
        {"band_fast", cfg.heatmap.bandFast},
        {"band_medium", cfg.heatmap.bandMedium},
        {"band_slow", cfg.heatmap.bandSlow}
    };
    if (cfg.heatmap.activeTimeframeMs > 0) {
        payload["heatmap"]["active_timeframe_ms"] = cfg.heatmap.activeTimeframeMs;
    }
    payload["orderbook"] = {
        {"tick_size", cfg.orderbook.tickSize},
        {"band_pct", cfg.orderbook.bandPct}
    };
    payload["candles"] = {
        {"update_bps_fast", cfg.candles.bpsFast},
        {"update_bps_slow", cfg.candles.bpsSlow},
        {"update_tick_mult_fast", cfg.candles.tickMultFast},
        {"update_tick_mult_slow", cfg.candles.tickMultSlow},
        {"update_silence_ms_fast", cfg.candles.silenceMsFast},
        {"update_silence_ms_slow", cfg.candles.silenceMsSlow},
        {"update_volume_fast", cfg.candles.volumeFast},
        {"update_volume_slow", cfg.candles.volumeSlow},
        {"update_tick_size", cfg.candles.tickSize}
    };
    payload["default_symbols"] = cfg.defaultSymbols;
    return payload;
}
}

class Session : public std::enable_shared_from_this<Session> {
    websocket::stream<beast::tcp_stream> ws_;
    beast::flat_buffer buffer_;
    ServerDataModel& model_;
    SentinelStreamServer* owner_ = nullptr;
    std::unordered_set<std::string> subscriptions_;
    std::vector<std::string> write_queue_;
    std::mutex queue_mutex_;
    
    QMetaObject::Connection tradeConn_;
    QMetaObject::Connection bookConn_;
    QMetaObject::Connection heatmapConn_;
    QMetaObject::Connection barUpdatedConn_;
    QMetaObject::Connection barClosedConn_;

    struct CandleStreamState {
        int64_t seq = 0;
        OHLCVBar lastBar;
        int64_t lastSentMs = 0;
        bool hasLast = false;
    };
    std::unordered_map<std::string, CandleStreamState> candleStates_;
    std::mutex candle_mutex_;

public:
    explicit Session(tcp::socket&& socket, ServerDataModel& model, SentinelStreamServer* owner)
        : ws_(std::move(socket))
        , model_(model)
        , owner_(owner)
    {
    }

    ~Session() {
        QObject::disconnect(tradeConn_);
        QObject::disconnect(bookConn_);
        QObject::disconnect(heatmapConn_);
        QObject::disconnect(barUpdatedConn_);
        QObject::disconnect(barClosedConn_);
    }

    void run() {
        net::dispatch(ws_.get_executor(),
            beast::bind_front_handler(
                &Session::on_run,
                shared_from_this()));
    }

    void on_run() {
        ws_.set_option(
            websocket::stream_base::timeout::suggested(
                beast::role_type::server));

        ws_.set_option(websocket::stream_base::decorator(
            [](websocket::response_type& res) {
                res.set(http::field::server,
                    std::string(BOOST_BEAST_VERSION_STRING) +
                        " sentinel-server");
            }));

        ws_.async_accept(
            beast::bind_front_handler(
                &Session::on_accept,
                shared_from_this()));
    }

    void on_accept(beast::error_code ec) {
        if(ec)
            return fail(ec, "accept");

        sLog_App("Sentinel client connected");
        auto self = shared_from_this();

        if (owner_) {
            auto configPayload = buildServerConfigPayload(owner_->serverConfig());
            do_write(configPayload.dump());
        }
        
        tradeConn_ = QObject::connect(&model_, &ServerDataModel::tradeBroadcast, 
            [self](const Trade& trade) {
                self->on_trade(trade);
            });
            
        bookConn_ = QObject::connect(&model_, &ServerDataModel::bookUpdateBroadcast,
            [self](const QString& productId, const std::vector<BookDelta>& deltas) {
                self->on_book_update(productId, deltas);
            });

        heatmapConn_ = QObject::connect(&model_, &ServerDataModel::heatmapSliceReady,
            [self](const HeatmapSlice& slice) {
                self->on_heatmap_slice(slice);
            });

        barUpdatedConn_ = QObject::connect(&model_, &ServerDataModel::barUpdated,
            [self](const QString& symbol, Timeframe tf, const OHLCVBar& bar) {
                self->on_bar_updated(symbol, tf, bar);
            });

        barClosedConn_ = QObject::connect(&model_, &ServerDataModel::barClosed,
            [self](const QString& symbol, Timeframe tf, const OHLCVBar& bar) {
                self->on_bar_closed(symbol, tf, bar);
            });
            
        do_read();
    }

    void do_read() {
        ws_.async_read(
            buffer_,
            beast::bind_front_handler(
                &Session::on_read,
                shared_from_this()));
    }

    void on_read(beast::error_code ec, std::size_t bytes_transferred) {
        boost::ignore_unused(bytes_transferred);

        if(ec == websocket::error::closed) {
            sLog_App("Sentinel client disconnected");
            return;
        }

        if(ec)
            return fail(ec, "read");

        std::string msg = beast::buffers_to_string(buffer_.data());
        handle_message(msg);
        buffer_.consume(buffer_.size());
        do_read();
    }

    void handle_message(const std::string& msg) {
        try {
            auto j = nlohmann::json::parse(msg);
            std::string type = j.value("type", "");
            
            if (type == "subscribe") {
                std::string symbol = j.value("symbol", "");
                if (!symbol.empty()) {
                    subscriptions_.insert(symbol);
                    if (owner_) {
                        owner_->notifyClientSubscribed(symbol);
                    }
                    
                    nlohmann::json ack;
                    ack["type"] = "ack";
                    ack["symbol"] = symbol;
                    do_write(ack.dump());

                    auto& hotData = model_.ensureSymbol(symbol);
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
            } else if (type == "heatmap_history_request") {
                std::string symbol = j.value("symbol", "");
                const int64_t timeframeMs = j.value("timeframe_ms", static_cast<int64_t>(0));
                const int64_t endTimeMs = j.value("end_time", static_cast<int64_t>(0));
                const int count = j.value("count", 0);
                if (!symbol.empty() && timeframeMs > 0 && count > 0) {
                    std::vector<HeatmapTwapStreamer::HistoryColumn> columns;
                    int gridWidth = 0;
                    int gridHeight = 0;
                    const bool ok = model_.getHeatmapHistory(symbol, timeframeMs, endTimeMs, count,
                                                            gridWidth, gridHeight, columns);
                    nlohmann::json payload;
                    payload["type"] = "heatmap_history_chunk";
                    payload["symbol"] = symbol;
                    payload["timeframe_ms"] = timeframeMs;
                    payload["grid_width"] = gridWidth;
                    payload["grid_height"] = gridHeight;
                    payload["format"] = "u16";
                    payload["encoding"] = "base64";
                    payload["liquidity_format"] = "u16";
                    payload["liquidity_encoding"] = "base64";
                    auto arr = nlohmann::json::array();
                    if (ok) {
                        for (const auto& col : columns) {
                            nlohmann::json item;
                            item["time_start"] = col.bucketStartMs;
                            item["time_end"] = col.bucketEndMs;
                            item["min_price"] = col.minPrice;
                            item["max_price"] = col.maxPrice;
                            item["tick_size"] = col.tickSize;
                            item["column"] = col.intensity.toBase64().toStdString();
                            if (!col.liquidity.isEmpty()) {
                                item["liquidity_column"] = col.liquidity.toBase64().toStdString();
                                item["liquidity_scale"] = col.liquidityScale;
                            }
                            arr.push_back(std::move(item));
                        }
                    }
                    payload["columns"] = std::move(arr);
                    do_write(payload.dump());
                }
            } else if (type == "candle_history_request") {
                std::string symbol = j.value("symbol", "");
                const int64_t timeframeSec = j.value("timeframe_sec", static_cast<int64_t>(0));
                int64_t endTimeSec = j.value("end_time_sec", static_cast<int64_t>(0));
                int limit = j.value("limit", 350);

                if (symbol.empty() || timeframeSec <= 0) {
                    send_error("candle_history_request", symbol, "missing symbol or timeframe_sec");
                    return;
                }

                if (limit <= 0) {
                    limit = 350;
                }
                if (limit > 350) {
                    limit = 350;
                }

                if (endTimeSec <= 0) {
                    endTimeSec = static_cast<int64_t>(
                        std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count());
                }

                if (timeframeSec == 1) {
                    const int64_t endMs = endTimeSec * 1000;
                    if (limit > 10000) {
                        limit = 10000;
                    }
                    const auto history = model_.getHistory(symbol, Timeframe::OneSecond, static_cast<size_t>(limit));
                    std::vector<OHLCVBar> filtered;
                    filtered.reserve(history.size());
                    for (const auto& bar : history) {
                        if (bar.timestamp_ms <= endMs) {
                            filtered.push_back(bar);
                        }
                    }

                    const int64_t tfMs = 1000;
                    const int64_t nowMs = static_cast<int64_t>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count());

                    int64_t startTimeSec = endTimeSec - (timeframeSec * static_cast<int64_t>(limit));
                    if (startTimeSec < 0) {
                        startTimeSec = 0;
                    }

                    nlohmann::json payload;
                    payload["type"] = "candle_history_chunk";
                    payload["symbol"] = symbol;
                    payload["timeframe_sec"] = timeframeSec;
                    payload["start_time_sec"] = startTimeSec;
                    payload["end_time_sec"] = endTimeSec;
                    auto arr = nlohmann::json::array();
                    for (const auto& bar : filtered) {
                        const bool isClosed = (bar.timestamp_ms + tfMs) <= nowMs;
                        nlohmann::json item;
                        item["time_start_ms"] = bar.timestamp_ms;
                        item["time_end_ms"] = bar.timestamp_ms + tfMs;
                        item["open"] = bar.open;
                        item["high"] = bar.high;
                        item["low"] = bar.low;
                        item["close"] = bar.close;
                        item["volume"] = bar.volume;
                        item["is_closed"] = isClosed;
                        arr.push_back(std::move(item));
                    }
                    payload["candles"] = std::move(arr);
                    do_write(payload.dump());
                    return;
                }

                const auto granularity = CoinbaseRestClient::granularityFromSeconds(timeframeSec);
                if (!granularity) {
                    send_error("candle_history_request", symbol, "unsupported timeframe_sec");
                    return;
                }

                const int64_t startTimeSec = endTimeSec - (timeframeSec * static_cast<int64_t>(limit));
                if (startTimeSec <= 0) {
                    send_error("candle_history_request", symbol, "invalid start time");
                    return;
                }

                auto self = shared_from_this();
                std::thread([self, symbol, timeframeSec, endTimeSec, startTimeSec, limit, granularity]() {
                    CandleFetchResult res = self->owner_->restClient().fetchProductCandles(
                        symbol, startTimeSec, endTimeSec, *granularity, limit);

                    if (!res.ok) {
                        self->send_error("candle_history_request", symbol,
                                         std::string("fetch failed: ") + res.error);
                        return;
                    }

                    std::sort(res.candles.begin(), res.candles.end(),
                              [](const OHLCVBar& a, const OHLCVBar& b) {
                                  return a.timestamp_ms < b.timestamp_ms;
                              });

                    const int64_t tfMs = timeframeSec * 1000;
                    const int64_t nowMs = static_cast<int64_t>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count());

                    nlohmann::json payload;
                    payload["type"] = "candle_history_chunk";
                    payload["symbol"] = symbol;
                    payload["timeframe_sec"] = timeframeSec;
                    payload["start_time_sec"] = startTimeSec;
                    payload["end_time_sec"] = endTimeSec;
                    auto arr = nlohmann::json::array();
                    for (auto& bar : res.candles) {
                        bar.is_closed = (bar.timestamp_ms + tfMs) <= nowMs;
                        nlohmann::json item;
                        item["time_start_ms"] = bar.timestamp_ms;
                        item["time_end_ms"] = bar.timestamp_ms + tfMs;
                        item["open"] = bar.open;
                        item["high"] = bar.high;
                        item["low"] = bar.low;
                        item["close"] = bar.close;
                        item["volume"] = bar.volume;
                        item["is_closed"] = bar.is_closed;
                        arr.push_back(std::move(item));
                    }
                    payload["candles"] = std::move(arr);

                    self->do_write(payload.dump());
                }).detach();
            } else if (type == "unsubscribe") {
                 std::string symbol = j.value("symbol", "");
                 subscriptions_.erase(symbol);
                 if (!symbol.empty() && owner_) {
                     owner_->notifyClientUnsubscribed(symbol);
                 }
            }
        } catch (const std::exception& e) {
            sLog_Error("Server message parse error: " << e.what());
        }
    }
    
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

    void on_heatmap_slice(const HeatmapSlice& slice) {
        const std::string sym = slice.symbol.toStdString();
        if (subscriptions_.find(sym) == subscriptions_.end()) return;

        nlohmann::json j;
        j["type"] = "heatmap_slice";
        j["symbol"] = sym;
        j["time_start"] = slice.bucketStartMs;
        j["time_end"] = slice.bucketEndMs;
        j["timeframe_ms"] = slice.timeframeMs;
        j["grid_width"] = slice.gridWidth;
        j["grid_height"] = slice.gridHeight;
        j["min_price"] = slice.minPrice;
        j["max_price"] = slice.maxPrice;
        j["tick_size"] = slice.tickSize;
        j["mid_price"] = slice.midPrice;
        j["last_trade"] = slice.lastTrade;
        j["reset"] = slice.reset;
        j["format"] = slice.format.toStdString();
        j["encoding"] = "base64";
        j["column"] = slice.column.toBase64().toStdString();
        if (!slice.liquidityColumn.isEmpty()) {
            j["liquidity_format"] = "u16";
            j["liquidity_encoding"] = "base64";
            j["liquidity_scale"] = slice.liquidityScale;
            j["liquidity_column"] = slice.liquidityColumn.toBase64().toStdString();
        }

        do_write(j.dump());
    }

    static bool should_emit_update(const OHLCVBar& bar,
                                   const CandleStreamState& state,
                                   const ServerCandleGateConfig& cfg,
                                   const ServerOrderBookConfig& obConfig,
                                   int64_t tfSec,
                                   int64_t nowMs) {
        if (!state.hasLast) {
            return true;
        }

        const OHLCVBar& last = state.lastBar;
        const bool highChanged = bar.high > last.high;
        const bool lowChanged = bar.low < last.low;

        const double closeDelta = std::abs(bar.close - last.close);
        const double tickSize = (cfg.tickSize > 0.0) ? cfg.tickSize : obConfig.tickSize;
        const int tickMultiplier = (tfSec <= 1) ? cfg.tickMultFast : cfg.tickMultSlow;
        const double bpsThreshold = (tfSec <= 1) ? cfg.bpsFast : cfg.bpsSlow;
        const double priceThreshold = std::max(tickSize * tickMultiplier,
                                               bar.close * bpsThreshold);
        const bool closeMoved = closeDelta >= priceThreshold;

        const double volumeDelta = bar.volume - last.volume;
        const double volumeThreshold = (tfSec <= 1) ? cfg.volumeFast : cfg.volumeSlow;
        const bool volumeMoved = volumeThreshold > 0.0 && volumeDelta >= volumeThreshold;

        const int64_t maxSilenceMs = (tfSec <= 1) ? cfg.silenceMsFast : cfg.silenceMsSlow;
        const bool silence = (nowMs - state.lastSentMs) >= maxSilenceMs;

        return highChanged || lowChanged || closeMoved || volumeMoved || silence;
    }

    void on_bar_updated(const QString& symbol, Timeframe tf, const OHLCVBar& bar) {
        const std::string sym = symbol.toStdString();
        if (subscriptions_.find(sym) == subscriptions_.end()) return;

        const int64_t tfSec = static_cast<int64_t>(tf);
        const std::string key = sym + "|" + std::to_string(tfSec);
        const int64_t nowMs = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        const bool logBars = qEnvironmentVariableIsSet("SENTINEL_CANDLE_BAR_LOG");

        CandleStreamState state;
        {
            std::lock_guard<std::mutex> lock(candle_mutex_);
            auto& entry = candleStates_[key];
            if (!should_emit_update(bar, entry, owner_->serverConfig().candles,
                                    owner_->serverConfig().orderbook, tfSec, nowMs)) {
                return;
            }
            entry.seq++;
            entry.lastBar = bar;
            entry.lastSentMs = nowMs;
            entry.hasLast = true;
            state = entry;
        }
        if (logBars) {
            sLog_App("Candle bar update: symbol=" << symbol
                                                  << " tfSec=" << tfSec
                                                  << " start=" << bar.timestamp_ms
                                                  << " end=" << (bar.timestamp_ms + tfSec * 1000)
                                                  << " now=" << nowMs
                                                  << " closed=" << (bar.is_closed ? "true" : "false"));
        }

        nlohmann::json item;
        item["time_start_ms"] = bar.timestamp_ms;
        item["time_end_ms"] = bar.timestamp_ms + tfSec * 1000;
        item["open"] = bar.open;
        item["high"] = bar.high;
        item["low"] = bar.low;
        item["close"] = bar.close;
        item["volume"] = bar.volume;
        item["is_closed"] = bar.is_closed;

        nlohmann::json payload;
        payload["type"] = "candle_bar_update";
        payload["symbol"] = sym;
        payload["timeframe_sec"] = tfSec;
        payload["bucket_start_ms"] = bar.timestamp_ms;
        payload["seq"] = state.seq;
        payload["candle"] = std::move(item);

        do_write(payload.dump());
    }

    void on_bar_closed(const QString& symbol, Timeframe tf, const OHLCVBar& bar) {
        const std::string sym = symbol.toStdString();
        if (subscriptions_.find(sym) == subscriptions_.end()) return;

        const int64_t tfSec = static_cast<int64_t>(tf);
        const std::string key = sym + "|" + std::to_string(tfSec);
        CandleStreamState state;
        const bool logBars = qEnvironmentVariableIsSet("SENTINEL_CANDLE_BAR_LOG");
        {
            std::lock_guard<std::mutex> lock(candle_mutex_);
            auto& entry = candleStates_[key];
            entry.seq++;
            entry.lastBar = bar;
            entry.lastSentMs = static_cast<int64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());
            entry.hasLast = true;
            state = entry;
        }
        if (logBars) {
            const int64_t nowMs = state.lastSentMs;
            sLog_App("Candle bar closed: symbol=" << symbol
                                                  << " tfSec=" << tfSec
                                                  << " start=" << bar.timestamp_ms
                                                  << " end=" << (bar.timestamp_ms + tfSec * 1000)
                                                  << " now=" << nowMs);
        }

        nlohmann::json item;
        item["time_start_ms"] = bar.timestamp_ms;
        item["time_end_ms"] = bar.timestamp_ms + tfSec * 1000;
        item["open"] = bar.open;
        item["high"] = bar.high;
        item["low"] = bar.low;
        item["close"] = bar.close;
        item["volume"] = bar.volume;
        item["is_closed"] = true;

        nlohmann::json payload;
        payload["type"] = "candle_bar_closed";
        payload["symbol"] = sym;
        payload["timeframe_sec"] = tfSec;
        payload["bucket_start_ms"] = bar.timestamp_ms;
        payload["seq"] = state.seq;
        payload["candle"] = std::move(item);

        do_write(payload.dump());
    }

    void do_write(std::string payload) {
        net::post(ws_.get_executor(),
            beast::bind_front_handler(
                &Session::on_write_post,
                shared_from_this(),
                std::move(payload)));
    }

    void send_error(const std::string& context, const std::string& symbol, const std::string& message) {
        nlohmann::json err;
        err["type"] = "error";
        err["context"] = context;
        if (!symbol.empty()) {
            err["symbol"] = symbol;
        }
        err["message"] = message;
        do_write(err.dump());
    }
    
    void on_write_post(std::string payload) {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        write_queue_.push_back(std::move(payload));
        
        if (write_queue_.size() > 1) {
            return;
        }
        
        internal_async_write();
    }
    
    void internal_async_write() {
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
        if (ec != websocket::error::closed && ec != net::error::operation_aborted) {
             sLog_Error("Session error: " << what << ": " << ec.message().c_str());
        }
    }
};

// ============================================================================

SentinelStreamServer::SentinelStreamServer(ServerDataModel& model,
                                           Authenticator& auth,
                                           const ServerConfig& config,
                                           int port,
                                           QObject* parent)
    : QObject(parent)
    , m_model(model)
    , m_restClient(std::make_unique<CoinbaseRestClient>(auth))
    , m_serverConfig(config)
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
        
        tcp::endpoint endpoint(tcp::v4(), m_port);
        m_acceptor = std::make_unique<tcp::acceptor>(m_ioc);
        m_acceptor->open(endpoint.protocol());
        m_acceptor->set_option(net::socket_base::reuse_address(true));
        m_acceptor->bind(endpoint);
        m_acceptor->listen();

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
                std::make_shared<Session>(std::move(socket), m_model, this)->run();
            } else {
                sLog_Error("Accept error: " << ec.message().c_str());
            }
            if (m_running) {
                doAccept();
            }
        });
}

void SentinelStreamServer::notifyClientSubscribed(const std::string& symbol) {
    emit clientSubscribed(QString::fromStdString(symbol));
}

void SentinelStreamServer::notifyClientUnsubscribed(const std::string& symbol) {
    emit clientUnsubscribed(QString::fromStdString(symbol));
}

CoinbaseRestClient& SentinelStreamServer::restClient() {
    return *m_restClient;
}
