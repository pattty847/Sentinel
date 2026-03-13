#include "SentinelStreamServer.hpp"
#include "HeatmapSlice.hpp"
#include "SentinelStreamProtocol.hpp"
#include "SentinelLogging.hpp"
#include "../servermodel/SessionManager.hpp"
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/strand.hpp>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <nlohmann/json.hpp>
#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QtEndian>
#include <cstdio>
#include "../marketdata/auth/Authenticator.hpp"
#include "../marketdata/rest/CoinbaseRestClient.hpp"
#include "../marketdata/model/TradeData.h"
#include "../trading/TradingEngine.hpp"
#include "Cpp20Utils.hpp"

namespace beast = boost::beast;         // from <boost/beast.hpp>
namespace http = beast::http;           // from <boost/beast/http.hpp>
namespace websocket = beast::websocket; // from <boost/beast/websocket.hpp>
namespace net = boost::asio;            // from <boost/asio.hpp>
using tcp = boost::asio::ip::tcp;       // from <boost/asio/ip/tcp.hpp>

namespace {

int64_t tradeTimestampMs(const Trade& trade) {
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(trade.timestamp.time_since_epoch()).count());
}

nlohmann::json buildServerConfigPayload(const ServerConfig& cfg) {
    nlohmann::json payload;
    payload["type"] = "server_config";
    payload["schema_version"] = protocol::SentinelProtocol::kServerConfigSchemaVersion;
    payload["timeframes_ms"] = cfg.heatmap.timeframesMs;
    payload["heatmap"] = {
        {"grid_width", cfg.heatmap.gridWidth},
        {"grid_height", cfg.heatmap.gridHeight},
        {"tick_size", cfg.heatmap.tickSize},
        {"recenter_delta", cfg.heatmap.recenterDelta},
        {"band_fast", cfg.heatmap.bandFast},
        {"band_medium", cfg.heatmap.bandMedium},
        {"band_slow", cfg.heatmap.bandSlow},
        {"intensity_mode", cfg.heatmap.intensityMode},
        {"intensity_max_mode", cfg.heatmap.intensityMaxMode},
        {"intensity_max_decay", cfg.heatmap.intensityMaxDecay},
        {"intensity_log_scale", cfg.heatmap.intensityLogScale},
        {"intensity_power", cfg.heatmap.intensityPower},
        {"intensity_floor", cfg.heatmap.intensityFloor},
        {"debug_slice_log", cfg.heatmap.debugSliceLog}
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
    websocket::stream<beast::ssl_stream<beast::tcp_stream>> ws_;
    beast::flat_buffer buffer_;
    ServerDataModel& model_;
    SentinelStreamServer* owner_ = nullptr;
    std::unordered_set<std::string> subscriptions_;
    std::vector<std::string> write_queue_;
    std::mutex queue_mutex_;
    QByteArray footprintDeltaScratch_;
    std::vector<double> footprintRowDeltaScratch_;
    
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
    int64_t tpoBucketMs_ = 900'000;
    SessionManager::SessionType tpoSessionType_ = SessionManager::SessionType::H24;
    int64_t tpoSessionMs_ = SessionManager::sessionDurationMs(SessionManager::SessionType::H24);
    uint64_t m_latencySenderId = 0;

    bool buildFootprintDeltaColumn(const HeatmapSlice& slice, QByteArray& out, double& outQuantScale) {
        if (slice.gridHeight <= 0 || slice.tickSize <= 0.0 || slice.maxPrice <= slice.minPrice) {
            return false;
        }

        const int gridHeight = slice.gridHeight;
        if (gridHeight > (std::numeric_limits<int>::max() / static_cast<int>(sizeof(uint16_t)))) {
            return false;
        }

        if (footprintRowDeltaScratch_.size() != static_cast<size_t>(gridHeight)) {
            footprintRowDeltaScratch_.assign(static_cast<size_t>(gridHeight), 0.0);
        } else {
            std::fill(footprintRowDeltaScratch_.begin(), footprintRowDeltaScratch_.end(), 0.0);
        }

        std::vector<ServerDataModel::FootprintTradeSample> trades;
        model_.collectFootprintTrades(slice.symbol.toStdString(),
                                      slice.bucketStartMs,
                                      slice.bucketEndMs,
                                      trades);
        for (const auto& sample : trades) {
            const int row = static_cast<int>(std::floor((slice.maxPrice - sample.price) / slice.tickSize));
            if (row < 0 || row >= gridHeight) {
                continue;
            }
            if (sample.side == AggressorSide::Buy) {
                footprintRowDeltaScratch_[static_cast<size_t>(row)] += sample.size;
            } else if (sample.side == AggressorSide::Sell) {
                footprintRowDeltaScratch_[static_cast<size_t>(row)] -= sample.size;
            }
        }

        double maxAbs = 0.0;
        for (double v : footprintRowDeltaScratch_) {
            const double av = std::abs(v);
            if (av > maxAbs) {
                maxAbs = av;
            }
        }
        outQuantScale = (maxAbs > 0.0) ? std::max(1e-9, maxAbs / 32767.0) : 1.0;

        out.resize(gridHeight * static_cast<int>(sizeof(uint16_t)));
        auto* dst = reinterpret_cast<uchar*>(out.data());
        for (int y = 0; y < gridHeight; ++y) {
            const double delta = footprintRowDeltaScratch_[static_cast<size_t>(y)];
            const double q = std::round(delta / outQuantScale);
            const int32_t q16 = static_cast<int32_t>(std::clamp(q, -32768.0, 32767.0));
            const uint16_t biased = static_cast<uint16_t>(q16 + 32768);
            qToLittleEndian<uint16_t>(biased, dst + (y * sizeof(uint16_t)));
        }
        return true;
    }

    bool resolveFootprintGridAndRange(const std::string& symbol,
                                      int& outGridWidth,
                                      int& outGridHeight,
                                      double& outTickSize,
                                      double& outMinPrice,
                                      double& outMaxPrice) {
        if (!owner_) {
            return false;
        }
        const auto& cfg = owner_->serverConfig();
        outGridWidth = std::max(1, cfg.heatmap.gridWidth);
        outGridHeight = std::max(1, cfg.heatmap.gridHeight);
        outTickSize = (cfg.heatmap.tickSize > 0.0) ? cfg.heatmap.tickSize : cfg.orderbook.tickSize;
        if (outTickSize <= 0.0) {
            outTickSize = 0.01;
        }

        const auto& hotData = model_.ensureSymbol(symbol);
        const auto& book = hotData.liveBook;
        if (book.getTickSize() > 0.0) {
            outTickSize = book.getTickSize();
        }

        outMinPrice = book.getMinPrice();
        outMaxPrice = book.getMaxPrice();
        if (!(outMaxPrice > outMinPrice)) {
            double anchorPrice = 0.0;
            std::vector<ServerDataModel::FootprintTradeSample> recentTrades;
            const int64_t nowMs = static_cast<int64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());
            if (model_.collectFootprintTrades(symbol, nowMs - 60'000, nowMs + 1, recentTrades) &&
                !recentTrades.empty()) {
                anchorPrice = recentTrades.back().price;
            }
            if (anchorPrice <= 0.0) {
                anchorPrice = outTickSize * static_cast<double>(outGridHeight);
            }
            const double span = outTickSize * static_cast<double>(outGridHeight);
            outMinPrice = std::max(0.0, anchorPrice - (span * 0.5));
            outMaxPrice = outMinPrice + span;
        }
        return outMaxPrice > outMinPrice && outTickSize > 0.0;
    }

    bool buildFootprintDeltaWindow(const std::string& symbol,
                                   int64_t bucketStartMs,
                                   int64_t bucketEndMs,
                                   int64_t timeframeMs,
                                   int gridHeight,
                                   double minPrice,
                                   double maxPrice,
                                   double tickSize,
                                   QByteArray& out,
                                   double& outQuantScale) {
        if (bucketStartMs <= 0 || bucketEndMs <= bucketStartMs || timeframeMs <= 0 ||
            gridHeight <= 0 || tickSize <= 0.0 || maxPrice <= minPrice) {
            return false;
        }
        if (gridHeight > (std::numeric_limits<int>::max() / static_cast<int>(sizeof(uint16_t)))) {
            return false;
        }
        if (footprintRowDeltaScratch_.size() != static_cast<size_t>(gridHeight)) {
            footprintRowDeltaScratch_.assign(static_cast<size_t>(gridHeight), 0.0);
        } else {
            std::fill(footprintRowDeltaScratch_.begin(), footprintRowDeltaScratch_.end(), 0.0);
        }

        std::vector<ServerDataModel::FootprintTradeSample> trades;
        model_.collectFootprintTrades(symbol, bucketStartMs, bucketEndMs, trades);
        for (const auto& sample : trades) {
            const int row = static_cast<int>(std::floor((maxPrice - sample.price) / tickSize));
            if (row < 0 || row >= gridHeight) {
                continue;
            }
            if (sample.side == AggressorSide::Buy) {
                footprintRowDeltaScratch_[static_cast<size_t>(row)] += sample.size;
            } else if (sample.side == AggressorSide::Sell) {
                footprintRowDeltaScratch_[static_cast<size_t>(row)] -= sample.size;
            }
        }

        double maxAbs = 0.0;
        for (double v : footprintRowDeltaScratch_) {
            const double av = std::abs(v);
            if (av > maxAbs) {
                maxAbs = av;
            }
        }
        outQuantScale = (maxAbs > 0.0) ? std::max(1e-9, maxAbs / 32767.0) : 1.0;
        out.resize(gridHeight * static_cast<int>(sizeof(uint16_t)));
        auto* dst = reinterpret_cast<uchar*>(out.data());
        for (int y = 0; y < gridHeight; ++y) {
            const double delta = footprintRowDeltaScratch_[static_cast<size_t>(y)];
            const double q = std::round(delta / outQuantScale);
            const int32_t q16 = static_cast<int32_t>(std::clamp(q, -32768.0, 32767.0));
            const uint16_t biased = static_cast<uint16_t>(q16 + 32768);
            qToLittleEndian<uint16_t>(biased, dst + (y * sizeof(uint16_t)));
        }
        return true;
    }

    static char tpoLetterForBucket(int64_t bucketStartMs, int64_t timeframeMs) {
        if (timeframeMs <= 0) {
            return 'A';
        }
        const int64_t sequence = bucketStartMs / timeframeMs;
        const int letterIndex = static_cast<int>(sequence % 26);
        return static_cast<char>('A' + ((letterIndex + 26) % 26));
    }

    bool buildTpoColumnWindow(const std::string& symbol,
                              int64_t bucketStartMs,
                              int64_t bucketEndMs,
                              int64_t timeframeMs,
                              int gridHeight,
                              double maxPrice,
                              double tickSize,
                              QByteArray& out) {
        if (bucketStartMs <= 0 || bucketEndMs <= bucketStartMs || timeframeMs <= 0 ||
            gridHeight <= 0 || tickSize <= 0.0) {
            return false;
        }
        out = QByteArray(gridHeight, '\0');
        std::vector<ServerDataModel::FootprintTradeSample> trades;
        model_.collectFootprintTrades(symbol, bucketStartMs, bucketEndMs, trades);
        const char letter = tpoLetterForBucket(bucketStartMs, timeframeMs);
        for (const auto& sample : trades) {
            const int row = static_cast<int>(std::floor((maxPrice - sample.price) / tickSize));
            if (row < 0 || row >= gridHeight) {
                continue;
            }
            out[row] = letter;
        }
        return true;
    }

    // ── Volume Profile builder ──────────────────────────────────────────────
    // Aggregates total trade volume (buy + sell) per price bin for
    // [sessionStartMs, sessionEndMs).  Computes POC and 70 % value area
    // (Steidlmayer methodology) in-place before emitting to the client.
    //
    // Output: volumeBinsF32 – float32 LE, one value per grid row (top→bottom),
    //         same row convention as HeatmapSlice / FootprintSlice.
    bool buildVolumeProfileWindow(const std::string& symbol,
                                  int64_t sessionStartMs,
                                  int64_t sessionEndMs,
                                  int gridHeight,
                                  double maxPrice,
                                  double tickSize,
                                  QByteArray& outBinsF32,
                                  double& outTotalVolume,
                                  int& outPocRow,
                                  double& outPocPrice,
                                  double& outVahPrice,
                                  double& outValPrice) {
        if (sessionStartMs <= 0 || sessionEndMs <= sessionStartMs ||
            gridHeight <= 0 || gridHeight > protocol::SentinelProtocol::kMaxGridHeight ||
            tickSize <= 0.0) {
            return false;
        }
        if (gridHeight > (std::numeric_limits<int>::max() / static_cast<int>(sizeof(float)))) {
            return false;
        }

        // Reuse scratch vector: one float per price bin.
        std::vector<float> bins(static_cast<size_t>(gridHeight), 0.0f);

        std::vector<ServerDataModel::FootprintTradeSample> trades;
        model_.collectFootprintTrades(symbol, sessionStartMs, sessionEndMs, trades);

        for (const auto& sample : trades) {
            const int row = static_cast<int>(std::floor((maxPrice - sample.price) / tickSize));
            if (row < 0 || row >= gridHeight) {
                continue;
            }
            // Volume Profile aggregates absolute volume regardless of direction.
            bins[static_cast<size_t>(row)] += static_cast<float>(sample.size);
        }

        // ── Steidlmayer 70 % Value-Area ─────────────────────────────────────
        double total = 0.0;
        outPocRow    = 0;
        float  pocVol = bins[0];
        for (int i = 0; i < gridHeight; ++i) {
            total += static_cast<double>(bins[static_cast<size_t>(i)]);
            if (bins[static_cast<size_t>(i)] > pocVol) {
                pocVol    = bins[static_cast<size_t>(i)];
                outPocRow = i;
            }
        }
        outTotalVolume = total;
        outPocPrice    = maxPrice - (static_cast<double>(outPocRow) + 0.5) * tickSize;

        // Expand VA from POC.
        const double vaTarget = 0.70 * total;
        double cumVol = static_cast<double>(bins[static_cast<size_t>(outPocRow)]);
        int hi = outPocRow;
        int lo = outPocRow;
        while (cumVol < vaTarget) {
            const int nextLo = lo - 1;
            const int nextHi = hi + 1;
            const float volAbove = (nextLo >= 0)       ? bins[static_cast<size_t>(nextLo)] : -1.0f;
            const float volBelow = (nextHi < gridHeight) ? bins[static_cast<size_t>(nextHi)] : -1.0f;
            if (volAbove < 0.0f && volBelow < 0.0f) break;
            if (volAbove >= volBelow) {
                lo = nextLo;
                cumVol += static_cast<double>(volAbove);
            } else {
                hi = nextHi;
                cumVol += static_cast<double>(volBelow);
            }
        }
        outVahPrice = maxPrice - static_cast<double>(lo) * tickSize;       // top of lowest row index
        outValPrice = maxPrice - static_cast<double>(hi + 1) * tickSize;   // bottom of highest row index

        // Pack bins as little-endian float32.
        outBinsF32.resize(gridHeight * static_cast<int>(sizeof(float)));
        auto* dst = reinterpret_cast<uchar*>(outBinsF32.data());
        for (int i = 0; i < gridHeight; ++i) {
            const float v = bins[static_cast<size_t>(i)];
            std::memcpy(dst + (static_cast<size_t>(i) * sizeof(float)), &v, sizeof(float));
        }
        return true;
    }

    void streamFootprintHistory(const std::string& symbol,
                                int64_t timeframeMs,
                                int64_t endTimeMs,
                                int count) {
        if (symbol.empty() || timeframeMs <= 0 || count <= 0) {
            return;
        }
        int gridWidth = 0;
        int gridHeight = 0;
        double tickSize = 0.0;
        double minPrice = 0.0;
        double maxPrice = 0.0;
        if (!resolveFootprintGridAndRange(symbol, gridWidth, gridHeight, tickSize, minPrice, maxPrice)) {
            return;
        }

        const int sessionPeriods = static_cast<int>(std::max<int64_t>(1, tpoSessionMs_ / timeframeMs));
        gridWidth = std::max(1, std::min(gridWidth, sessionPeriods));
        int effectiveCount = std::max(1, std::min(count, std::max(1, gridWidth)));
        effectiveCount = std::min(effectiveCount, 512);
        int64_t effectiveEnd = endTimeMs;
        if (effectiveEnd <= 0) {
            effectiveEnd = static_cast<int64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());
        }
        effectiveEnd = (effectiveEnd / timeframeMs) * timeframeMs;
        if (effectiveEnd <= 0) {
            return;
        }
        const int64_t firstStart = effectiveEnd - (timeframeMs * effectiveCount);

        nlohmann::json payload;
        payload["type"] = "footprint_history_chunk";
        payload["schema_version"] = protocol::SentinelProtocol::kFootprintSchemaVersion;
        payload["symbol"] = symbol;
        payload["timeframe_ms"] = timeframeMs;
        payload["session_type"] = static_cast<int>(tpoSessionType_);
        payload["grid_width"] = gridWidth;
        payload["grid_height"] = gridHeight;
        payload["format"] = "q16_delta";
        payload["encoding"] = "base64";
        auto columns = nlohmann::json::array();
        for (int i = 0; i < effectiveCount; ++i) {
            const int64_t bucketStart = firstStart + static_cast<int64_t>(i) * timeframeMs;
            const int64_t bucketEnd = bucketStart + timeframeMs;
            QByteArray deltaLevelsQ16;
            double quantScale = 1.0;
            if (!buildFootprintDeltaWindow(symbol,
                                           bucketStart,
                                           bucketEnd,
                                           timeframeMs,
                                           gridHeight,
                                           minPrice,
                                           maxPrice,
                                           tickSize,
                                           deltaLevelsQ16,
                                           quantScale)) {
                continue;
            }
            nlohmann::json item;
            item["time_start"] = bucketStart;
            item["time_end"] = bucketEnd;
            item["min_price"] = minPrice;
            item["max_price"] = maxPrice;
            item["tick_size"] = tickSize;
            item["quant_scale"] = quantScale;
            item["format"] = "q16_delta";
            item["delta_levels_q16"] = deltaLevelsQ16.toBase64().toStdString();
            columns.push_back(std::move(item));
        }
        payload["columns"] = std::move(columns);
        do_write(payload.dump());
    }

    void streamTpoHistory(const std::string& symbol,
                          int64_t timeframeMs,
                          int64_t endTimeMs,
                          int count) {
        if (symbol.empty() || timeframeMs <= 0 || count <= 0) {
            return;
        }
        int gridWidth = 0;
        int gridHeight = 0;
        double tickSize = 0.0;
        double minPrice = 0.0;
        double maxPrice = 0.0;
        if (!resolveFootprintGridAndRange(symbol, gridWidth, gridHeight, tickSize, minPrice, maxPrice)) {
            return;
        }

        int effectiveCount = std::max(1, std::min(count, std::max(1, gridWidth)));
        effectiveCount = std::min(effectiveCount, 512);
        int64_t effectiveEnd = endTimeMs;
        if (effectiveEnd <= 0) {
            effectiveEnd = static_cast<int64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());
        }
        effectiveEnd = (effectiveEnd / timeframeMs) * timeframeMs;
        if (effectiveEnd <= 0) {
            return;
        }
        const int64_t firstStart = effectiveEnd - (timeframeMs * effectiveCount);

        // ── Coinbase candle fallback ─────────────────────────────────────────
        // The trade tape only covers the period since server startup.  For
        // historical buckets where the tape is empty, synthesise TPO letters
        // from the Coinbase OHLCV candle's L/H range (all prices between Low
        // and High are marked as visited — standard OHLC approximation).
        // Fetch the whole range in one call so the inner loop is O(1) lookups.
        std::unordered_map<int64_t, OHLCVBar> candleByBucket;
        const int64_t timeframeSec = timeframeMs / 1000;
        const auto candleGranularity = CoinbaseRestClient::granularityFromSeconds(timeframeSec);
        if (candleGranularity) {
            const int64_t startSec = firstStart / 1000;
            const int64_t endSec   = effectiveEnd / 1000;
            const CandleFetchResult res = owner_->restClient().fetchProductCandles(
                symbol, startSec, endSec, *candleGranularity, effectiveCount);
            if (res.ok) {
                for (const auto& bar : res.candles) {
                    candleByBucket[bar.timestamp_ms] = bar;
                }
            }
        }

        nlohmann::json payload;
        payload["type"] = "tpo_history_chunk";
        payload["schema_version"] = protocol::SentinelProtocol::kTpoSchemaVersion;
        payload["symbol"] = symbol;
        payload["timeframe_ms"] = timeframeMs;
        payload["session_type"] = static_cast<int>(tpoSessionType_);
        payload["grid_width"] = gridWidth;
        payload["grid_height"] = gridHeight;
        payload["format"] = "tpo_ascii";
        payload["encoding"] = "base64";
        auto columns = nlohmann::json::array();
        for (int i = 0; i < effectiveCount; ++i) {
            const int64_t bucketStart = firstStart + static_cast<int64_t>(i) * timeframeMs;
            const int64_t bucketEnd = bucketStart + timeframeMs;
            QByteArray letters;
            if (!buildTpoColumnWindow(symbol,
                                      bucketStart,
                                      bucketEnd,
                                      timeframeMs,
                                      gridHeight,
                                      maxPrice,
                                      tickSize,
                                      letters)) {
                continue;
            }

            // If the trade tape had no data for this bucket, fall back to the
            // Coinbase candle L/H range so the full session profile is visible.
            if (!candleByBucket.empty()) {
                bool hasLetters = false;
                for (int r = 0; r < gridHeight; ++r) {
                    if (letters.at(r) != '\0') { hasLetters = true; break; }
                }
                if (!hasLetters) {
                    const auto it = candleByBucket.find(bucketStart);
                    if (it != candleByBucket.end()) {
                        const auto& bar = it->second;
                        const char letter = tpoLetterForBucket(bucketStart, timeframeMs);
                        // Row 0 = maxPrice; row (gridHeight-1) = minPrice.
                        // High price → smaller row index; Low price → larger row index.
                        const int rowHigh = static_cast<int>(std::floor((maxPrice - bar.high) / tickSize));
                        const int rowLow  = static_cast<int>(std::floor((maxPrice - bar.low)  / tickSize));
                        const int rStart  = std::max(0, rowHigh);
                        const int rEnd    = std::min(gridHeight - 1, rowLow);
                        for (int r = rStart; r <= rEnd; ++r) {
                            letters[r] = letter;
                        }
                    }
                }
            }

            nlohmann::json item;
            item["time_start"] = bucketStart;
            item["time_end"] = bucketEnd;
            item["min_price"] = minPrice;
            item["max_price"] = maxPrice;
            item["tick_size"] = tickSize;
            item["format"] = "tpo_ascii";
            item["letters"] = letters.toBase64().toStdString();
            columns.push_back(std::move(item));
        }
        payload["columns"] = std::move(columns);
        do_write(payload.dump());
    }

public:
    explicit Session(tcp::socket&& socket, ssl::context& ctx,
                     ServerDataModel& model, SentinelStreamServer* owner)
        : ws_(std::move(socket), ctx)
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
        beast::get_lowest_layer(ws_).expires_after(std::chrono::seconds(30));
        ws_.next_layer().async_handshake(
            ssl::stream_base::server,
            beast::bind_front_handler(
                &Session::on_ssl_handshake,
                shared_from_this()));
    }

    void on_ssl_handshake(beast::error_code ec) {
        if (ec) {
            return fail(ec, "ssl_handshake");
        }

        beast::get_lowest_layer(ws_).expires_never();
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
        if (owner_) {
            m_latencySenderId = owner_->registerLatencySender(
                [weak_this = std::weak_ptr<Session>(self), exec = ws_.get_executor()](int ms) {
                    net::post(exec, [weak_this, ms]() {
                        if (auto s = weak_this.lock())
                            s->sendCoinbaseLatency(ms);
                    });
                });
        }
        do_read();
    }

    void sendCoinbaseLatency(int ms) {
        nlohmann::json payload;
        payload["type"] = protocol::toString(protocol::MessageType::CoinbaseLatency);
        payload["ms"] = ms;
        do_write(payload.dump());
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
                    payload["schema_version"] = protocol::SentinelProtocol::kHeatmapSchemaVersion;
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
            } else if (type == "footprint_history_request") {
                std::string symbol = j.value("symbol", "");
                const int64_t timeframeMs = j.value("timeframe_ms", static_cast<int64_t>(0));
                const int64_t endTimeMs = j.value("end_time", static_cast<int64_t>(0));
                const int count = j.value("count", 0);
                if (!symbol.empty() && timeframeMs > 0 && count > 0) {
                    streamFootprintHistory(symbol, timeframeMs, endTimeMs, count);
                }
            } else if (type == "tpo_history_request") {
                std::string symbol = j.value("symbol", "");
                const int64_t timeframeMs = j.value("timeframe_ms", static_cast<int64_t>(0));
                const int sessionType = j.value("session_type", static_cast<int>(SessionManager::SessionType::H24));
                const int64_t endTimeMs = j.value("end_time", static_cast<int64_t>(0));
                const int count = j.value("count", 0);
                if (!symbol.empty() && timeframeMs > 0 && count > 0) {
                    switch (sessionType) {
                        case static_cast<int>(SessionManager::SessionType::H24):
                            tpoSessionType_ = SessionManager::SessionType::H24;
                            break;
                        case static_cast<int>(SessionManager::SessionType::W1):
                            tpoSessionType_ = SessionManager::SessionType::W1;
                            break;
                        default:
                            tpoSessionType_ = SessionManager::SessionType::H24;
                            break;
                    }
                    tpoBucketMs_ = timeframeMs;
                    tpoSessionMs_ = SessionManager::sessionDurationMs(tpoSessionType_);
                    streamTpoHistory(symbol, timeframeMs, endTimeMs, count);
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
                    payload["schema_version"] = protocol::SentinelProtocol::kCandleSchemaVersion;
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
                    payload["schema_version"] = protocol::SentinelProtocol::kCandleSchemaVersion;
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
            } else if (type == "screener_request") {
                const std::string asset   = j.value("asset", "crypto");
                const int         limit   = j.value("limit", 50);
                const double      minVol  = j.value("min_volume", 0.0);

                auto self = shared_from_this();
                std::thread([self, asset, limit, minVol]() {
                    // Locate scripts/ dir relative to the server binary.
                    // Binary is at <repo>/build/<preset>/apps/sentinel-server/Debug/
                    // scripts/ is at <repo>/scripts/  (5 levels up)
                    const QString appDir = QCoreApplication::applicationDirPath();
                    QString scriptsDir;
                    const QStringList dirCandidates = {
                        QDir(appDir).absoluteFilePath("../../../../../scripts"),
                        QDir(appDir).absoluteFilePath("../../../../scripts"),
                        QDir(appDir).absoluteFilePath("../../../scripts"),
                        QDir(appDir).absoluteFilePath("../../scripts"),
                        QDir(appDir).absoluteFilePath("../scripts"),
                        QDir(appDir).absoluteFilePath("scripts"),
                    };
                    for (const auto& c : dirCandidates) {
                        if (QFileInfo::exists(QDir(c).absoluteFilePath("screener/screener_fetch.py"))) {
                            scriptsDir = QDir(c).absolutePath();
                            break;
                        }
                    }

                    if (scriptsDir.isEmpty()) {
                        sLog_Error("Screener: could not locate scripts/ dir from appDir=" << appDir);
                        self->send_error("screener_request", "", "scripts/ dir not found (checked relative to server binary)");
                        return;
                    }

                    const QString scriptPath = QDir(scriptsDir).absoluteFilePath("screener/screener_fetch.py");
                    sLog_App("Screener: running " << scriptPath << " asset=" << QString::fromStdString(asset));

                    // Build command: uv run python <script> --asset <x> --limit <n> --min-volume <v>
                    // Use popen — QProcess requires a Qt event loop and cannot be used in std::thread.
                    const std::string cmd =
                        "cd \"" + scriptsDir.toStdString() + "\" && "
                        "uv run python \"" + scriptPath.toStdString() + "\""
                        " --asset "      + asset +
                        " --limit "      + std::to_string(limit) +
                        " --min-volume " + std::to_string(minVol) +
                        " 2>&1";

#ifdef _WIN32
                    FILE* pipe = _popen(cmd.c_str(), "r");
#else
                    FILE* pipe = popen(cmd.c_str(), "r");
#endif
                    if (!pipe) {
                        self->send_error("screener_request", "", "failed to launch screener_fetch.py");
                        return;
                    }

                    std::string output;
                    char buf[4096];
                    while (fgets(buf, sizeof(buf), pipe)) {
                        output += buf;
                    }
#ifdef _WIN32
                    _pclose(pipe);
#else
                    pclose(pipe);
#endif

                    sLog_App("Screener: output length=" << output.size());

                    const std::string marker = "SCREENER_DATA:";
                    const auto idx = output.find(marker);
                    if (idx == std::string::npos) {
                        sLog_Error("Screener: no SCREENER_DATA marker. Output: " << QString::fromStdString(output.substr(0, 500)));
                        self->send_error("screener_request", "",
                                         "no SCREENER_DATA in output: " + output.substr(0, 200));
                        return;
                    }

                    // Trim to just the JSON after the marker
                    std::string dataStr = output.substr(idx + marker.size());
                    // Strip trailing whitespace/newlines
                    while (!dataStr.empty() && (dataStr.back() == '\n' || dataStr.back() == '\r' || dataStr.back() == ' '))
                        dataStr.pop_back();

                    nlohmann::json data = nlohmann::json::parse(dataStr, nullptr, false);
                    if (data.is_discarded()) {
                        sLog_Error("Screener: JSON parse failed. Raw: " << QString::fromStdString(dataStr.substr(0, 200)));
                        self->send_error("screener_request", "", "failed to parse screener JSON");
                        return;
                    }

                    nlohmann::json response;
                    response["type"]      = "screener_update";
                    response["asset"]     = data.value("asset", asset);
                    response["rows"]      = data.value("rows", nlohmann::json::array());
                    response["row_count"] = static_cast<int>(response["rows"].size());
                    sLog_App("Screener: sending " << response["row_count"].get<int>() << " rows to client");
                    self->do_write(response.dump());
                }).detach();
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
        j["schema_version"] = protocol::SentinelProtocol::kHeatmapSchemaVersion;
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

        double quantScale = 1.0;
        if (!buildFootprintDeltaColumn(slice, footprintDeltaScratch_, quantScale)) {
            return;
        }

        nlohmann::json footprint;
        footprint["type"] = "footprint_slice";
        footprint["schema_version"] = protocol::SentinelProtocol::kFootprintSchemaVersion;
        footprint["symbol"] = sym;
        footprint["time_start"] = slice.bucketStartMs;
        footprint["time_end"] = slice.bucketEndMs;
        footprint["timeframe_ms"] = slice.timeframeMs;
        footprint["grid_width"] = slice.gridWidth;
        footprint["grid_height"] = slice.gridHeight;
        footprint["min_price"] = slice.minPrice;
        footprint["max_price"] = slice.maxPrice;
        footprint["tick_size"] = slice.tickSize;
        footprint["quant_scale"] = quantScale;
        footprint["format"] = "q16_delta";
        footprint["encoding"] = "base64";
        footprint["delta_levels_q16"] = footprintDeltaScratch_.toBase64().toStdString();

        if (qEnvironmentVariableIsSet("SENTINEL_CHART_DEBUG")) {
            sLog_Debug(QString("Footprint emit: symbol=%1 t=[%2..%3] tfMs=%4 grid=%5x%6 bytes=%7 q=%8")
                           .arg(QString::fromStdString(sym))
                           .arg(slice.bucketStartMs)
                           .arg(slice.bucketEndMs)
                           .arg(slice.timeframeMs)
                           .arg(slice.gridWidth)
                           .arg(slice.gridHeight)
                           .arg(footprintDeltaScratch_.size())
                           .arg(quantScale, 0, 'g', 8));
        }

        do_write(footprint.dump());

        const int64_t tpoTimeframeMs = (tpoBucketMs_ > 0) ? tpoBucketMs_ : 60000;
        const int64_t tpoBucketStart = (slice.bucketStartMs / tpoTimeframeMs) * tpoTimeframeMs;
        const int64_t tpoBucketEnd = tpoBucketStart + tpoTimeframeMs;
        const int tpoSessionPeriods =
            static_cast<int>(std::max<int64_t>(1, tpoSessionMs_ / tpoTimeframeMs));
        const int tpoGridWidth = std::max(1, std::min(slice.gridWidth, tpoSessionPeriods));

        QByteArray tpoLetters;
        if (!buildTpoColumnWindow(sym,
                                  tpoBucketStart,
                                  tpoBucketEnd,
                                  tpoTimeframeMs,
                                  slice.gridHeight,
                                  slice.maxPrice,
                                  slice.tickSize,
                                  tpoLetters)) {
            return;
        }

        nlohmann::json tpo;
        tpo["type"] = "tpo_slice";
        tpo["schema_version"] = protocol::SentinelProtocol::kTpoSchemaVersion;
        tpo["symbol"] = sym;
        tpo["time_start"] = tpoBucketStart;
        tpo["time_end"] = tpoBucketEnd;
        tpo["timeframe_ms"] = tpoTimeframeMs;
        tpo["session_type"] = static_cast<int>(tpoSessionType_);
        tpo["grid_width"] = tpoGridWidth;
        tpo["grid_height"] = slice.gridHeight;
        tpo["min_price"] = slice.minPrice;
        tpo["max_price"] = slice.maxPrice;
        tpo["tick_size"] = slice.tickSize;
        tpo["format"] = "tpo_ascii";
        tpo["encoding"] = "base64";
        tpo["letters"] = tpoLetters.toBase64().toStdString();
        do_write(tpo.dump());

        // ── Mode A: Volume Profile slice (session-scoped) ───────────────────
        // Use SessionManager to determine the current session boundary so that
        // the profile always covers exactly one full session window.
        const auto sessionBoundary =
            SessionManager::sessionContaining(slice.bucketStartMs, tpoSessionType_);
        QByteArray vpBinsF32;
        double vpTotalVolume = 0.0;
        int    vpPocRow      = 0;
        double vpPocPrice    = 0.0;
        double vpVahPrice    = 0.0;
        double vpValPrice    = 0.0;
        if (sessionBoundary.valid &&
            buildVolumeProfileWindow(sym,
                                     sessionBoundary.startMs,
                                     sessionBoundary.endMs,
                                     slice.gridHeight,
                                     slice.maxPrice,
                                     slice.tickSize,
                                     vpBinsF32,
                                     vpTotalVolume,
                                     vpPocRow,
                                     vpPocPrice,
                                     vpVahPrice,
                                     vpValPrice)) {
            nlohmann::json vp;
            vp["type"]           = "volume_profile_slice";
            vp["schema_version"] = protocol::SentinelProtocol::kVolumeProfileSchemaVersion;
            vp["symbol"]         = sym;
            vp["session_start"]  = sessionBoundary.startMs;
            vp["session_end"]    = sessionBoundary.endMs;
            vp["session_type"]   = static_cast<int>(tpoSessionType_);
            vp["grid_height"]    = slice.gridHeight;
            vp["min_price"]      = slice.minPrice;
            vp["max_price"]      = slice.maxPrice;
            vp["tick_size"]      = slice.tickSize;
            vp["total_volume"]   = vpTotalVolume;
            vp["poc_price"]      = vpPocPrice;
            vp["vah_price"]      = vpVahPrice;
            vp["val_price"]      = vpValPrice;
            vp["format"]         = "vp_f32";
            vp["encoding"]       = "base64";
            vp["volume_bins"]    = vpBinsF32.toBase64().toStdString();
            do_write(vp.dump());
        }
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
        payload["schema_version"] = protocol::SentinelProtocol::kCandleSchemaVersion;
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
        payload["schema_version"] = protocol::SentinelProtocol::kCandleSchemaVersion;
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
        if (owner_ && m_latencySenderId != 0) {
            owner_->unregisterLatencySender(m_latencySenderId);
            m_latencySenderId = 0;
        }
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
    , m_restClient(std::make_unique<CoinbaseRestClient>(auth,
                                                        "api.coinbase.com",
                                                        "443",
                                                        config.mdc.sslCaBundle))
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

        const auto& tls = m_serverConfig.tls;
        m_sslCtx.use_certificate_chain_file(tls.certFile);
        m_sslCtx.use_private_key_file(tls.keyFile, ssl::context::pem);

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
                std::make_shared<Session>(std::move(socket), m_sslCtx, m_model, this)->run();
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

uint64_t SentinelStreamServer::registerLatencySender(std::function<void(int)> sendFn) {
    const uint64_t id = m_nextLatencySenderId++;
    std::lock_guard<std::mutex> lock(m_latencySendersMutex);
    m_latencySenders.emplace_back(id, std::move(sendFn));
    return id;
}

void SentinelStreamServer::unregisterLatencySender(uint64_t id) {
    std::lock_guard<std::mutex> lock(m_latencySendersMutex);
    m_latencySenders.erase(
        std::remove_if(m_latencySenders.begin(), m_latencySenders.end(),
            [id](const std::pair<uint64_t, std::function<void(int)>>& p) { return p.first == id; }),
        m_latencySenders.end());
}

void SentinelStreamServer::broadcastCoinbaseLatency(int milliseconds) {
    std::vector<std::function<void(int)>> copy;
    {
        std::lock_guard<std::mutex> lock(m_latencySendersMutex);
        copy.reserve(m_latencySenders.size());
        for (auto& p : m_latencySenders)
            copy.push_back(p.second);
    }
    for (auto& fn : copy)
        fn(milliseconds);
}
