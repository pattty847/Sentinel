#include "metrics/MetricsRegistry.hpp"
#include "SentinelStreamServer.hpp"
#include "HeatmapSlice.hpp"
#include "../servermodel/TradeOverlayPublisher.hpp"
#include <boost/asio/steady_timer.hpp>
#include "SentinelStreamProtocol.hpp"
#include "RecordingHistoryWire.hpp"
#include "ChunkWire.hpp"
#include <list>
#include "SentinelLogging.hpp"
#include "../servermodel/SessionManager.hpp"
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/strand.hpp>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <unordered_set>
#include <nlohmann/json.hpp>
#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QtEndian>
#include <cstdio>
#include "../marketdata/auth/Authenticator.hpp"
#include "../marketdata/rest/CoinbaseRestClient.hpp"
#include "../marketdata/model/TradeData.h"
#include "../trading/LiveTradingSession.hpp"
#include "Cpp20Utils.hpp"

#include <filesystem>
#ifdef Q_OS_UNIX
#include <cerrno>
#include <csignal>
#include <unistd.h>
#endif

namespace beast = boost::beast;         // from <boost/beast.hpp>
namespace http = beast::http;           // from <boost/beast/http.hpp>
namespace websocket = beast::websocket; // from <boost/beast/websocket.hpp>
namespace net = boost::asio;            // from <boost/asio.hpp>
using tcp = boost::asio::ip::tcp;       // from <boost/asio/ip/tcp.hpp>

namespace {
// Allow uv startup, tvscreener's own 30-second HTTP timeout, and serialization.
constexpr auto kScreenerProcessBudget = std::chrono::seconds(90);
constexpr auto kProcessTerminationGrace = std::chrono::milliseconds(250);

void terminateProcessTree(QProcess& process) {
#ifdef Q_OS_UNIX
    // The child modifier makes the launched process its group's leader before
    // exec. Keep the pgid even if uv exits during the grace period: Python may
    // still be alive. Signal the whole group, then reap the direct child.
    const auto group = static_cast<pid_t>(process.processId());
    if (group > 0) ::kill(-group, SIGTERM);
    std::this_thread::sleep_for(kProcessTerminationGrace);
    if (group > 0) ::kill(-group, SIGKILL);
#else
    process.terminate();
    process.waitForFinished(static_cast<int>(kProcessTerminationGrace.count()));
#endif
    // Also handles cancellation during startup, before setpgid has run.
    process.kill();
    process.waitForFinished(1000);
}

struct ProcessResult { std::string output, error; };
ProcessResult runBoundedProcess(const QString& program, const QStringList& arguments,
                               const QString& directory, const std::function<bool()>& stopped,
                               std::chrono::milliseconds budget = kScreenerProcessBudget) {
    ProcessResult result;
    if (stopped()) { result.error = "process cancelled"; return result; }
    QProcess process;
#ifdef Q_OS_UNIX
    // Do not let the screener inherit server sockets or files (a hung child
    // would otherwise hold the listen port across a server restart).
    process.setUnixProcessParameters(QProcess::UnixProcessFlag::CloseFileDescriptors);
    process.setChildProcessModifier([&process] {
        if (::setpgid(0, 0) == -1) process.failChildProcessModifier("setpgid", errno);
    });
#endif
    process.setWorkingDirectory(directory);
    process.setProcessChannelMode(QProcess::MergedChannels);
    const auto deadline = std::chrono::steady_clock::now() + budget;
    process.start(program, arguments);
    while (process.state() != QProcess::NotRunning) {
        if (stopped() || std::chrono::steady_clock::now() >= deadline) {
            terminateProcessTree(process);
            result.error = stopped() ? "process cancelled" : "process deadline exceeded";
            return result;
        }
        process.waitForFinished(50);
        result.output += process.readAll().toStdString();
        if (result.output.size() > 8 * 1024 * 1024) {
            terminateProcessTree(process);
            result.error = "process output budget exceeded";
            return result;
        }
    }
    if (process.error() == QProcess::FailedToStart || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
        result.error = "process failed: " + process.errorString().toStdString();
    return result;
}


int64_t tradeTimestampMs(const Trade& trade) {
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(trade.timestamp.time_since_epoch()).count());
}

nlohmann::json buildServerConfigPayload(const ServerConfig& cfg, bool recordingAvailable, bool liveAvailable) {
    nlohmann::json payload;
    payload["type"] = "server_config";
    payload["schema_version"] = protocol::SentinelProtocol::kServerConfigSchemaVersion;
    payload["timeframes_ms"] = cfg.heatmap.timeframesMs;
    const int64_t servedTimeframeMs = cfg.heatmap.activeTimeframeMs > 0
        ? cfg.heatmap.activeTimeframeMs
        : (cfg.heatmap.timeframesMs.empty() ? 0 : cfg.heatmap.timeframesMs.front());
    std::vector<int64_t> servedHeatmap;
    if (servedTimeframeMs > 0) servedHeatmap.push_back(servedTimeframeMs);
    if (servedTimeframeMs == 60'000) {
        for (const auto tf : cfg.heatmap.timeframesMs) {
            if (tf > 60'000 && tf % 60'000 == 0) servedHeatmap.push_back(tf);
        }
    }
    payload["trade_overlays"] = {
        {"grid_width", cfg.tradeOverlays.gridWidth}, {"grid_height", cfg.tradeOverlays.gridHeight},
        {"tick_size", cfg.tradeOverlays.tickSize}, {"footprint_timeframe_ms", cfg.tradeOverlays.footprintTimeframeMs}};
    payload["heatmap"] = {
        {"served_timeframes_ms", servedHeatmap},
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
    payload["recording"] = protocol::recordingwire::capability(cfg, recordingAvailable);
    payload["recording"]["chunk_wire_version"] = heatmap::kChunkWireVersion;
    payload["recording"]["chunk_live"] = recordingAvailable && liveAvailable;
    return payload;
}

double resolveMidPrice(const LiveOrderBook& book) {
    const auto& bids = book.getBids();
    const auto& asks = book.getAsks();

    double bestBid = 0.0;
    for (size_t i = bids.size(); i > 0; --i) {
        if (bids[i - 1] > 0.0) {
            bestBid = book.index_to_price(i - 1);
            break;
        }
    }

    double bestAsk = 0.0;
    for (size_t i = 0; i < asks.size(); ++i) {
        if (asks[i] > 0.0) {
            bestAsk = book.index_to_price(i);
            break;
        }
    }

    if (bestBid > 0.0 && bestAsk > 0.0) {
        return (bestBid + bestAsk) * 0.5;
    }
    return (bestBid > 0.0) ? bestBid : bestAsk;
}


}

class Session : public std::enable_shared_from_this<Session> {
    friend struct RecordingServerStopTest;
    friend struct HeatmapChunkWireTest;
    friend struct ServerFeedAdmissionTest;
    websocket::stream<beast::ssl_stream<beast::tcp_stream>> ws_;
    beast::flat_buffer buffer_;
    ServerDataModel& model_;
    SentinelStreamServer* owner_ = nullptr;
    std::string peer_;  // "ip:port" of the client, for log lines only
    std::unordered_set<std::string> subscriptions_;
    // binary: sent as a WebSocket binary frame. chunk: counted against the
    // per-session chunk byte budget instead of the slow-client write limit.
    struct PendingWrite {
        std::string payload;
        bool recording = false, binary = false, chunk = false, rawLive = false;
        std::weak_ptr<recording::LiveService::RawSubscription> rawSubscription;
    };
    std::list<PendingWrite> write_queue_; // insertion preserves the in-flight Beast buffer
    std::shared_ptr<recording::LiveWriteSlot> recordingWriteSlot_ = std::make_shared<recording::LiveWriteSlot>();
    recording::LiveRegistrationGate recordingRegistrationGate_;
    std::shared_ptr<recording::LiveService::Subscription> recordingView_;
    std::shared_ptr<recording::LiveWriteBudget> rawWriteSlot_ = std::make_shared<recording::LiveWriteBudget>();
    std::map<std::string, std::shared_ptr<recording::LiveService::RawSubscription>> rawViews_;
    std::atomic_size_t pendingWriteBytes_{0};
    std::atomic_size_t pendingModelEvents_{0};
    std::atomic_bool closing_{false};
    std::atomic_bool closePosted_{false};
    
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
    uint64_t m_latencySenderId = 0;
    uint64_t m_tradingBroadcasterId = 0;
    bool m_tradingBroadcasterRegistered = false;

    static constexpr size_t kMaxPendingWriteBytes = 16U * 1024U * 1024U;
    static constexpr size_t kMaxPendingModelEvents = 2048U;
    // Heatmap chunk budget (executor-only counters). A job is admitted only while
    // fewer than kMaxChunkJobs are building and queued chunk replies hold less than
    // kMaxChunkBytes; anything else gets an explicit Busy error frame. One reply
    // (at most 16 MiB) can overshoot the byte budget per admitted job. Half of the
    // server-wide history pool (8), so one session cannot starve candle/TPO history.
    static constexpr size_t kMaxChunkJobs = 4U;
    static constexpr size_t kMaxChunkBytes = 32U * 1024U * 1024U;
    size_t chunkJobs_ = 0;
    size_t chunkBytes_ = 0;
    // Availability push: last message sent and the watermark fingerprint it
    // was built from, per subscribed symbol. Executor-only.
    struct AvailabilityState { std::vector<int64_t> fingerprint; std::string sent; bool busy = false; };
    std::unordered_map<std::string, AvailabilityState> availability_;

    void releaseWrite(const PendingWrite& write) {
        if (write.recording) recordingWriteSlot_->release();
        else if (write.rawLive) rawWriteSlot_->release(write.payload.size());
        else if (write.chunk) chunkBytes_ -= std::min(chunkBytes_, write.payload.size());
        else releasePendingWriteBytes(write.payload.size());
    }

    void releasePendingWriteBytes(size_t bytes) {
        size_t current = pendingWriteBytes_.load(std::memory_order_relaxed);
        while (!pendingWriteBytes_.compare_exchange_weak(
            current,
            (bytes >= current) ? 0U : current - bytes,
            std::memory_order_relaxed)) {
        }
    }

    void disconnectModelSignals() {
        QObject::disconnect(tradeConn_);
        QObject::disconnect(bookConn_);
        QObject::disconnect(heatmapConn_);
        QObject::disconnect(barUpdatedConn_);
        QObject::disconnect(barClosedConn_);
        tradeConn_ = {};
        bookConn_ = {};
        heatmapConn_ = {};
        barUpdatedConn_ = {};
        barClosedConn_ = {};
    }

    void requestClose(const char* reason) {
        if (closing_.load(std::memory_order_acquire) || closePosted_.exchange(true)) {
            return;
        }
        auto self = shared_from_this();
        net::post(ws_.get_executor(), [self, reason = std::string(reason)] {
            self->beginClose(reason.c_str());
        });
    }

    template <typename Callback>
    void postModelEvent(Callback&& callback) {
        if (closing_.load(std::memory_order_acquire)) {
            return;
        }

        const size_t queued = pendingModelEvents_.fetch_add(1, std::memory_order_relaxed);
        if (queued >= kMaxPendingModelEvents) {
            pendingModelEvents_.fetch_sub(1, std::memory_order_relaxed);
            if (!closePosted_.load(std::memory_order_relaxed)) {
                sLog_Warning("Closing slow client: peer=" << peer_ << " model event backlog exceeded"
                             << " queued=" << queued << " limit=" << kMaxPendingModelEvents);
            }
            requestClose("model event backlog exceeded");
            return;
        }

        auto weak = weak_from_this();
        net::post(ws_.get_executor(),
                  [weak, callback = std::forward<Callback>(callback)]() mutable {
                      if (auto self = weak.lock()) {
                          self->pendingModelEvents_.fetch_sub(1, std::memory_order_relaxed);
                          if (!self->closing_.load(std::memory_order_acquire)) {
                              callback(*self);
                          }
                      }
                  });
    }

    void beginClose(const char* reason) {
        if (closing_.exchange(true)) {
            return;
        }

        disconnectModelSignals();
        overlayTimer_.cancel();
        availabilityTimer_.cancel();
        availability_.clear();
        overlayHistory_.clear();
        if (recordingView_) recordingView_->active.store(false);
        recordingView_.reset();
        for (const auto& [symbol, view] : rawViews_) view->active.store(false);
        rawViews_.clear();
        if (owner_ && m_latencySenderId != 0) {
            owner_->unregisterLatencySender(m_latencySenderId);
            m_latencySenderId = 0;
        }
        if (owner_ && m_tradingBroadcasterRegistered) {
            owner_->unregisterTradingBroadcaster(m_tradingBroadcasterId);
            m_tradingBroadcasterRegistered = false;
        }
        const size_t subscriptionCount = subscriptions_.size();
        if (owner_) {
            for (const auto& symbol : subscriptions_) {
                owner_->notifyClientUnsubscribed(symbol);
            }
            subscriptions_.clear();
        }

        beast::error_code ignored;
        beast::get_lowest_layer(ws_).cancel();
        beast::get_lowest_layer(ws_).socket().shutdown(tcp::socket::shutdown_both, ignored);
        beast::get_lowest_layer(ws_).socket().close(ignored);

        if (write_queue_.size() > 1) {
            for (auto it = std::next(write_queue_.begin()); it != write_queue_.end(); ++it)
                releaseWrite(*it);
            write_queue_.erase(std::next(write_queue_.begin()), write_queue_.end());
        }

        if (owner_) {
            owner_->unregisterSession(this);
        }
        sLog_App("Sentinel client session closed: peer=" << peer_ << " reason=" << reason
                 << " subscriptions=" << subscriptionCount);
    }

    struct OverlayState {
        trade_overlay::Request request;
        uint64_t generation = 0;
        std::shared_ptr<std::atomic_bool> cancelled = std::make_shared<std::atomic_bool>(false);
    };
    std::map<std::string, OverlayState> overlays_;
    std::deque<trade_overlay::Request> overlayHistory_;
    // Cancel flags of queued/running TPO history requests, by request_id. Bounded
    // by the history queue (8) plus the running job; erased on completion/cancel.
    std::map<std::string, std::shared_ptr<std::atomic_bool>> tpoRequestCancel_;
    net::steady_timer overlayTimer_{ws_.get_executor()};
    net::steady_timer availabilityTimer_{ws_.get_executor()};
    bool overlayBusy_ = false;
    size_t overlayCursor_ = 0;
    uint64_t nextOverlayGeneration_ = 1;
    bool overlayHistoryTurn_ = true;

    OverlayState& overlayState(const std::string& symbol) {
        auto [it, inserted] = overlays_.try_emplace(symbol);
        if (inserted) {
            it->second.generation = nextOverlayGeneration_++;
            const auto& cfg = owner_->serverConfig().tradeOverlays;
            it->second.request.symbol = symbol;
            it->second.request.grid = {cfg.gridWidth, cfg.gridHeight, cfg.tickSize, 0};
            it->second.request.footprintMs = cfg.footprintTimeframeMs;
        }
        return it->second;
    }
    trade_overlay::StopRequested overlayStopRequested(const std::string& symbol,
                                                      std::shared_ptr<std::atomic_bool> request = {}) {
        return [weak = weak_from_this(), cancelled = overlays_.at(symbol).cancelled, request] {
            const auto self = weak.lock();
            return cancelled->load() || (request && request->load()) || !self || self->closing_.load() ||
                self->closePosted_.load() || !self->owner_->m_running.load();
        };
    }
    std::shared_ptr<std::atomic_bool> tpoRequestFlag(const std::string& requestId) const {
        const auto it = tpoRequestCancel_.find(requestId);
        return it == tpoRequestCancel_.end() ? nullptr : it->second;
    }
    // The client abandoned or superseded a TPO history page: drop it if queued,
    // stop its REST paging if running. No reply is sent for a cancelled page.
    void cancelOverlayHistory(const nlohmann::json& j) {
        const auto requestId = j.value("request_id", std::string{});
        const auto it = tpoRequestCancel_.find(requestId);
        if (requestId.empty() || it == tpoRequestCancel_.end()) return;
        it->second->store(true);
        tpoRequestCancel_.erase(it);
        std::erase_if(overlayHistory_, [&](const auto& q) { return q.requestId == requestId; });
        sLog_Data("TPO history cancelled: peer=" << peer_ << " request_id=" << requestId);
    }
    void armOverlayTimer() {
        if (closing_.load()) return;
        overlayTimer_.expires_after(std::chrono::milliseconds(trade_overlay::kRefreshMs));
        overlayTimer_.async_wait([weak = weak_from_this()](beast::error_code ec) {
            if (auto self = weak.lock(); self && !ec && !self->closing_.load()) {
                self->pumpOverlays();
                self->armOverlayTimer();
            }
        });
    }
    // Overlay errors echo the client's request_id (TPO history) so it can pace pages.
    void send_overlay_error(const std::string& symbol, const std::string& requestId, const std::string& message) {
        if (requestId.empty()) { send_error("trade_overlay", symbol, message); return; }
        sLog_Warning("Sending error to client: peer=" << peer_ << " context=trade_overlay symbol=" << symbol
                     << " request_id=" << requestId << " message=" << message);
        do_write(nlohmann::json{{"type", "error"}, {"context", "trade_overlay"}, {"symbol", symbol},
                                {"request_id", requestId}, {"message", message}}.dump());
    }
    void requestOverlayHistory(const nlohmann::json& j, bool tpo) {
        const auto symbol = j.value("symbol", std::string{});
        const auto tf = j.value("timeframe_ms", int64_t{0});
        std::string requestId;
        if (tpo) {
            if (j.contains("request_id") && j["request_id"].is_string())
                requestId = j["request_id"].get<std::string>();
            if (requestId.empty() || requestId.size() > trade_overlay::kMaxRequestIdLength ||
                tpoRequestCancel_.contains(requestId) || tpoRequestCancel_.size() >= 16) {
                send_error("trade_overlay", symbol, "TPO history requires a unique request_id of at most 64 characters");
                return;
            }
        }
        if (!subscriptions_.contains(symbol) || (overlays_.size() >= 16 && !overlays_.contains(symbol)) ||
            tf < (tpo ? 60000 : 1000) || tf > 86400000 || j.value("count", 0) <= 0 || overlayHistory_.size() >= 8) {
            send_overlay_error(symbol, requestId, "invalid request or overlay queue full"); return;
        }
        auto& state = overlayState(symbol);
        auto q = state.request;
        if (tpo) {
            q.tpoMs = tf;
            const int type = j.value("session_type", static_cast<int>(q.session));
            if (type < 0 || type > static_cast<int>(SessionManager::SessionType::M1)) {
                send_overlay_error(symbol, requestId, "invalid session"); return;
            }
            q.session = static_cast<SessionManager::SessionType>(type);
        } else q.footprintMs = tf;
        const auto duration = SessionManager::sessionDurationMs(q.session);
        if (duration % q.tpoMs != 0 || duration / q.tpoMs > trade_overlay::kMaxGridWidth) {
            send_overlay_error(symbol, requestId, "TPO timeframe must partition the session within the grid budget"); return;
        }
        if (j.contains("tick_size")) q.grid.tick = j.at("tick_size").get<double>();
        if (j.contains("rows")) q.grid.rows = j.at("rows").get<int>();
        if (j.contains("price_min")) q.grid.maxPrice = j.at("price_min").get<double>() + q.grid.rows * q.grid.tick;
        auto check = q.grid;
        if (check.maxPrice == 0) check.maxPrice = check.rows * check.tick;
        if (!check.valid()) { send_overlay_error(symbol, requestId, "invalid overlay grid"); return; }
        // A selection change invalidates worker replies and queued requests for that symbol.
        const bool changed = q.footprintMs != state.request.footprintMs || q.tpoMs != state.request.tpoMs ||
            q.session != state.request.session || q.grid.tick != state.request.grid.tick ||
            q.grid.rows != state.request.grid.rows || q.grid.maxPrice != state.request.grid.maxPrice;
        if (changed) {
            state.cancelled->store(true);
            state.cancelled = std::make_shared<std::atomic_bool>(false);
            state.generation = nextOverlayGeneration_++; q.previousMs = 0;
        }
        state.request = q;
        q.kind = tpo ? trade_overlay::Kind::TpoHistory : trade_overlay::Kind::FootprintHistory;
        q.endMs = j.value("end_time", int64_t{0});
        q.count = std::clamp(j.value("count", 128), 1, trade_overlay::kMaxColumns);
        q.requestId = requestId;
        if (tpo) tpoRequestCancel_.emplace(requestId, std::make_shared<std::atomic_bool>(false));
        overlayHistory_.push_back(std::move(q));
    }
    void pumpOverlays() {
        if (overlayBusy_ || !owner_ || closing_.load()) return;
        // Bound per-session state, and round-robin live symbols. Never enqueue refresh backlog.
        for (const auto& symbol : subscriptions_) {
            if (overlays_.size() >= 16) break;
            overlayState(symbol);
        }
        std::erase_if(overlays_, [&](const auto& entry) { return !subscriptions_.contains(entry.first); });
        if (overlays_.empty()) return;
        trade_overlay::Request q;
        if (!overlayHistory_.empty() && overlayHistoryTurn_) {
            const auto pending = overlayHistory_.front(); overlayHistory_.pop_front();
            if (!overlays_.contains(pending.symbol)) { tpoRequestCancel_.erase(pending.requestId); return; }
            q = overlays_.at(pending.symbol).request;
            q.kind = pending.kind; q.endMs = pending.endMs; q.count = pending.count; q.requestId = pending.requestId;
            overlayHistoryTurn_ = false;
        } else {
            auto it = overlays_.begin(); std::advance(it, overlayCursor_++ % overlays_.size());
            q = it->second.request; q.kind = trade_overlay::Kind::Live;
            overlayHistoryTurn_ = true;
        }
        q.nowMs = model_.exchangeNowMs();
        const auto generation = overlays_.at(q.symbol).generation;
        const auto executor = ws_.get_executor();
        auto* model = &model_;
        auto* rest = &owner_->restClient();
        const auto requestFlag = q.requestId.empty() ? nullptr : tpoRequestFlag(q.requestId);
        auto stopped = overlayStopRequested(q.symbol, requestFlag);
        const bool queued = owner_->submitHistoryTask([weak = weak_from_this(), executor, model, rest, q, generation, stopped, requestFlag] {
            trade_overlay::Result result;
            try {
                std::vector<ServerDataModel::FootprintTradeSample> trades;
                const auto window = trade_overlay::tradeWindow(q);
                int64_t retainedFromMs = 0;
                if (stopped()) {
                    result.error = "overlay history cancelled";
                } else if (!model->collectOverlayTrades(q.symbol, window.startMs, window.endMs,
                                                 trade_overlay::kMaxTrades, trades, &retainedFromMs)) {
                    result.error = "overlay trade budget exceeded";
                } else {
                    const auto candles = trade_overlay::fetchTpoCandles(q, retainedFromMs,
                        [rest, &q](int64_t startSec, int64_t endSec, int64_t granularitySec, int limit) {
                            const char* name = trade_overlay::candleGranularityName(granularitySec);
                            if (!name) { CandleFetchResult bad; bad.error = "unsupported candle granularity"; return bad; }
                            return rest->fetchProductCandles(q.symbol, startSec, endSec, name, limit);
                        }, stopped);
                    if (stopped()) {
                        result.error = "overlay history cancelled";
                    } else if (!candles.ok) {
                        result.error = "TPO candle history fetch failed: " + candles.error;
                        sLog_Warning("Trade overlay candle fetch failed: symbol=" << q.symbol << " error=" << candles.error);
                    } else result = trade_overlay::build(q, trades, candles.candles, retainedFromMs);
                }
            } catch (const std::exception& e) { result.error = e.what(); }
            net::post(executor, [weak, q, generation, requestFlag, result = std::move(result)]() mutable {
                const auto self = weak.lock();
                if (!self || self->closing_.load()) return;
                self->overlayBusy_ = false;
                if (requestFlag && requestFlag->load()) return;  // cancelled by the client: no reply
                const auto it = self->overlays_.find(q.symbol);
                if (it == self->overlays_.end() || !self->subscriptions_.contains(q.symbol)) {
                    self->tpoRequestCancel_.erase(q.requestId);
                    return;
                }
                if (it->second.generation != generation) {
                    if (q.kind != trade_overlay::Kind::Live && self->overlayHistory_.size() < 8)
                        self->overlayHistory_.push_front(q); // rebuilt against the latest selection
                    else self->tpoRequestCancel_.erase(q.requestId);
                    return;
                }
                if (!q.requestId.empty()) self->tpoRequestCancel_.erase(q.requestId);
                if (!result.error.empty()) {
                    sLog_DataN(5000, "Trade overlay not published: symbol=" << q.symbol << " error=" << result.error);
                    if (q.kind != trade_overlay::Kind::Live) self->send_overlay_error(q.symbol, q.requestId, result.error);
                    return;
                }
                it->second.request.grid = result.grid;
                if (q.kind == trade_overlay::Kind::Live) it->second.request.previousMs = q.nowMs;
                for (const auto& message : result.messages) self->do_write(message);
                sLog_Probe("overlay.publish", "symbol=" << q.symbol << " messages=" << result.messages.size()
                           << " tf=" << q.footprintMs << " tick=" << result.grid.tick);
            });
        });
        overlayBusy_ = queued;
        if (!queued && q.kind != trade_overlay::Kind::Live) {
            tpoRequestCancel_.erase(q.requestId);
            send_overlay_error(q.symbol, q.requestId, "overlay worker queue full");
        }
    }

    std::shared_ptr<recording::ChunkService> chunkService() const {
        return owner_ ? owner_->m_chunks : nullptr;
    }
    static std::string chunkEnvelope(uint64_t req, const std::vector<uint8_t>& body) {
        const auto wire = heatmap::encodeChunkEnvelope(req, body);
        return std::string(reinterpret_cast<const char*>(wire.data()), wire.size());
    }
    // Immediate refusals are small and go through the ordinary write limit.
    void sendChunkError(uint64_t req, const heatmap::ChunkKey& key, heatmap::ChunkError code,
                        const std::string& message) {
        sLog_Probe("chunks.refuse", "peer=" << peer_ << " req=" << req << " start=" << key.startMs
                   << " code=" << heatmap::chunkErrorName(code) << " message=" << message);
        do_write(chunkEnvelope(req, *recording::chunkErrorFrame(key, code, message)), true);
    }
    void handleChunkRequest(const nlohmann::json& j) {
        protocol::chunkwire::Request q;
        if (const auto error = protocol::chunkwire::parseRequest(j, q)) {
            sLog_Warning("Chunk request rejected: peer=" << peer_ << " req=" << q.req << " reason=" << *error);
            sendChunkError(q.req, {q.symbol, q.source, q.levelMs, 0}, heatmap::ChunkError::InvalidRequest, *error);
            return;
        }
        const auto service = chunkService();
        size_t admitted = 0;
        for (size_t i = 0; i < q.starts.size(); ++i) {
            const auto key = q.key(i);
            if (!service) {
                sendChunkError(q.req, key, heatmap::ChunkError::Unavailable, "recording chunks unavailable");
                continue;
            }
            if (chunkJobs_ >= kMaxChunkJobs || chunkBytes_ >= kMaxChunkBytes) {
                sendChunkError(q.req, key, heatmap::ChunkError::Busy,
                               "session chunk budget exhausted: jobs=" + std::to_string(chunkJobs_) +
                               " bytes=" + std::to_string(chunkBytes_));
                continue;
            }
            ++chunkJobs_;
            const bool queued = owner_->submitHistoryTask(
                [weak = weak_from_this(), executor = ws_.get_executor(), service, key,
                 have = q.haveHash[i], req = q.req] {
                    auto payload = chunkEnvelope(req, *service->serve(key, have));
                    net::post(executor, [weak, payload = std::move(payload)]() mutable {
                        if (auto self = weak.lock()) self->onChunkReply(std::move(payload));
                    });
                });
            if (!queued) {
                --chunkJobs_;
                sendChunkError(q.req, key, heatmap::ChunkError::Busy, "server chunk workers busy");
                continue;
            }
            ++admitted;
        }
        sLog_Probe("chunks.request", "peer=" << peer_ << " req=" << q.req << " symbol=" << q.symbol
                   << " source=" << q.source << " level=" << q.levelMs << " starts=" << q.starts.size()
                   << " admitted=" << admitted << " jobs=" << chunkJobs_ << " bytes=" << chunkBytes_);
    }
    // Executor: a worker finished one admitted job.
    void onChunkReply(std::string payload) {
        if (chunkJobs_ > 0) --chunkJobs_;
        if (closing_.load(std::memory_order_acquire)) return;
        chunkBytes_ += payload.size();
        write_queue_.push_back({std::move(payload), false, true, true});
        if (write_queue_.size() == 1) internal_async_write();
    }
    void pushAvailability(const std::string& symbol) {
        const auto service = chunkService();
        if (!service || closing_.load()) return;
        auto& state = availability_[symbol];
        if (state.busy) return;
        auto fingerprint = service->availabilityFingerprint(symbol);
        if (!state.sent.empty() && fingerprint == state.fingerprint) return;
        state.busy = true;
        const bool queued = owner_->submitHistoryTask(
            [weak = weak_from_this(), executor = ws_.get_executor(), service, symbol, fingerprint] {
                std::string payload;
                try {
                    payload = protocol::chunkwire::buildAvailability(symbol, service->availability(symbol)).dump();
                } catch (const std::exception& e) {
                    sLog_Warning("Heatmap availability failed: symbol=" << symbol << " error=" << e.what());
                }
                net::post(executor, [weak, symbol, fingerprint, payload = std::move(payload)]() mutable {
                    if (auto self = weak.lock()) self->onAvailability(symbol, std::move(fingerprint), std::move(payload));
                });
            });
        if (!queued) state.busy = false; // the availability timer retries
    }
    void onAvailability(const std::string& symbol, std::vector<int64_t> fingerprint, std::string payload) {
        if (closing_.load()) return;
        const auto it = availability_.find(symbol);
        if (it == availability_.end()) return; // unsubscribed meanwhile
        it->second.busy = false;
        if (payload.empty()) return;            // failed; the timer retries
        it->second.fingerprint = std::move(fingerprint);
        if (payload == it->second.sent) return;
        it->second.sent = payload;
        do_write(std::move(payload));
    }
    void armAvailabilityTimer() {
        if (closing_.load() || !chunkService()) return;
        availabilityTimer_.expires_after(std::chrono::seconds(1));
        availabilityTimer_.async_wait([weak = weak_from_this()](beast::error_code ec) {
            if (auto self = weak.lock(); self && !ec && !self->closing_.load()) {
                std::vector<std::string> symbols;
                for (const auto& [symbol, state] : self->availability_) symbols.push_back(symbol);
                for (const auto& symbol : symbols) self->pushAvailability(symbol);
                self->armAvailabilityTimer();
            }
        });
    }

public:
    explicit Session(tcp::socket&& socket, ssl::context& ctx,
                     ServerDataModel& model, SentinelStreamServer* owner)
        : ws_(std::move(socket), ctx)
        , model_(model)
        , owner_(owner)
    {
        beast::error_code ec;
        const auto endpoint = beast::get_lowest_layer(ws_).socket().remote_endpoint(ec);
        peer_ = ec ? std::string("unknown")
                   : endpoint.address().to_string() + ":" + std::to_string(endpoint.port());
    }

    ~Session() {
        if (recordingView_) recordingView_->active.store(false);
        for (const auto& [symbol, view] : rawViews_) view->active.store(false);
        disconnectModelSignals();
    }

    void run() {
        net::dispatch(ws_.get_executor(),
            beast::bind_front_handler(
                &Session::on_run,
                shared_from_this()));
    }

    void stop() {
        requestClose("server stopping");
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

        sLog_App("Sentinel client connected: peer=" << peer_);
        auto self = shared_from_this();
        auto weak = std::weak_ptr<Session>(self);

        if (owner_) {
            auto configPayload = buildServerConfigPayload(owner_->serverConfig(),
                                                          owner_->m_model.recordingAvailable(),
                                                          owner_->m_model.recordingLive() != nullptr);
            do_write(configPayload.dump());
        }
        
        tradeConn_ = QObject::connect(&model_, &ServerDataModel::tradeBroadcast, 
            [weak](const Trade& trade) {
                if (auto self = weak.lock()) {
                    self->postModelEvent([trade](Session& session) {
                        session.on_trade(trade);
                    });
                }
            });
            
        bookConn_ = QObject::connect(&model_, &ServerDataModel::bookUpdateBroadcast,
            [weak](const QString& productId, const std::vector<BookDelta>& deltas) {
                if (auto self = weak.lock()) {
                    self->postModelEvent([productId, deltas](Session& session) {
                        session.on_book_update(productId, deltas);
                    });
                }
            });

        heatmapConn_ = QObject::connect(&model_, &ServerDataModel::heatmapSliceReady,
            [weak](const HeatmapSlice& slice) {
                if (auto self = weak.lock()) {
                    self->postModelEvent([slice](Session& session) {
                        session.on_heatmap_slice(slice);
                    });
                }
            });

        barUpdatedConn_ = QObject::connect(&model_, &ServerDataModel::barUpdated,
            [weak](const QString& symbol, int64_t timeframeMs, const OHLCVBar& bar) {
                if (auto self = weak.lock()) {
                    self->postModelEvent([symbol, timeframeMs, bar](Session& session) {
                        session.on_bar_updated(symbol, timeframeMs, bar);
                    });
                }
            });

        barClosedConn_ = QObject::connect(&model_, &ServerDataModel::barClosed,
            [weak](const QString& symbol, int64_t timeframeMs, const OHLCVBar& bar) {
                if (auto self = weak.lock()) {
                    self->postModelEvent([symbol, timeframeMs, bar](Session& session) {
                        session.on_bar_closed(symbol, timeframeMs, bar);
                    });
                }
            });
        if (owner_) {
            m_latencySenderId = owner_->registerLatencySender(
                [weak](int ms) {
                    if (auto self = weak.lock()) {
                        self->postModelEvent([ms](Session& session) {
                            session.sendCoinbaseLatency(ms);
                        });
                    }
                });

            // Register for trading/algo broadcast messages
            m_tradingBroadcasterId = owner_->registerTradingBroadcaster(
                [weak](const std::string& json) {
                    if (auto self = weak.lock()) {
                        self->postModelEvent([json](Session& session) {
                            session.do_write(json);
                        });
                    }
                });
            m_tradingBroadcasterRegistered = true;
        }
        armOverlayTimer();
        armAvailabilityTimer();
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
            // beginClose logs the peer, reason and subscription count.
            beginClose("peer closed");
            return;
        }

        if(ec)
            return fail(ec, "read");

        if (ws_.got_binary()) {
            // Clients send JSON requests only; binary frames are server -> client.
            sLog_Warning("Ignoring binary frame from client: peer=" << peer_ << " bytes=" << buffer_.size());
            buffer_.consume(buffer_.size());
            do_read();
            return;
        }
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
                    std::transform(symbol.begin(), symbol.end(), symbol.begin(),
                                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
                    if (!subscriptions_.contains(symbol) && owner_ && !owner_->notifyClientSubscribed(symbol)) {
                        const int cap = owner_->serverConfig().mdc.maxConnections;
                        const std::string message = "Cannot subscribe to " + symbol + ": GUI connection cap (" +
                            std::to_string(cap) + ") reached. Close another symbol and retry.";
                        do_write(nlohmann::json{{"type", "error"}, {"context", "subscribe"},
                            {"code", "connection_cap"}, {"symbol", symbol}, {"max_connections", cap},
                            {"message", message}}.dump());
                        return;
                    }
                    const bool inserted = subscriptions_.insert(symbol).second;
                    sLog_Data("Client subscribe: peer=" << peer_ << " symbol=" << symbol
                              << " new=" << inserted << " subscriptions=" << subscriptions_.size());

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
                    // Availability is (re)sent on every subscribe, then on change.
                    availability_[symbol].sent.clear();
                    pushAvailability(symbol);
                }
            } else if (type == "heatmap_recording_view") {
                const auto view = protocol::recordingwire::parseView(j);
                recording::LiveView identity;
                identity.symbol = j.value("symbol", std::string{});
                identity.generation = j.value("band_generation", uint64_t{0});
                const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                if (!recordingRegistrationGate_.admit(now)) {
                    do_write(protocol::recordingwire::viewError(identity, "rate_limited", "recording registration rate exceeded").dump());
                    return;
                }
                if (!view || !model_.recordingLive()) {
                    do_write(protocol::recordingwire::viewError(identity,
                        view ? "unavailable" : "invalid_request", "invalid recording view or recording unavailable").dump());
                    return;
                }
                if (recordingView_) recordingView_->active.store(false);
                // The worker never locks a Session. All Session ownership and final
                // releases stay on its executor; shutdown joins the worker first.
                auto weak = weak_from_this();
                const auto executor = ws_.get_executor();
                recordingView_ = model_.recordingLive()->subscribe(*view,
                    [weak, executor, slot = recordingWriteSlot_](const recording::LiveView& v,
                                                               const recording::BuildResult& page) {
                        if (!slot->tryAcquire()) return false;
                        try {
                            auto payload = (page.status == recording::BuildStatus::Complete
                                ? protocol::recordingwire::buildLive(v, page)
                                : protocol::recordingwire::viewError(v, protocol::recordingwire::statusName(page.status),
                                                                    page.message)).dump();
                            if (payload.size() > 1024 * 1024) { slot->release(); return false; }
                            net::post(executor, [weak, slot, payload = std::move(payload)]() mutable {
                                if (auto self = weak.lock()) self->onRecordingWritePost(std::move(payload));
                                else slot->release();
                            });
                            return true;
                        } catch (...) { slot->release(); throw; }
                    });
                if (!recordingView_) do_write(protocol::recordingwire::viewError(*view,
                    "capacity", "recording live view capacity reached or service stopped").dump());
                sLog_Probe("recording.live.view", "symbol=" << view->symbol << " tf=" << view->tfMs
                    << " gen=" << view->generation << " layer=" << view->layer);
            } else if (type == "heatmap_recording_unview") {
                // The client muted its legacy band stream (GPU renderer, S6b).
                if (recordingView_) recordingView_->active.store(false);
                recordingView_.reset();
                sLog_Probe("recording.live.view", "released symbol=" << j.value("symbol", std::string{}));
            } else if (type == "heatmap_live_subscribe") {
                handleLiveSubscribe(j);
            } else if (type == "heatmap_live_unsubscribe") {
                if (!j.contains("symbol") || !j["symbol"].is_string()) {
                    send_error(type, "", "symbol is required");
                    return;
                }
                const auto symbol = j["symbol"].get<std::string>();
                if (const auto it = rawViews_.find(symbol); it != rawViews_.end()) {
                    it->second->active.store(false);
                    rawViews_.erase(it);
                    sLog_Data("Raw heatmap unsubscribe: peer=" << peer_ << " symbol=" << symbol);
                }
            } else if (type == "heatmap_chunk_request") {
                handleChunkRequest(j);
            } else if (type == "heatmap_history_request") {
                std::string symbol = j.value("symbol", "");
                const std::string source = j.value("source", std::string("legacy"));
                if (source == "recording") {
                    const std::string requestId = j.contains("request_id") && j["request_id"].is_string()
                        ? j["request_id"].get<std::string>() : "";
                    const uint64_t generation = j.contains("band_generation") && j["band_generation"].is_number_unsigned()
                        ? j["band_generation"].get<uint64_t>() : 0;
                    auto fail = [this, &symbol, &requestId, generation](const std::string& message) {
                        sLog_Warning("Recording history request rejected: symbol=" << symbol
                                     << " requestId=" << requestId << " reason=" << message);
                        do_write(protocol::recordingwire::error(symbol, message, requestId, generation).dump());
                    };
                    const auto q = protocol::recordingwire::parseRequest(j);
                    if (!q) { fail("invalid recording history request"); return; }
                    if (!owner_ || !owner_->m_model.recordingDir()) {
                        fail("recording unavailable"); return;
                    }
                    auto weak = weak_from_this();
                    auto* owner = owner_;
                    const auto root = *owner->m_model.recordingDir();
                    const bool queued = owner->submitHistoryTask([weak, q = *q, root] {
                        try {
                            auto request = protocol::recordingwire::buildRequest(q);
                            auto page = recording::buildPage(protocol::recordingwire::threadReader(root), request);
                            protocol::recordingwire::emptyPaddingValues(q, page);
                            auto response = protocol::recordingwire::buildChunk(q, page).dump();
                            if (auto self = weak.lock()) self->do_write(std::move(response));
                        } catch (const std::exception& ex) {
                            sLog_Error("Recording history build failed: symbol=" << q.symbol
                                       << " requestId=" << q.requestId << " error=" << ex.what());
                            if (auto self = weak.lock())
                                self->do_write(protocol::recordingwire::error(
                                    q.symbol, std::string("history build failed: ") + ex.what(),
                                    q.requestId, q.bandGeneration).dump());
                        }
                    });
                    if (!queued) fail("history worker queue is full");
                    return;
                }
                if (source != "legacy") {
                    const auto id = j.contains("request_id") && j["request_id"].is_string()
                        ? j["request_id"].get<std::string>() : "";
                    const auto generation = j.contains("band_generation") && j["band_generation"].is_number_unsigned()
                        ? j["band_generation"].get<uint64_t>() : 0;
                    do_write(protocol::recordingwire::error(symbol, "unsupported source", id, generation).dump());
                    return;
                }
                const int64_t timeframeMs = j.value("timeframe_ms", static_cast<int64_t>(0));
                const int64_t endTimeMs = j.value("end_time", static_cast<int64_t>(0));
                // Phase 4: optional start_time. 0 (or absent) means no lower bound.
                const int64_t startTimeMs = j.value("start_time", static_cast<int64_t>(0));
                const int requestedCount = j.value("count", 0);
                const int count = std::min(requestedCount,
                    protocol::SentinelProtocol::kMaxHeatmapHistoryColumns);
                if (!symbol.empty() && timeframeMs > 0 && count > 0) {
                    if (!owner_) {
                        send_error("heatmap_history_request", symbol, "server unavailable");
                        return;
                    }
                    auto weak = weak_from_this();
                    auto* owner = owner_;
                    const bool queued = owner->submitHistoryTask(
                        [weak, owner, symbol, timeframeMs, endTimeMs, startTimeMs, count] {
                            try {
                                auto response = owner->buildHeatmapHistoryChunk(
                                    symbol, timeframeMs, endTimeMs, startTimeMs, count);
                                if (auto self = weak.lock()) {
                                    self->do_write(std::move(response));
                                }
                            } catch (const std::exception& ex) {
                                if (auto self = weak.lock()) {
                                    self->send_error("heatmap_history_request", symbol,
                                                     std::string("history build failed: ") + ex.what());
                                }
                            }
                        });
                    if (!queued) {
                        // send_error logs the rejection.
                        send_error("heatmap_history_request", symbol,
                                   "history worker queue is full");
                    }
                } else {
                    sLog_Warning("Ignoring invalid heatmap_history_request: peer=" << peer_
                                 << " symbol=" << symbol << " tfMs=" << timeframeMs
                                 << " count=" << requestedCount);
                }
            } else if (type == "footprint_history_request") {
                requestOverlayHistory(j, false);
            } else if (type == "tpo_history_request") {
                requestOverlayHistory(j, true);
            } else if (type == "tpo_history_cancel") {
                cancelOverlayHistory(j);
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
                    // Page the retained 1s series, not just its newest `limit`
                    // bars; otherwise the second backward page appears empty.
                    const auto history = model_.getHistory(symbol, 1000, 10000);
                    std::vector<OHLCVBar> filtered;
                    filtered.reserve(history.size());
                    for (const auto& bar : history) {
                        if (bar.timestamp_ms <= endMs) {
                            filtered.push_back(bar);
                        }
                    }
                    // Existing contract: inclusive end and limit counts retained
                    // bars, not seconds. Sparse series may span a wider interval.
                    if (filtered.size() > static_cast<size_t>(limit))
                        filtered.erase(filtered.begin(), filtered.end() - limit);

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
                    sLog_Data("Candle history sent: symbol=" << symbol << " tfSec=1 source=memory"
                              << " endSec=" << endTimeSec << " limit=" << limit
                              << " candles=" << arr.size());
                    payload["candles"] = std::move(arr);
                    do_write(payload.dump());
                    return;
                }

                const int64_t timeframeMs = timeframeSec * 1000;
                if (timeframeMs < 60'000 || timeframeMs % 60'000 != 0 ||
                    std::find(owner_->serverConfig().heatmap.timeframesMs.begin(),
                              owner_->serverConfig().heatmap.timeframesMs.end(),
                              timeframeMs) == owner_->serverConfig().heatmap.timeframesMs.end()) {
                    send_error("candle_history_request", symbol, "unsupported timeframe_sec");
                    return;
                }

                const int64_t startTimeSec = endTimeSec - (timeframeSec * static_cast<int64_t>(limit));
                if (startTimeSec <= 0) {
                    send_error("candle_history_request", symbol, "invalid start time");
                    return;
                }

                auto self = shared_from_this();
                const bool queued = owner_->submitHistoryTask([self, symbol, timeframeSec, endTimeSec, startTimeSec, limit]() {
                    sentinel::logging::setCurrentThreadName("candle-fetch");
                    // Coinbase caps one request at 350 1m bars. Page the anchor,
                    // then use the same UTC rollup rule as live candle updates.
                    std::vector<OHLCVBar> minutes;
                    const int64_t firstMinuteSec = (startTimeSec / 60) * 60;
                    for (int64_t pageStart = firstMinuteSec; pageStart < endTimeSec;) {
                        if (self->closing_.load() || self->closePosted_.load() || !self->owner_->m_running.load()) return;
                        const int64_t pageEnd = std::min(pageStart + 350 * 60, endTimeSec);
                        auto page = self->owner_->restClient().fetchProductCandles(
                            symbol, pageStart, pageEnd, "ONE_MINUTE", 350);
                        if (self->closing_.load() || self->closePosted_.load() || !self->owner_->m_running.load()) return;
                        if (!page.ok) {
                            self->send_error("candle_history_request", symbol,
                                             std::string("fetch failed: ") + page.error);
                            return;
                        }
                        for (auto& minute : page.candles) {
                            if (minute.timestamp_ms >= pageStart * 1000 &&
                                minute.timestamp_ms < pageEnd * 1000)
                                minutes.push_back(std::move(minute));
                        }
                        pageStart = pageEnd;
                    }
                    std::sort(minutes.begin(), minutes.end(),
                              [](const OHLCVBar& a, const OHLCVBar& b) {
                                  return a.timestamp_ms < b.timestamp_ms;
                              });
                    minutes.erase(std::unique(minutes.begin(), minutes.end(),
                        [](const OHLCVBar& a, const OHLCVBar& b) {
                            return a.timestamp_ms == b.timestamp_ms;
                        }), minutes.end());
                    CandleFetchResult res;
                    res.ok = true;
                    res.candles = TimeframeAggregator::rollupMinutes(minutes, timeframeSec * 1000);
                    res.candles.erase(std::remove_if(res.candles.begin(), res.candles.end(),
                        [startTimeSec, endTimeSec](const OHLCVBar& bar) {
                            return bar.timestamp_ms < startTimeSec * 1000 ||
                                   bar.timestamp_ms > endTimeSec * 1000;
                        }), res.candles.end());
                    if (res.candles.size() > static_cast<size_t>(limit))
                        res.candles.erase(res.candles.begin(), res.candles.end() - limit);

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
                    sLog_Data("Candle history sent: symbol=" << symbol << " tfSec=" << timeframeSec
                              << " source=rest window=[" << startTimeSec << ".." << endTimeSec << "]"
                              << " limit=" << limit << " candles=" << arr.size());
                    payload["candles"] = std::move(arr);

                    self->do_write(payload.dump());
                });
                if (!queued) send_error("candle_history_request", symbol, "history worker queue is full");
            } else if (type == "trade_command") {
                if (owner_ && owner_->tradingSessionPtr()) {
                    try {
                        auto cmd = trading::parseTradeCommandJson(msg);
                        owner_->processTradeCommand(cmd);
                    } catch (const std::exception& ex) {
                        sLog_Error("trade_command parse error: peer=" << peer_ << " error=" << ex.what());
                    }
                }
            } else if (type == "algo_command") {
                if (owner_ && owner_->tradingSessionPtr()) {
                    try {
                        const std::string algoId = j.value("algo_id", "");
                        const std::string action  = j.value("action", "");
                        const std::string symbol  = j.value("symbol", "");
                        auto paramsJ = j.value("params", nlohmann::json::object());
                        trading::AlgoParams params;
                        params.spreadBps       = paramsJ.value("spread_bps", 10.0);
                        params.orderQty        = paramsJ.value("order_qty", 0.01);
                        params.maxPositionQty  = paramsJ.value("max_position_qty", 0.1);
                        params.skewBps         = paramsJ.value("skew_bps", 5.0);
                        if (action == "start") {
                            if (owner_->startAlgo(algoId, symbol, params)) {
                                sLog_App("LiveTradingSession: started algo=" << algoId << " symbol=" << symbol
                                         << " spreadBps=" << params.spreadBps << " orderQty=" << params.orderQty
                                         << " maxPositionQty=" << params.maxPositionQty
                                         << " skewBps=" << params.skewBps);
                            } else {
                                sLog_Warning("LiveTradingSession: start refused algo=" << algoId
                                             << " symbol=" << symbol);
                            }
                        } else if (action == "stop") {
                            owner_->stopAlgo(algoId);
                            sLog_App("LiveTradingSession: stopped algo=" << algoId);
                        } else {
                            sLog_Warning("algo_command ignored: unknown action=" << action
                                         << " algo=" << algoId);
                        }
                    } catch (const std::exception& ex) {
                        sLog_Error("algo_command error: peer=" << peer_ << " error=" << ex.what());
                    }
                }
            } else if (type == "unsubscribe") {
                 std::string symbol = j.value("symbol", "");
                 std::transform(symbol.begin(), symbol.end(), symbol.begin(),
                                [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
                 const bool removed = !symbol.empty() && subscriptions_.erase(symbol) > 0;
                 if (recordingView_ && recordingView_->view.symbol == symbol) {
                     recordingView_->active.store(false);
                     recordingView_.reset();
                 }
                 if (const auto it = rawViews_.find(symbol); it != rawViews_.end()) {
                     it->second->active.store(false);
                     rawViews_.erase(it);
                 }
                 if (const auto it = overlays_.find(symbol); it != overlays_.end())
                     it->second.cancelled->store(true);
                 overlays_.erase(symbol);
                 availability_.erase(symbol);
                 std::erase_if(overlayHistory_, [&](const auto& q) {
                     if (q.symbol != symbol) return false;
                     // Queued pages never reach pumpOverlays(); release their cancel flags here.
                     if (const auto c = tpoRequestCancel_.find(q.requestId); c != tpoRequestCancel_.end()) {
                         c->second->store(true);
                         tpoRequestCancel_.erase(c);
                     }
                     return true;
                 });
                 if (removed && owner_) {
                     owner_->notifyClientUnsubscribed(symbol);
                 }
                 sLog_Data("Client unsubscribe: peer=" << peer_ << " symbol=" << symbol
                           << " removed=" << removed << " subscriptions=" << subscriptions_.size());
            } else if (type == "screener_request") {
                const std::string asset   = j.value("asset", "crypto");
                const int         limit   = j.value("limit", 50);
                const double      minVol  = j.value("min_volume", 0.0);

                auto self = shared_from_this();
                const bool queued = owner_->submitHistoryTask([self, asset, limit, minVol]() {
                    const auto stopped = [&] {
                        return self->closing_.load() || self->closePosted_.load() || !self->owner_->m_running.load();
                    };
                    if (stopped()) return;
                    sentinel::logging::setCurrentThreadName("screener");
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

                    // Blocking QProcess APIs work without a Qt event loop. Keep
                    // the subprocess bounded/cancellable so joining cannot hang.
                    const auto process = runBoundedProcess("uv",
                        {"run", "python", scriptPath, "--asset", QString::fromStdString(asset),
                         "--limit", QString::number(limit), "--min-volume", QString::number(minVol)},
                        scriptsDir, stopped);
                    if (stopped()) return;
                    if (!process.error.empty()) {
                        self->send_error("screener_request", "", process.error);
                        return;
                    }
                    const auto& output = process.output;

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
                });
                if (!queued) send_error("screener_request", "", "history worker queue is full");
            }
        } catch (const std::exception& e) {
            sLog_Error("Server message parse error: peer=" << peer_ << " bytes=" << msg.size()
                       << " error=" << e.what());
        }
    }
    
    void on_trade(const Trade& trade) {
        // Tick live trading session on every trade (even unsubscribed symbols — algos may be running)
        if (owner_ && owner_->tradingSessionPtr()) {
            const int64_t tsMs = static_cast<int64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    trade.timestamp.time_since_epoch()).count());
            owner_->tradingSession().onTradeTick(trade.product_id, trade.price, tsMs);
        }
        if (subscriptions_.find(trade.product_id) == subscriptions_.end()) return;
        
        nlohmann::json j;
        j["type"] = "trade";
        j["product_id"] = trade.product_id;
        j["price"] = trade.price;
        j["size"] = trade.size;
        j["side"] = trade.side == AggressorSide::Buy ? "buy"
                    : trade.side == AggressorSide::Sell ? "sell" : "unknown";
        j["side_basis"] = "aggressor";
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

    void on_bar_updated(const QString& symbol, int64_t timeframeMs, const OHLCVBar& bar) {
        const std::string sym = symbol.toStdString();
        if (subscriptions_.find(sym) == subscriptions_.end()) return;

        const int64_t tfSec = (timeframeMs / 1000);
        const std::string key = sym + "|" + std::to_string(tfSec);
        const int64_t nowMs = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());

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
        sLog_Probe("candles.update",
                   "symbol=" << sym
                   << " tfSec=" << tfSec
                   << " start=" << bar.timestamp_ms
                   << " end=" << (bar.timestamp_ms + tfSec * 1000)
                   << " now=" << nowMs
                   << " seq=" << state.seq
                   << " closed=" << (bar.is_closed ? "true" : "false"));

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

    void on_bar_closed(const QString& symbol, int64_t timeframeMs, const OHLCVBar& bar) {
        const std::string sym = symbol.toStdString();
        if (subscriptions_.find(sym) == subscriptions_.end()) return;

        const int64_t tfSec = (timeframeMs / 1000);
        const std::string key = sym + "|" + std::to_string(tfSec);
        CandleStreamState state;
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
        sLog_Probe("candles.closed",
                   "symbol=" << sym
                   << " tfSec=" << tfSec
                   << " start=" << bar.timestamp_ms
                   << " end=" << (bar.timestamp_ms + tfSec * 1000)
                   << " now=" << state.lastSentMs
                   << " seq=" << state.seq);

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

    void handleLiveSubscribe(const nlohmann::json& j) {
        recording::RawTailView view;
        const auto error = protocol::chunkwire::parseLiveSubscribe(j, view);
        auto refuse = [&](heatmap::ChunkError code, const std::string& message) {
            heatmap::ChunkFrame frame;
            frame.kind = heatmap::ChunkKind::Error;
            frame.key.symbol = view.symbol.size() <= protocol::chunkwire::kMaxIdLength ? view.symbol : "";
            frame.error = code; frame.message = message;
            const auto wire = heatmap::encodeChunkEnvelope(view.sub, heatmap::encodeChunk(frame));
            do_write(std::string(wire.begin(), wire.end()), true);
        };
        if (error) { refuse(heatmap::ChunkError::InvalidRequest, *error); return; }
        if (!model_.recordingLive()) { refuse(heatmap::ChunkError::Unavailable, "recording live unavailable"); return; }
        const auto old = rawViews_.find(view.symbol);
        if (old == rawViews_.end() && rawViews_.size() >= protocol::chunkwire::kMaxLiveSymbols) {
            refuse(heatmap::ChunkError::Busy, "live symbol capacity reached"); return;
        }
        if (old != rawViews_.end()) { old->second->active.store(false); rawViews_.erase(old); }
        auto weak = weak_from_this();
        const auto executor = ws_.get_executor();
        // Filled on this executor before any posted delivery can execute. No
        // worker callback locks or owns a Session (same lifetime rule as legacy).
        auto token = std::make_shared<std::weak_ptr<recording::LiveService::RawSubscription>>();
        auto warning = std::make_shared<sentinel::log_throttle::Site>();
        auto subscription = model_.recordingLive()->subscribeRaw(view,
            [weak, executor, token, warning, slot = rawWriteSlot_](const recording::RawTailView& v,
                const std::string& source, const recording::RawTailFrame& frame) {
                if (!frame.bytes) return false;
                if (frame.bytes->size() > recording::kRawLiveByteBudget - 14) {
                    uint32_t suppressed = 0;
                    if (warning->admit(5000, sentinel::log_throttle::nowMs(), suppressed))
                        sLog_Warning("Raw heatmap live frame exceeds byte budget: symbol=" << v.symbol
                            << " source=" << source << " bytes=" << frame.bytes->size()+14
                            << sentinel::log_throttle::Suppressed{suppressed});
                    return false;
                }
                const auto bytes = frame.bytes->size()+14;
                if (!slot->tryAcquire(bytes)) return false;
                try {
                    // SHC1 is shared; SHE1 then the owning queue string still
                    // make two per-subscriber copies (bounded by the budget).
                    auto wire = heatmap::encodeChunkEnvelope(v.sub, *frame.bytes);
                    net::post(executor, [weak, slot, token, bytes, payload = std::string(wire.begin(), wire.end())]() mutable {
                        auto subscription = token->lock();
                        auto self = weak.lock();
                        if (!self || !subscription || !subscription->active.load() || self->closing_.load()) {
                            slot->release(bytes); return;
                        }
                        PendingWrite write{std::move(payload), false, true, false, true, subscription};
                        if (self->write_queue_.empty()) {
                            self->write_queue_.push_back(std::move(write));
                            self->internal_async_write();
                        } else {
                            // Prioritize live behind the in-flight write while
                            // retaining FIFO among all queued raw siblings.
                            auto position = std::next(self->write_queue_.begin());
                            for (auto it = position; it != self->write_queue_.end(); ++it)
                                if (it->rawLive) position = std::next(it);
                            self->write_queue_.insert(position, std::move(write));
                        }
                    });
                    return true;
                } catch (...) { slot->release(bytes); throw; }
            });
        if (!subscription) { refuse(heatmap::ChunkError::Busy, "raw live service capacity reached or stopped"); return; }
        *token = subscription;
        rawViews_[view.symbol] = std::move(subscription);
        sLog_Data("Raw heatmap subscribe: peer=" << peer_ << " symbol=" << view.symbol
            << " sub=" << view.sub << " sources=" << view.sources.size() << " since=" << view.sinceMs);
    }

    void onRecordingWritePost(std::string payload) {
        if (closing_.load()) { recordingWriteSlot_->release(); return; }
        // One independent <=1 MiB live payload; put it immediately behind the
        // write already in flight so history cannot indefinitely starve it.
        if (write_queue_.empty()) {
            write_queue_.push_back({std::move(payload), true});
            internal_async_write();
        } else write_queue_.insert(std::next(write_queue_.begin()), {std::move(payload), true});
    }

    void do_write(std::string payload, bool binary = false) {
        if (payload.empty() || closing_.load(std::memory_order_acquire)) {
            return;
        }

        const size_t bytes = payload.size();
        size_t pending = pendingWriteBytes_.load(std::memory_order_relaxed);
        while (true) {
            if (bytes > kMaxPendingWriteBytes || pending > kMaxPendingWriteBytes - bytes) {
                if (!closePosted_.load(std::memory_order_relaxed)) {
                    sLog_Warning("Closing slow client: peer=" << peer_ << " write backlog exceeded"
                                 << " pendingBytes=" << pending << " payloadBytes=" << bytes
                                 << " limit=" << kMaxPendingWriteBytes);
                }
                requestClose("write backlog exceeded");
                return;
            }
            if (pendingWriteBytes_.compare_exchange_weak(
                    pending, pending + bytes, std::memory_order_relaxed)) {
                break;
            }
        }

        net::post(ws_.get_executor(),
            beast::bind_front_handler(
                &Session::on_write_post,
                shared_from_this(),
                std::move(payload), binary));
    }

    void send_error(const std::string& context, const std::string& symbol, const std::string& message) {
        sLog_Warning("Sending error to client: peer=" << peer_ << " context=" << context
                     << " symbol=" << symbol << " message=" << message);
        nlohmann::json err;
        err["type"] = "error";
        err["context"] = context;
        if (!symbol.empty()) {
            err["symbol"] = symbol;
        }
        err["message"] = message;
        do_write(err.dump());
    }
    
    void on_write_post(std::string payload, bool binary) {
        if (closing_.load(std::memory_order_acquire)) {
            releasePendingWriteBytes(payload.size());
            return;
        }
        write_queue_.push_back({std::move(payload), false, binary});
        
        if (write_queue_.size() > 1) {
            return;
        }
        
        internal_async_write();
    }
    
    void internal_async_write() {
        // Unsubscribe/replacement cancels queued raw frames; the in-flight
        // Beast buffer remains owned until its completion callback.
        while (!write_queue_.empty() && write_queue_.front().rawLive) {
            const auto subscription = write_queue_.front().rawSubscription.lock();
            if (subscription && subscription->active.load()) break;
            releaseWrite(write_queue_.front());
            write_queue_.pop_front();
        }
        if (write_queue_.empty()) return;
        ws_.binary(write_queue_.front().binary);
        ws_.async_write(
            net::buffer(write_queue_.front().payload),
            beast::bind_front_handler(
                &Session::on_write_complete,
                shared_from_this()));
    }
    
    void on_write_complete(beast::error_code ec, std::size_t) {
        if (!write_queue_.empty()) {
            releaseWrite(write_queue_.front());
            write_queue_.pop_front();
        }
        if (ec) {
            return fail(ec, "write");
        }
        
        if (!write_queue_.empty()) {
            internal_async_write();
        }
    }

    void fail(beast::error_code ec, char const* what) {
        if (ec != websocket::error::closed && ec != net::error::operation_aborted) {
             sLog_Error("Session error: peer=" << peer_ << " op=" << what
                        << " error=" << ec.message().c_str());
        }
        beginClose(what);
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
    if (model.recordingDir()) {
        m_chunks = std::make_shared<recording::ChunkService>(
            *model.recordingDir(),
            [&model](const std::string& symbol, const std::string& layer) {
                return model.recordingWatermarks(symbol, layer);
            },
            [] {
                return std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
            });
    }
}

SentinelStreamServer::~SentinelStreamServer() {
    stop();
}

bool SentinelStreamServer::submitHistoryTask(std::function<void()> task) {
    if (!task || !m_running.load(std::memory_order_acquire)) {
        return false;
    }

    const size_t pending = m_pendingHistoryTasks.fetch_add(1, std::memory_order_acq_rel);
    if (pending >= kMaxPendingHistoryTasks) {
        m_pendingHistoryTasks.fetch_sub(1, std::memory_order_acq_rel);
        return false;
    }

    std::lock_guard<std::mutex> lock(m_historyWorkersMutex);
    if (!m_running.load(std::memory_order_acquire) || !m_historyWorkers) {
        m_pendingHistoryTasks.fetch_sub(1, std::memory_order_acq_rel);
        return false;
    }

    try {
        net::post(*m_historyWorkers, [this, task = std::move(task)]() mutable {
            try {
                task();
            } catch (const std::exception& ex) {
                sLog_Error("History worker task failed: " << ex.what());
            } catch (...) {
                sLog_Error("History worker task failed with an unknown exception");
            }
            m_pendingHistoryTasks.fetch_sub(1, std::memory_order_acq_rel);
        });
    } catch (...) {
        m_pendingHistoryTasks.fetch_sub(1, std::memory_order_acq_rel);
        return false;
    }
    return true;
}

std::string SentinelStreamServer::buildHeatmapHistoryChunk(const std::string& symbol,
                                                           int64_t timeframeMs,
                                                           int64_t endTimeMs,
                                                           int64_t startTimeMs,
                                                           int count) const {
    std::vector<HeatmapTwapStreamer::HistoryColumn> columns;
    int gridWidth = 0;
    int gridHeight = 0;
    const bool ok = m_model.getHeatmapHistory(symbol, timeframeMs, endTimeMs, count,
                                              gridWidth, gridHeight, columns, startTimeMs);

    nlohmann::json payload;
    payload["type"] = "heatmap_history_chunk";
    payload["schema_version"] = protocol::SentinelProtocol::kHeatmapSchemaVersion;
    payload["symbol"] = symbol;
    payload["timeframe_ms"] = timeframeMs;
    payload["request_end_time"] = endTimeMs;
    payload["grid_width"] = gridWidth;
    payload["grid_height"] = gridHeight;
    payload["format"] = "u16";
    payload["encoding"] = "base64";
    payload["liquidity_format"] = "u16";
    payload["liquidity_encoding"] = "base64";
    // 0 means no persisted data (or persistence disabled); the client should
    // treat the in-memory ring as the only source.
    payload["oldest_available_ms"] =
        m_model.oldestHeatmapPersistedMs(symbol, timeframeMs);
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
    sLog_Data("Heatmap history built: symbol=" << symbol << " tfMs=" << timeframeMs
              << " start=" << startTimeMs << " end=" << endTimeMs << " requested=" << count
              << " ok=" << ok << " columns=" << arr.size()
              << " grid=" << gridWidth << "x" << gridHeight
              << " oldestAvailable=" << payload["oldest_available_ms"].get<int64_t>());
    payload["columns"] = std::move(arr);
    return payload.dump();
}

void SentinelStreamServer::start() {
    if (m_running) return;

    try {
        m_ioc.restart();
        if (auto* live = m_model.recordingLive()) live->start();
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

        if (!m_tradingSession) {
            const double slippageBps = m_serverConfig.trading.slippageBps;
            m_tradingSession = std::make_unique<trading::LiveTradingSession>(
                [this](const std::string& symbol) -> double {
                    auto& hotData = m_model.ensureSymbol(symbol);
                    return resolveMidPrice(hotData.liveBook);
                },
                slippageBps);
            m_tradingSession->registerAlgo(std::make_unique<trading::AvendellaMM>());
            m_tradingSession->setResultCallback(
                [this](trading::TradingResult result, std::vector<trading::AlgoOrderEvent> events) {
                    for (auto& ou : result.orderUpdates) broadcastOrderUpdate(ou);
                    for (auto& pu : result.positionUpdates) broadcastPositionUpdate(pu);
                    for (auto& ru : result.riskOrderUpdates) broadcastRiskOrderUpdate(ru);
                    for (auto& ps : result.pnlSnapshots) broadcastPnlSnapshot(ps);
                    for (auto& ev : events) broadcastAlgoOrderEvent(ev);
                });
        }

        {
            std::lock_guard<std::mutex> lock(m_historyWorkersMutex);
            m_historyWorkers = std::make_unique<net::thread_pool>(kHistoryWorkerCount);
        }
        m_pendingHistoryTasks.store(0, std::memory_order_release);

        doAccept();
        sLog_App("SentinelStreamServer listening: port=" << m_port
                 << " cert=" << tls.certFile
                 << " historyWorkers=" << kHistoryWorkerCount);

        m_thread = std::thread([this] {
            sentinel::logging::setCurrentThreadName("stream-server");
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
        std::error_code fsError;
        const bool tlsFilesMissing =
            !std::filesystem::exists(m_serverConfig.tls.certFile, fsError) ||
            !std::filesystem::exists(m_serverConfig.tls.keyFile, fsError);
        sLog_Error("SentinelStreamServer start failed: port=" << m_port
                   << " cert=" << m_serverConfig.tls.certFile
                   << " key=" << m_serverConfig.tls.keyFile
                   << " error=" << e.what()
                   << (tlsFilesMissing
                           ? " (TLS files missing: run `bash certs/gen-certs.sh` from the repo root)"
                           : ""));
        m_running = false;
        std::unique_ptr<net::thread_pool> historyWorkers;
        {
            std::lock_guard<std::mutex> lock(m_historyWorkersMutex);
            historyWorkers = std::move(m_historyWorkers);
        }
        if (historyWorkers) {
            historyWorkers->stop();
            historyWorkers->join();
        }
        m_pendingHistoryTasks.store(0, std::memory_order_release);
    }
}

void SentinelStreamServer::stop() {
    // Live delivery can hold a Session and post to its executor. Join it while
    // that executor is alive, even when session drain will time out or start failed.
    if (auto* live = m_model.recordingLive()) live->shutdown();
    const bool wasRunning = m_running.exchange(false);
    if (!wasRunning && !m_thread.joinable()) {
        return;
    }

    net::post(m_ioc, [this] {
        if (m_acceptor) {
            beast::error_code ignored;
            m_acceptor->close(ignored);
        }
    });

    std::vector<std::shared_ptr<Session>> sessions;
    {
        std::lock_guard<std::mutex> lock(m_sessionsMutex);
        sessions.assign(m_sessions.begin(), m_sessions.end());
    }
    for (const auto& session : sessions) {
        session->stop();
    }

    std::unique_ptr<net::thread_pool> historyWorkers;
    {
        std::lock_guard<std::mutex> lock(m_historyWorkersMutex);
        historyWorkers = std::move(m_historyWorkers);
    }
    if (historyWorkers) {
        historyWorkers->stop();
        historyWorkers->join();
    }
    m_pendingHistoryTasks.store(0, std::memory_order_release);

    {
        std::unique_lock<std::mutex> lock(m_sessionsMutex);
        if (!m_sessionsDrained.wait_for(lock, std::chrono::seconds(2), [this] {
                return m_sessions.empty();
            })) {
            sLog_Warning("Timed out waiting for " << m_sessions.size()
                                                   << " Sentinel client session(s) to close");
        }
    }

    m_ioc.stop();
    if (m_thread.joinable()) {
        m_thread.join();
    }
    m_acceptor.reset();
}

void SentinelStreamServer::doAccept() {
    m_acceptor->async_accept(
        net::make_strand(m_ioc),
        [this](beast::error_code ec, tcp::socket socket) {
            if (!ec) {
                auto session = std::make_shared<Session>(std::move(socket), m_sslCtx, m_model, this);
                registerSession(session);
                session->run();
            } else if (m_running) {
                sLog_Error("Accept error: " << ec.message().c_str());
            }
            if (m_running) {
                doAccept();
            }
        });
}

size_t SentinelStreamServer::sessionCount() {
    std::lock_guard<std::mutex> lock(m_sessionsMutex);
    return m_sessions.size();
}

void SentinelStreamServer::registerSession(const std::shared_ptr<Session>& session) {
    std::lock_guard<std::mutex> lock(m_sessionsMutex);
    m_sessions.insert(session);
}

void SentinelStreamServer::unregisterSession(const Session* session) {
    {
        std::lock_guard<std::mutex> lock(m_sessionsMutex);
        std::erase_if(m_sessions, [session](const std::shared_ptr<Session>& candidate) {
            return candidate.get() == session;
        });
    }
    m_sessionsDrained.notify_all();
}

bool SentinelStreamServer::notifyClientSubscribed(const std::string& symbol) {
    bool firstSubscriber = false;
    {
        std::lock_guard<std::mutex> lock(m_symbolSubscriptionsMutex);
        const auto pinned = normalizedDefaultSymbols(m_serverConfig.defaultSymbols);
        const auto isPinned = [&](const auto& name) { return std::find(pinned.begin(), pinned.end(), name) != pinned.end(); };
        if (!m_symbolSubscriptions.contains(symbol) && !isPinned(symbol)) {
            const auto count = std::count_if(m_symbolSubscriptions.begin(), m_symbolSubscriptions.end(),
                [&](const auto& entry) { return !isPinned(entry.first); });
            if (count >= m_serverConfig.mdc.maxConnections) {
                uint64_t refusals = 1;
                const auto found = std::find_if(m_refusals.begin(), m_refusals.end(),
                    [&](const auto& entry) { return entry.first == symbol; });
                if (found != m_refusals.end()) { refusals += found->second; m_refusals.erase(found); }
                if (m_refusals.size() == 8) m_refusals.erase(m_refusals.begin());
                m_refusals.emplace_back(symbol, refusals);
                sLog_Error("Feed refused: symbol=" << symbol << " cap=" << m_serverConfig.mdc.maxConnections
                           << " code=connection_cap");
                return false;
            }
        }
        auto& count = m_symbolSubscriptions[symbol];
        firstSubscriber = count++ == 0;
    }
    if (firstSubscriber) emit clientSubscribed(QString::fromStdString(symbol));
    return true;
}

void SentinelStreamServer::registerMetrics(sentinel::metrics::MetricsRegistry& r) {
    using Registry = sentinel::metrics::MetricsRegistry;
    r.gaugeFn("sentinel_mdc_max_connections", "Maximum GUI-only product connections (pinned exempt).", {},
        [this]() -> std::optional<double> { return m_serverConfig.mdc.maxConnections; });
    r.familyFn("sentinel_mdc_connections", "Admitted product connections, including connecting feeds.", Registry::Type::Gauge,
        [this] {
            std::lock_guard lock(m_symbolSubscriptionsMutex);
            const auto pinned = normalizedDefaultSymbols(m_serverConfig.defaultSymbols);
            const auto gui = std::count_if(m_symbolSubscriptions.begin(), m_symbolSubscriptions.end(), [&](const auto& entry) {
                return std::find(pinned.begin(), pinned.end(), entry.first) == pinned.end();
            });
            return std::vector<Registry::Sample>{{{{"pinned", "1"}}, double(pinned.size())}, {{{"pinned", "0"}}, double(gui)}};
        });
    r.familyFn("sentinel_mdc_refused_total", "Refusals per product; eight most recently refused products, resets on eviction.",
        Registry::Type::Counter, [this] {
            std::lock_guard lock(m_symbolSubscriptionsMutex);
            std::vector<Registry::Sample> samples;
            for (const auto& [symbol, count] : m_refusals) samples.push_back({{{"product", symbol}}, double(count)});
            return samples;
        });
}

void SentinelStreamServer::notifyClientUnsubscribed(const std::string& symbol) {
    bool lastSubscriber = false;
    {
        std::lock_guard<std::mutex> lock(m_symbolSubscriptionsMutex);
        auto it = m_symbolSubscriptions.find(symbol);
        if (it == m_symbolSubscriptions.end()) {
            return;
        }
        if (--it->second == 0) {
            m_symbolSubscriptions.erase(it);
            lastSubscriber = true;
        }
    }
    if (lastSubscriber) {
        emit clientUnsubscribed(QString::fromStdString(symbol));
    }
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

// ─── Trading engine & AlgoEngine initialization ──────────────────────────────

void SentinelStreamServer::processTradeCommand(const trading::TradeCommand& command) {
    if (!m_tradingSession) return;
    m_tradingSession->processTradeCommand(command);
}

bool SentinelStreamServer::startAlgo(const std::string& algoId, const std::string& symbol, const trading::AlgoParams& params) {
    if (!m_tradingSession) {
        return false;
    }
    return m_tradingSession->startAlgo(algoId, symbol, params);
}

void SentinelStreamServer::stopAlgo(const std::string& algoId) {
    if (!m_tradingSession) {
        return;
    }
    m_tradingSession->stopAlgo(algoId);
}

uint64_t SentinelStreamServer::registerTradingBroadcaster(std::function<void(const std::string&)> fn) {
    const uint64_t id = m_nextBroadcasterId++;
        std::lock_guard<std::mutex> lock(m_tradingBroadcastMutex);
        m_tradingBroadcasters.emplace_back(id, std::move(fn));
    return id;
}

void SentinelStreamServer::unregisterTradingBroadcaster(uint64_t id) {
    std::lock_guard<std::mutex> lock(m_tradingBroadcastMutex);
    m_tradingBroadcasters.erase(
        std::remove_if(m_tradingBroadcasters.begin(), m_tradingBroadcasters.end(),
            [id](const std::pair<uint64_t, std::function<void(const std::string&)>>& p) { return p.first == id; }),
        m_tradingBroadcasters.end());
}

namespace {
void broadcastJson(std::mutex& mtx,
                   std::vector<std::pair<uint64_t, std::function<void(const std::string&)>>>& senders,
                   const std::string& json) {
    std::vector<std::function<void(const std::string&)>> copy;
    {
        std::lock_guard<std::mutex> lock(mtx);
        copy.reserve(senders.size());
        for (auto& p : senders) copy.push_back(p.second);
    }
    for (auto& fn : copy) fn(json);
}
} // namespace

void SentinelStreamServer::broadcastOrderUpdate(const trading::OrderUpdate& ou) {
    emit orderUpdateBroadcast(ou);
    nlohmann::json j;
    j["type"] = "order_update";
    j["order_id"] = ou.orderId;
    j["symbol"] = ou.symbol;
    j["status"] = trading::toString(ou.status);
    j["side"] = trading::toString(ou.side);
    j["qty"] = ou.qty;
    j["filled_qty"] = ou.filledQty;
    j["remaining_qty"] = ou.remainingQty;
    j["avg_price"] = ou.avgPrice;
    j["limit_price"] = ou.limitPrice;
    j["algo_id"] = ou.algoId;
    broadcastJson(m_tradingBroadcastMutex, m_tradingBroadcasters, j.dump());
}

void SentinelStreamServer::broadcastPositionUpdate(const trading::PositionUpdate& pu) {
    emit positionUpdateBroadcast(pu);
    nlohmann::json j;
    j["type"] = "position_update";
    j["symbol"] = pu.symbol;
    j["position_qty"] = pu.positionQty;
    j["avg_price"] = pu.avgPrice;
    j["unrealized_pnl"] = pu.unrealizedPnl;
    j["realized_pnl"] = pu.realizedPnl;
    broadcastJson(m_tradingBroadcastMutex, m_tradingBroadcasters, j.dump());
}

void SentinelStreamServer::broadcastRiskOrderUpdate(const trading::RiskOrderUpdate& ru) {
    emit riskOrderUpdateBroadcast(ru);
    nlohmann::json j;
    j["type"] = "risk_order_update";
    j["symbol"] = ru.symbol;
    j["has_take_profit"] = ru.hasTakeProfit;
    j["take_profit_price"] = ru.takeProfitPrice;
    j["has_stop_loss"] = ru.hasStopLoss;
    j["stop_loss_price"] = ru.stopLossPrice;
    broadcastJson(m_tradingBroadcastMutex, m_tradingBroadcasters, j.dump());
}

void SentinelStreamServer::broadcastAlgoOrderEvent(const trading::AlgoOrderEvent& ev) {
    emit algoOrderEventBroadcast(ev);
    nlohmann::json j;
    j["type"] = "algo_order_event";
    j["algo_id"] = ev.algoId;
    j["order_id"] = ev.orderId;
    j["symbol"] = ev.symbol;
    j["side"] = trading::toString(ev.side);
    j["order_type"] = trading::toString(ev.orderType);
    j["price"] = ev.price;
    j["qty"] = ev.qty;
    j["status"] = trading::toString(ev.status);
    j["timestamp_ms"] = ev.timestampMs;
    broadcastJson(m_tradingBroadcastMutex, m_tradingBroadcasters, j.dump());
}

void SentinelStreamServer::broadcastPnlSnapshot(const trading::PnlSnapshot& ps) {
    emit pnlSnapshotBroadcast(ps);
    nlohmann::json j;
    j["type"] = "pnl_snapshot";
    j["symbol"] = ps.symbol;
    j["timestamp_ms"] = ps.timestampMs;
    j["unrealized_pnl"] = ps.unrealizedPnl;
    j["realized_pnl"] = ps.realizedPnl;
    j["total_pnl"] = ps.totalPnl;
    j["algo_id"] = ps.algoId;
    broadcastJson(m_tradingBroadcastMutex, m_tradingBroadcasters, j.dump());
}
