// See KalshiDecoder.hpp for units and integrity rules. Decode a complete message
// before publishing; malformed snapshots never install a partial book. Consumer
// exceptions are handled by JournalFeed, outside this payload parser.
#include "KalshiDecoder.hpp"
#include <limits>
#include <QDateTime>

namespace sentinel::roller {
namespace {
using nlohmann::json;
int64_t integer(const json& j, int64_t maximum = INT64_MAX - 1) {
    if (!j.is_number_integer() || (j.is_number_unsigned() && j.get<uint64_t>() > INT64_MAX))
        throw std::runtime_error("malformed Kalshi integer");
    const auto n = j.get<int64_t>();
    if (n < 0 || n > maximum) throw std::runtime_error("malformed Kalshi integer");
    return n;
}
int64_t fixed(const json& j, size_t places, bool signedValue = false) {
    if (!j.is_string()) throw std::runtime_error("malformed Kalshi fixed point");
    const auto& s = j.get_ref<const std::string&>();
    const bool negative = signedValue && !s.empty() && s.front() == '-';
    const size_t start = negative ? 1 : 0;
    const auto dot = s.find('.');
    if (dot == std::string::npos || dot <= start || s.size() - dot - 1 != places)
        throw std::runtime_error("malformed Kalshi fixed point");
    int64_t n = 0;
    for (size_t i = start; i < s.size(); ++i) {
        if (i == dot) continue;
        if (s[i] < '0' || s[i] > '9' || n > (INT64_MAX - (s[i] - '0')) / 10)
            throw std::runtime_error("malformed/overflowing Kalshi fixed point");
        n = n * 10 + (s[i] - '0');
    }
    return negative ? -n : n;
}
int64_t price(const json& j) {
    const auto p = fixed(j, 4);
    if (p > 10000) throw std::runtime_error("Kalshi price outside [0,1]");
    return p;
}
// The sole integer-to-double conversion boundary, shared by prices and sizes.
double units(int64_t value, int64_t scale) {
    return static_cast<double>(value) / static_cast<double>(scale);
}
recording::Level level(bool bid, int64_t p, int64_t q) {
    return {bid, units(bid ? p : 10000 - p, 10000), units(q, 100)};
}
int64_t envelope(const json& frame, const json& msg, int64_t local) {
    constexpr auto maximum = INT64_MAX / 1'000'000; // Trade's system_clock nanoseconds.
    if (msg.contains("ts_ms")) return integer(msg.at("ts_ms"), maximum);
    if (msg.contains("ts")) {
        const auto& ts = msg.at("ts");
        if (ts.is_number_integer()) return integer(ts, maximum / 1000) * 1000;
        if (!ts.is_string()) throw std::runtime_error("malformed Kalshi timestamp");
        const auto date = QDateTime::fromString(QString::fromStdString(ts.get<std::string>()), Qt::ISODateWithMs);
        if (!date.isValid() || date.timeSpec() == Qt::LocalTime || date.toMSecsSinceEpoch() < 0 ||
            date.toMSecsSinceEpoch() > maximum) throw std::runtime_error("malformed Kalshi timestamp");
        return date.toMSecsSinceEpoch();
    }
    if (frame.contains("sending_ts_ms")) return integer(frame.at("sending_ts_ms"), maximum);
    return local;
}
std::map<int64_t, int64_t> snapshotSide(const json& msg, const char* name) {
    std::map<int64_t, int64_t> out;
    if (!msg.contains(name)) return out;
    const auto& rows = msg.at(name);
    if (!rows.is_array()) throw std::runtime_error("malformed Kalshi snapshot side");
    for (const auto& row : rows) {
        if (!row.is_array() || row.size() != 2) throw std::runtime_error("malformed Kalshi snapshot level");
        const auto p = price(row.at(0)), q = fixed(row.at(1), 2);
        if (!out.emplace(p, q).second) throw std::runtime_error("duplicate Kalshi snapshot price");
    }
    std::erase_if(out, [](const auto& row) { return row.second == 0; });
    return out;
}
AggressorSide aggressor(const json& msg) {
    std::optional<bool> buy;
    const auto read = [&](const char* field, const char* yes, const char* no) {
        if (!msg.contains(field)) return;
        const auto& value = msg.at(field);
        if (value != yes && value != no) throw std::runtime_error("malformed Kalshi trade side");
        const bool next = value == yes;
        if (buy && *buy != next) throw std::runtime_error("conflicting Kalshi trade sides");
        buy = next;
    };
    read("taker_outcome_side", "yes", "no");
    read("taker_book_side", "bid", "ask");
    if (!buy) read("taker_side", "yes", "no"); // Legacy outcome side, only without modern fields.
    if (!buy) throw std::runtime_error("missing Kalshi trade side");
    return *buy ? AggressorSide::Buy : AggressorSide::Sell;
}
}
void KalshiDecoder::advance(const json& frame) {
    const auto seq = integer(frame.at("seq"));
    const bool broken = sequence_ && seq != *sequence_ + 1;
    sequence_ = seq; // Like Coinbase, reject this frame and re-anchor the sequence.
    if (broken) throw std::runtime_error("Kalshi orderbook sequence gap/regression");
}
KalshiDecoder::Result KalshiDecoder::apply(const json& frame, const std::string& product, int64_t local) {
    const auto type = frame.at("type").get<std::string>();
    if (type == "subscribed") {
        const auto& msg = frame.at("msg");
        if (msg.at("channel") == "orderbook_delta") {
            const auto sid = integer(msg.at("sid"));
            if (sid_ != sid || retired_) {
                const bool hadBook = valid_;
                reset(); sid_ = sid;
                if (hadBook) throw std::runtime_error("Kalshi orderbook subscription changed");
            }
        }
        return {};
    }
    const bool book = type == "orderbook_snapshot" || type == "orderbook_delta";
    bool ours = false;
    if (book) {
        const auto ticker = frame.at("msg").at("market_ticker").get<std::string>();
        if (ticker.empty()) throw std::runtime_error("missing Kalshi market ticker");
        ours = ticker == product;
    }
    if (book || (frame.contains("sid") && (type == "ok" || type == "unsubscribed" || type == "error"))) {
        const auto sid = integer(frame.at("sid"));
        if (ours && type == "orderbook_snapshot") {
            // A selected-market snapshot is authoritative, including a new sid,
            // a sequence discontinuity, or recovery after subscription retirement.
            sequence_ = integer(frame.at("seq"));
            sid_ = sid; retired_ = false;
        } else {
            if (!sid_ && book) sid_ = sid;
            if (sid_ != sid) {
                if (ours && valid_) throw std::runtime_error("Kalshi orderbook subscription changed");
                return {};
            }
            if (retired_) return {};
            if (book || frame.contains("seq")) advance(frame);
        }
        if (type == "unsubscribed") {
            retired_ = true;
            throw std::runtime_error("Kalshi orderbook unsubscribed");
        }
        if (type == "error") {
            const auto code = integer(frame.at("msg").at("code"));
            if (code == 7 || code == 10 || code == 25) {
                retired_ = true;
                throw std::runtime_error("Kalshi orderbook subscription error");
            }
        }
    }
    if (!book && type != "trade") return {};
    const auto& msg = frame.at("msg");
    if (book) {
        if (!ours) return {}; // Foreign books consumed the current sid sequence above.
        if (type == "orderbook_delta" && !valid_) return {}; // Await an authoritative snapshot.
    } else if (msg.at("market_ticker").get<std::string>() != product) return {};
    Result out;
    out.envelopeMs = envelope(frame, msg, local);
    if (type == "trade") {
        const auto p = price(msg.at("yes_price_dollars")), q = fixed(msg.at("count_fp"), 2);
        const auto side = aggressor(msg);
        const auto id = msg.at("trade_id").get<std::string>();
        if (q == 0 || id.empty()) throw std::runtime_error("malformed Kalshi trade");
        out.kind = Result::Kind::Trade;
        out.trade = ::Trade{std::chrono::system_clock::time_point(std::chrono::milliseconds(out.envelopeMs)),
                            product, id, side, units(p, 10000), units(q, 100)};
    } else if (type == "orderbook_snapshot") {
        auto yes = snapshotSide(msg, "yes_dollars_fp"), no = snapshotSide(msg, "no_dollars_fp");
        out.kind = Result::Kind::Snapshot;
        out.levels.reserve(yes.size() + no.size());
        for (const auto& [p, q] : yes) out.levels.push_back(level(true, p, q));
        for (const auto& [p, q] : no) out.levels.push_back(level(false, p, q));
        yes_ = std::move(yes); no_ = std::move(no); valid_ = true;
    } else {
        const auto p = price(msg.at("price_dollars")), delta = fixed(msg.at("delta_fp"), 2, true);
        const auto side = msg.at("side").get<std::string>();
        if (side != "yes" && side != "no") throw std::runtime_error("malformed Kalshi book side");
        auto& levels = side == "yes" ? yes_ : no_;
        const auto it = levels.find(p);
        const auto old = it == levels.end() ? 0 : it->second;
        if ((delta > 0 && old > INT64_MAX - delta) || (delta < 0 && old < -delta))
            throw std::runtime_error("Kalshi negative/overflowing level quantity");
        const auto q = old + delta;
        if (q) levels[p] = q; else levels.erase(p);
        out.kind = Result::Kind::Update;
        out.levels.push_back(level(side == "yes", p, q));
    }
    return out;
}
json KalshiDecoder::exportState() const {
    json j = {{"version", 1}, {"sid", sid_ ? json(*sid_) : json()},
              {"sequence", sequence_ ? json(*sequence_) : json()}, {"valid", valid_},
              {"retired", retired_}, {"yes", json::array()}, {"no", json::array()}};
    for (const auto& [p, q] : yes_) j["yes"].push_back({p, q});
    for (const auto& [p, q] : no_) j["no"].push_back({p, q});
    return j;
}
KalshiDecoder KalshiDecoder::fromState(const json& j) {
    if (j.at("version") != 1) throw std::runtime_error("Kalshi decoder state version");
    KalshiDecoder out;
    if (!j.at("sid").is_null()) out.sid_ = integer(j.at("sid"));
    if (!j.at("sequence").is_null()) out.sequence_ = integer(j.at("sequence"));
    out.valid_ = j.at("valid").get<bool>(); out.retired_ = j.at("retired").get<bool>();
    for (const auto* name : {"yes", "no"}) {
        auto& levels = std::string_view(name) == "yes" ? out.yes_ : out.no_;
        const auto& rows = j.at(name);
        if (!rows.is_array() || rows.size() > 10001) throw std::runtime_error("Kalshi state levels");
        for (const auto& row : rows) {
            if (!row.is_array() || row.size() != 2) throw std::runtime_error("Kalshi state level");
            const auto p = integer(row.at(0)), q = integer(row.at(1), INT64_MAX);
            if (p > 10000 || q == 0 || !levels.emplace(p, q).second)
                throw std::runtime_error("Kalshi state price/quantity");
        }
    }
    if ((out.sequence_ && !out.sid_) || (out.retired_ && !out.sid_) ||
        (out.valid_ && (!out.sid_ || !out.sequence_ || out.retired_)) ||
        (!out.valid_ && (!out.yes_.empty() || !out.no_.empty())))
        throw std::runtime_error("Kalshi state validity");
    return out;
}
} // namespace sentinel::roller
