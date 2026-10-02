#include "CaptureRouting.hpp"
#include <algorithm>
#include <stdexcept>
#include <set>

namespace sentinel::capture {
namespace {
// Retains only routing scalars. In particular, updates and prices/quantities are
// tokenized for JSON validity but never materialized as a DOM.
struct RouteSax : nlohmann::json_sax<nlohmann::json> {
    enum Role { Root, Events, Event, Trades, Trade, Ignore };
    struct Scope { Role role; std::string key; bool product = false, hasTrades = false; };
    std::vector<Scope> stack;
    const std::vector<std::string>& products;
    std::vector<std::string> l2, trades;
    std::string channel;
    nlohmann::json sequence = nullptr;
    bool valid = true, events = false, sawChannel = false, sawSequence = false;
    bool l2Valid = true, tradesValid = true;
    const std::string* tradeSymbol = nullptr;
    std::vector<CapturedTradeEvent> tradeEvents;
    CapturedTradeEvent tradeEvent;
    CapturedTrade trade;
    std::string tradeProduct;
    std::set<std::string> tradeKeys, eventKeys;
    bool extractionValid = true, stopAtChannel = false;
    explicit RouteSax(const std::vector<std::string>& p) : products(p) { stack.reserve(16); }
    bool scalar(const nlohmann::json& value) {
        if (stack.empty()) { valid = false; return true; }
        auto& scope = stack.back();
        if (tradeSymbol && scope.role == Event && scope.key == "type") {
            if (!value.is_string()) extractionValid = false;
            else tradeEvent.type = value.get<std::string>();
        }
        if (tradeSymbol && scope.role == Trade) {
            std::string* field = scope.key == "trade_id" ? &trade.id : scope.key == "size" ? &trade.size :
                scope.key == "side" ? &trade.side : scope.key == "time" ? &trade.time :
                scope.key == "product_id" ? &tradeProduct : nullptr;
            if (field) {
                if (!value.is_string()) extractionValid = false;
                else *field = value.get<std::string>();
            }
        }
        if (scope.role == Events || scope.role == Trades) valid = false;
        if (scope.role == Root && scope.key == "channel") {
            if (sawChannel || !value.is_string()) valid = false;
            else channel = value.get<std::string>();
            sawChannel = true;
            if (stopAtChannel) return false;
        } else if (scope.role == Root && scope.key == "sequence_num") {
            if (sawSequence) valid = false;
            sequence = value; sawSequence = true;
        } else if (scope.role == Root && scope.key == "events") valid = false;
        else if ((scope.role == Event || scope.role == Trade) && scope.key == "product_id") {
            auto& ids = scope.role == Event ? l2 : trades;
            auto& ok = scope.role == Event ? l2Valid : tradesValid;
            if (scope.product || !value.is_string()) ok = false;
            else {
                const auto id = value.get<std::string>();
                if (!std::binary_search(products.begin(), products.end(), id)) ok = false;
                else if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
            }
            scope.product = true;
        } else if (scope.role == Event && scope.key == "trades") tradesValid = false;
        return true;
    }
    bool null() override { return scalar(nullptr); }
    bool boolean(bool v) override { return scalar(v); }
    bool number_integer(number_integer_t v) override { return scalar(v); }
    bool number_unsigned(number_unsigned_t v) override { return scalar(v); }
    bool number_float(number_float_t v, const string_t&) override { return scalar(v); }
    bool string(string_t& v) override {
        // Avoid a JSON/string copy for each price, quantity or ignored value.
        if (!stack.empty() && stack.back().role == Ignore) return true;
        return scalar(v);
    }
    bool binary(binary_t&) override { valid = false; return true; }
    bool key(string_t& v) override {
        if (tradeSymbol && !stack.empty()) {
            const auto role = stack.back().role;
            if (role == Trade && !tradeKeys.insert(v).second) extractionValid = false;
            if (role == Event && !eventKeys.insert(v).second) extractionValid = false;
        }
        if (!stack.empty() && stack.back().role != Ignore) stack.back().key = v;
        return true;
    }
    bool start(bool object) {
        if (stack.size() >= 128) return false;
        Role role = Ignore;
        if (stack.empty()) { role = Root; if (!object) valid = false; }
        else {
            auto& parent = stack.back();
            if (parent.role == Root && parent.key == "events") {
                if (events || object) valid = false;
                events = true; role = object ? Ignore : Events;
            } else if (parent.role == Events) { role = object ? Event : Ignore; if (!object) valid = false; }
            else if (parent.role == Event && parent.key == "trades") {
                if (object || parent.hasTrades) tradesValid = false;
                parent.hasTrades = true;
                role = object ? Ignore : Trades;
            } else if (parent.role == Trades) { role = object ? Trade : Ignore; if (!object) tradesValid = false; }
            else if ((parent.role == Root && (parent.key == "channel" || parent.key == "sequence_num")) ||
                     ((parent.role == Event || parent.role == Trade) && parent.key == "product_id")) valid = false;
        }
        if (tradeSymbol) {
            if (role == Event) { tradeEvent = {}; eventKeys.clear(); }
            if (role == Trade) { trade = {}; tradeProduct.clear(); tradeKeys.clear(); }
            if (!stack.empty() && stack.back().role == Trade) extractionValid = false;
        }
        stack.push_back({role, {}}); return true;
    }
    bool start_object(std::size_t) override { return start(true); }
    bool start_array(std::size_t) override { return start(false); }
    bool end() {
        if (stack.back().role == Event && !stack.back().product) l2Valid = false;
        if (stack.back().role == Event && !stack.back().hasTrades) tradesValid = false;
        if (stack.back().role == Trade && !stack.back().product) tradesValid = false;
        if (tradeSymbol && stack.back().role == Trade) {
            if (trade.id.empty() || trade.size.empty() || trade.side.empty() || trade.time.empty()) extractionValid = false;
            if (tradeProduct == *tradeSymbol) tradeEvent.trades.push_back(std::move(trade));
        }
        if (tradeSymbol && stack.back().role == Event) {
            if (tradeEvent.type != "snapshot" && tradeEvent.type != "update") extractionValid = false;
            tradeEvents.push_back(std::move(tradeEvent));
        }
        stack.pop_back(); return true;
    }
    bool end_object() override { return end(); }
    bool end_array() override { return end(); }
    bool parse_error(std::size_t, const std::string&, const nlohmann::detail::exception&) override { return false; }
};
}
std::vector<CapturedTradeEvent> parseTradeEvents(std::string_view payload,
    const std::vector<std::string>& products, const std::string& symbol, nlohmann::json* envelope) {
    RouteSax sax(products);
    sax.tradeSymbol = &symbol;
    if (!nlohmann::json::sax_parse(payload.begin(), payload.end(), &sax) || !sax.valid ||
        !sax.events || !sax.tradesValid || !sax.extractionValid || sax.channel != "market_trades")
        throw std::runtime_error("invalid market_trades envelope");
    if (envelope) *envelope = {{"channel", sax.channel}, {"sequence_num", sax.sequence}};
    return std::move(sax.tradeEvents);
}
std::optional<std::string> peekFrameChannel(std::string_view payload) {
    const std::vector<std::string> products;
    RouteSax sax(products);
    sax.stopAtChannel = true;
    nlohmann::json::sax_parse(payload.begin(), payload.end(), &sax);
    if (sax.sawChannel && sax.valid) return sax.channel;
    return std::nullopt;
}
nlohmann::json frameReceipt(std::string_view payload, const std::vector<std::string>& products) {
    RouteSax sax(products);
    const bool parsed = nlohmann::json::sax_parse(payload.begin(), payload.end(), &sax);
    std::vector<std::string> targets;
    if (parsed && sax.valid && sax.events) {
        if (sax.channel == "l2_data" && sax.l2Valid) targets = sax.l2;
        if (sax.channel == "market_trades" && sax.tradesValid) targets = sax.trades;
    }
    if (targets.empty()) targets = products;
    std::sort(targets.begin(), targets.end());
    const auto hash = QCryptographicHash::hash(QByteArrayView(payload.data(), payload.size()), QCryptographicHash::Sha256);
    return {{"products", targets}, {"channel", parsed ? sax.channel : "<invalid-envelope>"},
        {"sequence_num", parsed && sax.valid ? sax.sequence : nlohmann::json(nullptr)},
        {"received_bytes", payload.size()}, {"sha256", hash.toHex().toStdString()}};
}
std::string frameIdentityLine(const Record& r, const nlohmann::json& identity) {
    return nlohmann::json::array({r.time.systemNs, r.time.steadyNs, r.connection, identity}).dump() + "\n";
}
void RoutingBatch::add(const Record& r, const nlohmann::json& identity) {
    const auto& seq = identity.at("sequence_num");
    const bool sequenced = seq.is_number_unsigned() || (seq.is_number_integer() && seq.get<int64_t>() >= 0);
    const auto value = sequenced ? seq.get<uint64_t>() : 0;
    if (!count) { first = r.time; connection = r.connection; firstSeq = value; }
    else if (lastSeq == UINT64_MAX || value != lastSeq + 1) ++gaps;
    if (!sequenced || value == UINT64_MAX) ++gaps;
    const auto size = identity.at("received_bytes").get<uint64_t>();
    last = r.time; lastSeq = value; ++count; bytes += size;
    const auto targets = identity.at("products").get<std::vector<std::string>>();
    for (const auto& symbol : targets) { ++own[symbol].frames; own[symbol].bytes += size; }
    // Every foreign frame has exactly one canonical raw-copy owner.
    ++destinations[targets.front()].frames; destinations[targets.front()].bytes += size;
    const auto line = frameIdentityLine(r, identity);
    digest.addData(QByteArrayView(line.data(), line.size()));
}
nlohmann::json RoutingBatch::receipt(const std::string& symbol) const {
    const auto found = own.find(symbol);
    const auto local = found == own.end() ? Counts{} : found->second;
    nlohmann::json owners = nlohmann::json::object();
    for (const auto& [id, value] : destinations) owners[id] = {value.frames, value.bytes};
    return {{"first_seq", firstSeq}, {"last_seq", lastSeq}, {"count", count}, {"bytes", bytes},
        {"foreign_count", count - local.frames}, {"foreign_bytes", bytes - local.bytes},
        {"destinations", owners}, {"sequence_gaps", gaps},
        {"sha256", digest.result().toHex().toStdString()}};
}
void RoutingBatch::clear() {
    count = bytes = firstSeq = lastSeq = gaps = connection = 0;
    own.clear(); destinations.clear(); digest.reset(); first = last = {};
}
void validateRange(const nlohmann::json& r, const std::vector<std::string>& products) {
    for (const char* key : {"first_seq", "last_seq", "count", "bytes", "foreign_count", "foreign_bytes", "sequence_gaps"})
        if (!r.at(key).is_number_unsigned()) throw std::runtime_error("invalid range integer");
    const auto count = r.at("count").get<uint64_t>();
    if (!count || count > MaxRoutingFrames || r.at("bytes").get<uint64_t>() > MaxRoutingFrames * MaxRecordBytes ||
        r.at("foreign_count").get<uint64_t>() > count || r.at("foreign_bytes") > r.at("bytes"))
        throw std::runtime_error("invalid range counts");
    const auto hash = r.at("sha256").get<std::string>();
    if (hash.size() != 64 || hash.find_first_not_of("0123456789abcdef") != std::string::npos)
        throw std::runtime_error("invalid range digest");
    uint64_t frames = 0, bytes = 0;
    for (const auto& [symbol, value] : r.at("destinations").items()) {
        if (!std::binary_search(products.begin(), products.end(), symbol) || !value.is_array() || value.size() != 2 ||
            !value[0].is_number_unsigned() || !value[1].is_number_unsigned() || value[0].get<uint64_t>() > count ||
            value[1].get<uint64_t>() > MaxRoutingFrames * MaxRecordBytes) throw std::runtime_error("invalid range destinations");
        frames += value[0].get<uint64_t>(); bytes += value[1].get<uint64_t>();
    }
    if (frames != count || bytes != r.at("bytes")) throw std::runtime_error("range destination totals differ");
}
}
