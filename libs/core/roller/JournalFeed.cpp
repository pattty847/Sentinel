#include "JournalFeed.hpp"
#include "marketdata/dispatch/BookParser.hpp"
#include "marketdata/dispatch/MessageDispatcher.hpp"
#include <algorithm>
#include <charconv>

namespace sentinel::roller {
namespace {
std::optional<uint64_t> numericTradeId(const std::string& id) {
    uint64_t value = 0;
    const auto result = std::from_chars(id.data(), id.data() + id.size(), value);
    if (result.ec != std::errc{} || result.ptr != id.data() + id.size()) return {};
    return value;
}
}
bool JournalFeed::rememberTradeId(const std::string& id) {
    // Missing IDs cannot identify a repeat; opaque nonempty IDs still dedupe.
    if (id.empty()) return true;
    if (tradeIds_.contains(id)) return false;
    if (tradeIdRing_.empty()) {
        tradeIdRing_.reserve(TradeIdWindowCapacity);
        tradeIds_.reserve(TradeIdWindowCapacity);
    }
    if (tradeIdRing_.size() < TradeIdWindowCapacity) {
        tradeIds_.insert(id);
        tradeIdRing_.push_back(id);
    } else {
        // Reuse the evicted hash node: no node allocation after the window fills.
        auto node = tradeIds_.extract(tradeIdRing_[nextTradeId_]);
        node.value() = id;
        tradeIds_.insert(std::move(node));
        tradeIdRing_[nextTradeId_] = id;
        nextTradeId_ = (nextTradeId_ + 1) % TradeIdWindowCapacity;
    }
    return true;
}
void JournalFeed::invalid(int64_t local, const std::string& reason) {
    anchored_ = false;
    if (onInvalid) onInvalid(local, reason);
}
void JournalFeed::apply(const JournalRecord& input) {
    const auto& r = input.record;
    const auto local = r.time.systemNs / 1'000'000;
    using capture::Kind;
    if (input.gapBefore || run_ != input.pos.run || connection_ != r.connection) {
        invalid(lastLocal_ ? lastLocal_ : local, "journal boundary"); sequence_.reset();
        run_ = input.pos.run; connection_ = r.connection;
    }
    if (r.kind == Kind::TransportUp) {
        invalid(local, "transport up"); sequence_.reset();
        if (onConnection) onConnection(true);
    } else if (r.kind == Kind::TransportDown || r.kind == Kind::CaptureStopped) {
        invalid(local, "transport down/stop");
        if (onConnection) onConnection(false);
    } else if (r.kind == Kind::BookInvalidated || r.kind == Kind::ResyncRequested) {
        const auto j = nlohmann::json::parse(r.payload);
        const auto product = j.value("product", "");
        if (product.empty() || product == product_) invalid(local, j.value("reason", "journal invalidation"));
    } else if (r.kind == Kind::FrameReference) {
        // V2 receipts certify foreign sequence ranges, but never supply book state.
        const auto j = nlohmann::json::parse(r.payload);
        if (j.value("sequence_gaps", uint64_t{0})) invalid(local, "journal routing sequence gap");
    } else if (r.kind == Kind::Frame) {
        try {
            const auto j = nlohmann::json::parse(r.payload);
            // V2 deliberately omits foreign envelopes: only v1 may require +1.
            // Both reject backwards/duplicate sequence within a connection.
            if (j.contains("sequence_num")) {
                const auto& value = j.at("sequence_num");
                if (!value.is_number_integer() || value.get<int64_t>() < 0 || value.get<int64_t>() == INT64_MAX)
                    throw std::runtime_error("invalid journal sequence number");
                const auto seq = value.get<uint64_t>();
                const bool discontinuity = sequence_ &&
                    (seq <= *sequence_ || (input.version == 1 && seq != *sequence_ + 1));
                sequence_ = seq; // Re-anchor sequence tracking, but require a fresh book snapshot.
                if (discontinuity) throw std::runtime_error("journal sequence gap/regression");
            }
            const auto channel = j.value("channel", "");
            if (channel == "l2_data") {
                const int64_t envelope = j.contains("timestamp")
                    ? std::chrono::duration_cast<std::chrono::milliseconds>(Cpp20Utils::parseISO8601(j.at("timestamp").get<std::string>(), std::chrono::system_clock::time_point(std::chrono::milliseconds(local))).time_since_epoch()).count()
                    : local;
                if (!j.contains("events") || !j["events"].is_array()) throw std::runtime_error("missing L2 events");
                for (const auto& event : j["events"]) {
                    if (event.value("product_id", "") != product_) continue;
                    const auto type = event.value("type", "");
                    std::vector<recording::Level> levels;
                    if (type == "snapshot") {
                        std::vector<OrderBookLevel> bids, asks;
                        if (dispatch::parseSnapshot(event, bids, asks)) throw std::runtime_error("malformed snapshot");
                        levels.reserve(bids.size() + asks.size());
                        for (const auto& l : bids) levels.push_back({true,l.price,l.size});
                        for (const auto& l : asks) levels.push_back({false,l.price,l.size});
                        if (onSnapshot) {
                            try { onSnapshot(envelope, local, std::move(levels)); }
                            catch (const std::exception& e) { throw std::logic_error(e.what()); }
                        }
                        anchored_ = true;
                    } else if (type == "update") {
                        std::vector<BookLevelUpdate> updates;
                        if (!dispatch::parseUpdates(event, updates)) throw std::runtime_error("malformed update");
                        if (!anchored_ || updates.empty()) continue;
                        levels.reserve(updates.size());
                        for (const auto& l : updates) levels.push_back({l.isBid,l.price,l.quantity});
                        if (onUpdates) {
                            try { onUpdates(envelope, local, std::move(levels)); }
                            catch (const std::exception& e) { throw std::logic_error(e.what()); }
                        }
                    }
                }
            } else if (channel == "market_trades") {
                frameTrades_.clear();
                const auto collect = [&](const nlohmann::json& trades) {
                    if (!trades.is_array()) return;
                    for (const auto& raw : trades) {
                        auto trade = MessageDispatcher::parseTrade(raw,
                            std::chrono::system_clock::time_point(std::chrono::milliseconds(local)));
                        if (trade.product_id != product_ || !onTrade) continue;
                        const auto id = numericTradeId(trade.trade_id);
                        frameTrades_.push_back({std::move(trade), id, frameTrades_.size()});
                    }
                };
                if (j.contains("trades")) collect(j["trades"]);
                if (j.contains("events")) for (const auto& e : j["events"]) if (e.contains("trades")) collect(e["trades"]);
                // Original position makes the ordering stable without stable_sort's scratch allocation.
                std::sort(frameTrades_.begin(), frameTrades_.end(), [](const auto& a, const auto& b) {
                    if (a.trade.timestamp != b.trade.timestamp) return a.trade.timestamp < b.trade.timestamp;
                    // Numeric Coinbase IDs precede opaque IDs; keep a strict total order for mixed input.
                    if (a.numericId.has_value() != b.numericId.has_value()) return a.numericId.has_value();
                    if (a.numericId && b.numericId) {
                        if (*a.numericId != *b.numericId) return *a.numericId < *b.numericId;
                    }
                    if (a.trade.trade_id != b.trade.trade_id) return a.trade.trade_id < b.trade.trade_id;
                    return a.order < b.order;
                });
                for (auto& pending : frameTrades_) {
                    auto& trade = pending.trade;
                    if (!rememberTradeId(trade.trade_id)) continue;
                    // Coinbase market_trades reports the resting maker. Like the
                    // engine boundary, deliver the initiating aggressor side.
                    if (trade.side == AggressorSide::Buy) trade.side = AggressorSide::Sell;
                    else if (trade.side == AggressorSide::Sell) trade.side = AggressorSide::Buy;
                    try { onTrade(trade); }
                    catch (const std::exception& error) { throw std::logic_error(error.what()); }
                }
            }
        } catch (const nlohmann::json::exception&) { invalid(local, "malformed journal JSON"); }
          catch (const std::runtime_error& e) { invalid(local, e.what()); }
    }
    // Exactly one tick for every journal record, including receipts/lifecycle.
    if (onTick) onTick(local);
    lastLocal_ = local;
}
} // namespace sentinel::roller
