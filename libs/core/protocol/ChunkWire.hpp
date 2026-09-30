#pragma once
// JSON half of the heatmap chunk protocol (docs/MARKETDATA.md, "Heatmap chunk wire").
// Requests and availability are text frames; chunk replies are binary SHE1/SHC1.
#include "../heatmap/ChunkCodec.hpp"
#include "../servermodel/ChunkService.hpp"
#include "../servermodel/RecordingLive.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <charconv>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

namespace protocol::chunkwire {
inline constexpr size_t kMaxStarts = 64;       // keys per heatmap_chunk_request
inline constexpr size_t kMaxIdLength = 64;     // symbol and source ids
inline constexpr size_t kMaxLiveSymbols = 8;

inline nlohmann::json buildLiveSubscribe(const recording::RawTailView& view) {
    return {{"type", "heatmap_live_subscribe"}, {"sub", view.sub}, {"symbol", view.symbol},
            {"sources", view.sources}, {"since_ms", view.sinceMs}};
}
inline std::optional<std::string> parseLiveSubscribe(const nlohmann::json& j, recording::RawTailView& out) {
    if (j.contains("sub") && j["sub"].is_number_unsigned()) out.sub = j["sub"].get<uint64_t>();
    if (j.contains("symbol") && j["symbol"].is_string()) out.symbol = j["symbol"].get<std::string>();
    if (!j.contains("sub") || !j["sub"].is_number_unsigned() || out.symbol.empty() ||
        out.symbol.size() > kMaxIdLength) return "sub and a bounded symbol are required";
    if (!j.contains("sources") || !j["sources"].is_array() || j["sources"].empty() ||
        j["sources"].size() > heatmap::kChunkSources.size()) return "sources must name 1..2 supported sources";
    for (const auto& item : j["sources"]) {
        if (!item.is_string()) return "source must be a string";
        const auto source = item.get<std::string>();
        if (!heatmap::findChunkSource(source) || std::find(out.sources.begin(), out.sources.end(), source) != out.sources.end())
            return "unknown or duplicate source";
        out.sources.push_back(source);
    }
    if (!j.contains("since_ms") || !j["since_ms"].is_number_integer() ||
        (j["since_ms"].is_number_unsigned() && j["since_ms"].get<uint64_t>() > uint64_t(recording::kHmc2EndMs)))
        return "since_ms must be a bounded integer";
    out.sinceMs = j["since_ms"].get<int64_t>();
    if (out.sinceMs != 0 && (out.sinceMs < recording::kHmc2MinMs || out.sinceMs >= recording::kHmc2EndMs ||
                             out.sinceMs % heatmap::kMinuteMs)) return "since_ms must be a supported minute cutoff or zero";
    return std::nullopt;
}

struct Request {
    uint64_t req = 0;
    std::string symbol, source;
    int64_t levelMs = 0;
    std::vector<int64_t> starts;
    std::vector<uint64_t> haveHash; // parallel to starts; 0 = none held
    heatmap::ChunkKey key(size_t i) const { return {symbol, source, levelMs, starts[i]}; }
};

// Content hashes travel as 16 lowercase hex digits: JSON numbers are not
// reliably 64-bit in every decoder. "" means none held.
inline std::string hashHex(uint64_t hash) {
    if (!hash) return {};
    char out[17];
    std::snprintf(out, sizeof out, "%016llx", static_cast<unsigned long long>(hash));
    return out;
}
inline std::optional<uint64_t> parseHashHex(const std::string& text) {
    if (text.empty()) return uint64_t{0};
    if (text.size() != 16) return std::nullopt;
    uint64_t value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value, 16);
    if (ec != std::errc{} || end != text.data() + text.size()) return std::nullopt;
    return value;
}

inline nlohmann::json buildRequest(const Request& q) {
    nlohmann::json hashes = nlohmann::json::array();
    for (const auto h : q.haveHash) hashes.push_back(hashHex(h));
    return {{"type", "heatmap_chunk_request"}, {"req", q.req}, {"symbol", q.symbol},
            {"source", q.source}, {"level_ms", q.levelMs}, {"starts", q.starts}, {"have_hash", hashes}};
}

// Best effort: fields that parse are kept so an error reply can echo them.
// Returns an error message when the request as a whole is invalid.
inline std::optional<std::string> parseRequest(const nlohmann::json& j, Request& q) {
    auto str = [&](const char* name, std::string& out) {
        if (!j.contains(name) || !j[name].is_string()) return false;
        out = j[name].get<std::string>();
        return !out.empty() && out.size() <= kMaxIdLength;
    };
    if (j.contains("req") && j["req"].is_number_unsigned()) q.req = j["req"].get<uint64_t>();
    const bool symbolOk = str("symbol", q.symbol), sourceOk = str("source", q.source);
    if (q.symbol.size() > kMaxIdLength) q.symbol.clear();
    if (q.source.size() > kMaxIdLength) q.source.clear();
    const bool levelOk = j.contains("level_ms") && j["level_ms"].is_number_integer();
    if (levelOk) q.levelMs = j["level_ms"].get<int64_t>();
    if (!j.contains("req") || !j["req"].is_number_unsigned()) return "req must be an unsigned integer";
    if (!symbolOk || !sourceOk || !levelOk) return "symbol, source and level_ms are required";
    if (!j.contains("starts") || !j["starts"].is_array() || j["starts"].empty() ||
        j["starts"].size() > kMaxStarts)
        return "starts must hold 1.." + std::to_string(kMaxStarts) + " chunk starts";
    for (const auto& s : j["starts"]) {
        if (!s.is_number_integer()) return "starts must be integers";
        q.starts.push_back(s.get<int64_t>());
    }
    q.haveHash.assign(q.starts.size(), 0);
    if (j.contains("have_hash")) {
        const auto& h = j["have_hash"];
        if (!h.is_array() || h.size() != q.starts.size()) return "have_hash must parallel starts";
        for (size_t i = 0; i < h.size(); ++i) {
            const auto parsed = h[i].is_string() ? parseHashHex(h[i].get<std::string>()) : std::nullopt;
            if (!parsed) return "have_hash entries are 16 hex digits or empty";
            q.haveHash[i] = *parsed;
        }
    }
    return std::nullopt;
}

struct GridInfo {
    uint64_t configHash = 0;
    int64_t rowTickUnits = 0;
    double priceScale = 0, sizeFloor = 0, codesPerOctave = 0;
};
struct LevelInfo {
    int64_t levelMs = 0, chunkSpanMs = 0, committedThroughMs = 0;
    int64_t oldestMs = 0, latestMs = 0; // bucket starts
};
struct SourceInfo {
    std::string id;
    bool migrationOnly = false;
    std::optional<GridInfo> latestGrid;
    std::vector<LevelInfo> levels;
};
struct Availability {
    std::string symbol;
    int chunkWireVersion = 0;
    std::vector<SourceInfo> sources;
};

inline nlohmann::json buildAvailability(const std::string& symbol,
                                        const std::vector<recording::ChunkService::SourceAvailability>& sources) {
    nlohmann::json out = nlohmann::json::object();
    for (const auto& s : sources) {
        nlohmann::json levels = nlohmann::json::array();
        int64_t oldest = 0, latest = 0;
        for (const auto& l : s.levels) {
            levels.push_back({{"level_ms", l.levelMs}, {"chunk_span_ms", l.chunkSpanMs},
                              {"oldest_ms", *l.oldestMs}, {"latest_ms", l.latestMs.value_or(*l.oldestMs)},
                              {"committed_through_ms", l.committedThroughMs}});
            oldest = oldest ? std::min(oldest, *l.oldestMs) : *l.oldestMs;
            latest = std::max(latest, l.latestMs.value_or(*l.oldestMs));
        }
        nlohmann::json item = {{"migration_only", s.id.rfind("hmc2.", 0) == 0},
                               {"oldest_ms", oldest}, {"latest_ms", latest}, {"levels", levels}};
        if (s.latestGrid) {
            const auto& g = *s.latestGrid;
            item["grids"] = nlohmann::json::array({{{"config_hash", hashHex(g.configHash)},
                {"row_tick_units", g.rowTickUnits}, {"price_scale", g.priceScale},
                {"size_floor", g.sizeScale.floor}, {"codes_per_octave", g.sizeScale.codesPerOctave}}});
        } else {
            item["grids"] = nlohmann::json::array();
        }
        out[s.id] = std::move(item);
    }
    return {{"type", "heatmap_availability"}, {"symbol", symbol},
            {"chunk_wire_version", heatmap::kChunkWireVersion}, {"sources", out}};
}

// Throws nlohmann::json::exception on malformed messages.
inline Availability parseAvailability(const nlohmann::json& j) {
    Availability out;
    out.symbol = j.at("symbol").get<std::string>();
    out.chunkWireVersion = j.at("chunk_wire_version").get<int>();
    for (const auto& [id, s] : j.at("sources").items()) {
        SourceInfo info;
        info.id = id;
        info.migrationOnly = s.value("migration_only", false);
        for (const auto& g : s.at("grids")) {
            const auto hash = parseHashHex(g.at("config_hash").get<std::string>());
            if (!hash) throw std::invalid_argument("bad grid config_hash");
            info.latestGrid = GridInfo{*hash, g.at("row_tick_units").get<int64_t>(),
                                       g.at("price_scale").get<double>(), g.at("size_floor").get<double>(),
                                       g.at("codes_per_octave").get<double>()};
        }
        for (const auto& l : s.at("levels"))
            info.levels.push_back({l.at("level_ms").get<int64_t>(), l.at("chunk_span_ms").get<int64_t>(),
                                   l.at("committed_through_ms").get<int64_t>(),
                                   l.at("oldest_ms").get<int64_t>(), l.at("latest_ms").get<int64_t>()});
        out.sources.push_back(std::move(info));
    }
    return out;
}
} // namespace protocol::chunkwire
