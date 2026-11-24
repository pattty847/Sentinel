#pragma once
// JSON control plane for sealed flow chunks. No disk I/O or model access here.
#include "ChunkWire.hpp"
#include "ChunkFrame.hpp"
#include "servermodel/FlowChunkStore.hpp"
#include <limits>

namespace protocol::flowchunks {
inline constexpr size_t MaxInstruments = 8;
struct Request {
    uint64_t req = 0;
    chunks::Key key;
    std::vector<int64_t> starts;
    std::vector<std::optional<uint64_t>> have;
    chunks::Key at(size_t n) const { auto result = key; result.startMs = starts[n]; return result; }
};
inline bool signedInteger(const nlohmann::json& value) {
    return value.is_number_integer() && (!value.is_number_unsigned() || value.get<uint64_t>() <= uint64_t(INT64_MAX));
}
inline std::optional<std::string> parseRequest(const nlohmann::json& j, Request& q) {
    if (j.contains("req") && j["req"].is_number_unsigned()) q.req = j["req"].get<uint64_t>();
    if (j.contains("instrument") && j["instrument"].is_string()) q.key.instrument = j["instrument"].get<std::string>();
    if (j.contains("channel") && j["channel"].is_string()) q.key.channel = j["channel"].get<std::string>();
    if (!j.contains("req") || !j["req"].is_number_unsigned()) return "req must be unsigned";
    if (!chunks::validInstrument(q.key.instrument)) return "invalid instrument";
    if (q.key.channel != "flow.summary" && q.key.channel != "flow.rows") return "unsupported channel";
    if (!j.contains("level_ms") || !signedInteger(j["level_ms"])) return "level_ms must be an integer";
    q.key.levelMs = j["level_ms"].get<int64_t>();
    if (!heatmap::chunkSpanMs(q.key.levelMs)) return "unsupported level_ms";
    if (!j.contains("price_step") || !signedInteger(j["price_step"]) ||
        j["price_step"] != (q.key.channel == "flow.rows" ? 1 : 0)) return "invalid price_step";
    q.key.priceStep = j["price_step"].get<uint64_t>();
    if (!j.contains("starts") || !j["starts"].is_array() || j["starts"].empty() || j["starts"].size() > chunkwire::kMaxStarts)
        return "starts must hold 1..64 aligned starts";
    for (const auto& v : j["starts"]) {
        if (!signedInteger(v)) return "starts must be bounded integers";
        auto key = q.key; key.startMs = v.get<int64_t>();
        if (!chunks::validKey(key)) return "invalid or unaligned start";
        q.starts.push_back(key.startMs);
    }
    q.have.resize(q.starts.size());
    if (j.contains("have_hash")) {
        const auto& hashes = j["have_hash"];
        if (!hashes.is_array() || hashes.size() != q.starts.size()) return "have_hash must parallel starts";
        for (size_t n = 0; n < hashes.size(); ++n) {
            if (!hashes[n].is_string()) return "invalid have_hash";
            const auto text = hashes[n].get<std::string>();
            const auto parsed = chunkwire::parseHashHex(text);
            if (!parsed) return "have_hash entries are 16 hex digits or empty";
            if (!text.empty()) q.have[n] = *parsed;
        }
    }
    return {};
}
// A full replacement, including an empty list to unsubscribe everything.
inline std::optional<std::string> parseSubscribe(const nlohmann::json& j, std::vector<std::string>& ids) {
    if (!j.contains("instruments") || !j["instruments"].is_array() || j["instruments"].size() > MaxInstruments)
        return "instruments must hold 0..8 distinct instrument ids";
    for (const auto& value : j["instruments"]) {
        if (!value.is_string()) return "invalid instrument";
        auto id = value.get<std::string>();
        if (!chunks::validInstrument(id) || std::find(ids.begin(), ids.end(), id) != ids.end()) return "invalid or duplicate instrument";
        ids.push_back(std::move(id));
    }
    return {};
}
inline nlohmann::json availability(const std::string& id, const recording::FlowChunkStore::Snapshot& s) {
    auto levels = nlohmann::json::array();
    for (size_t n = 0; n < heatmap::kChunkLevels.size(); ++n) {
        const auto level = heatmap::kChunkLevels[n], span = heatmap::chunkSpanMs(level);
        auto sealed = nlohmann::json::array();
        for (const auto& run : s.levels[n]) sealed.push_back({run.fromMs, run.toMs});
        levels.push_back({{"level_ms", level}, {"chunk_span_ms", span}, {"sealed", sealed},
            {"oldest_ms", s.levels[n].empty() ? nlohmann::json(nullptr) : nlohmann::json(s.levels[n].front().fromMs)},
            {"latest_ms", s.levels[n].empty() ? nlohmann::json(nullptr) : nlohmann::json(s.levels[n].back().toMs - span)}});
    }
    char epoch[17]; std::snprintf(epoch, sizeof epoch, "%016llx", static_cast<unsigned long long>(s.epoch));
    const nlohmann::json channel = {{"epoch", epoch}, {"meta", nlohmann::json::object()}, {"levels", levels}};
    return {{"type", "chunk_availability"}, {"instrument", id}, {"wire_version", chunks::WireVersion},
        {"live_from_ms", s.liveFromMs}, {"channels", {{"flow.summary", channel}, {"flow.rows", channel}}}};
}
} // namespace protocol::flowchunks
