#include "ChunkClientWire.hpp"
#include "ChunkWire.hpp"
#include <set>
#include <stdexcept>

namespace protocol::chunks {
namespace {
void require(bool ok) { if (!ok) throw std::invalid_argument("invalid chunk control message"); }
int64_t integer(const nlohmann::json& v) {
    require(v.is_number_integer() && (!v.is_number_unsigned() || v.get<uint64_t>() <= uint64_t(INT64_MAX)));
    return v.get<int64_t>();
}
std::string hex(uint64_t v) {
    char s[17]; std::snprintf(s, sizeof s, "%016llx", static_cast<unsigned long long>(v)); return s;
}
}
bool Level::contains(int64_t start) const {
    const auto it = std::upper_bound(sealed.begin(), sealed.end(), start,
        [](int64_t t, const Run& r) { return t < r.fromMs; });
    return it != sealed.begin() && start < std::prev(it)->toMs && heatmap::isChunkStart(levelMs, start);
}
const Level* Availability::level(const Key& key) const {
    if (instrument != key.instrument) return nullptr;
    const auto c = channels.find(key.channel);
    if (c != channels.end()) for (const auto& l : c->second.levels) if (l.levelMs == key.levelMs) return &l;
    return nullptr;
}
bool Availability::contains(const Key& key) const { const auto* l = level(key); return l && l->contains(key.startMs); }
uint64_t Availability::epoch(const Key& key) const {
    const auto c = channels.find(key.channel);
    return instrument == key.instrument && c != channels.end() ? c->second.epoch : 0;
}
Key requestKey(Key key) { if (key.channel == "flow.rows") key.priceStep = 1; return key; }
bool matches(const Key& request, const Key& reply) { return validKey(reply) && request == requestKey(reply); }
nlohmann::json buildRequest(uint64_t id, const Key& key, const std::vector<int64_t>& starts,
                            const std::vector<std::optional<uint64_t>>& hashes) {
    require(id && !starts.empty() && starts.size() <= 64 && (hashes.empty() || hashes.size() == starts.size()));
    require(key.priceStep == (key.channel == "flow.rows" ? 1u : 0u));
    auto have = nlohmann::json::array();
    for (size_t i = 0; i < starts.size(); ++i) {
        auto k = key; k.startMs = starts[i]; require(validKey(k));
        have.push_back(!hashes.empty() && hashes[i] ? hex(*hashes[i]) : "");
    }
    return {{"type", "chunk_request"}, {"req", id}, {"instrument", key.instrument}, {"channel", key.channel},
        {"level_ms", key.levelMs}, {"price_step", key.priceStep}, {"starts", starts}, {"have_hash", have}};
}
nlohmann::json buildSubscribe(const std::vector<std::string>& ids) {
    require(ids.size() <= 8); std::set<std::string> distinct;
    for (const auto& id : ids) require(validInstrument(id) && distinct.insert(id).second);
    return {{"type", "chunk_availability_subscribe"}, {"instruments", ids}};
}
Availability parseAvailability(const nlohmann::json& j) {
    require(j.at("type") == "chunk_availability" && integer(j.at("wire_version")) == WireVersion);
    Availability a; a.instrument = j.at("instrument").get<std::string>(); require(validInstrument(a.instrument));
    a.liveFromMs = integer(j.at("live_from_ms")); require(a.liveFromMs >= 0);
    const auto& channels = j.at("channels"); require(channels.is_object() && channels.size() == 2);
    for (const auto* name : {"flow.summary", "flow.rows"}) {
        const auto& c = channels.at(name); Channel channel;
        const auto hash = c.at("epoch").get<std::string>(); require(hash.size() == 16);
        const auto parsed = chunkwire::parseHashHex(hash); require(parsed.has_value()); channel.epoch = *parsed;
        require(c.at("meta").is_object());
        const auto& levels = c.at("levels"); require(levels.is_array() && levels.size() == 3);
        std::set<int64_t> seen;
        for (const auto& v : levels) {
            Level l; l.levelMs = integer(v.at("level_ms")); l.chunkSpanMs = integer(v.at("chunk_span_ms"));
            require(l.chunkSpanMs > 0 && l.chunkSpanMs == heatmap::chunkSpanMs(l.levelMs) && seen.insert(l.levelMs).second);
            const auto& runs = v.at("sealed"); require(runs.is_array() && runs.size() <= 1'000'000);
            Key key{a.instrument, name, l.levelMs, std::string_view(name) == "flow.rows" ? 1u : 0u, 0};
            for (const auto& r : runs) {
                require(r.is_array() && r.size() == 2);
                Run run{integer(r[0]), integer(r[1])};
                key.startMs = run.fromMs; require(validKey(key) && run.toMs > run.fromMs);
                key.startMs = run.toMs - l.chunkSpanMs; require(validKey(key));
                require(l.sealed.empty() || l.sealed.back().toMs < run.fromMs);
                l.sealed.push_back(run);
            }
            if (!v.at("oldest_ms").is_null()) l.oldestMs = integer(v.at("oldest_ms"));
            if (!v.at("latest_ms").is_null()) l.latestMs = integer(v.at("latest_ms"));
            if (l.sealed.empty()) require(!l.oldestMs && !l.latestMs);
            else require(l.oldestMs == l.sealed.front().fromMs && l.latestMs == l.sealed.back().toMs - l.chunkSpanMs);
            channel.levels.push_back(std::move(l));
        }
        a.channels.emplace(name, std::move(channel));
    }
    require(a.channels.at("flow.rows").epoch == a.channels.at("flow.summary").epoch);
    return a;
}
} // namespace protocol::chunks
