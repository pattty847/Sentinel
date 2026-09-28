#pragma once

#include "SentinelStreamProtocol.hpp"
#include "../config/ConfigTypes.hpp"
#include "../servermodel/RecordingPage.hpp"
#include "../servermodel/RecordingLive.hpp"
#include <QByteArray>
#include <QtEndian>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <optional>
#include <string>

namespace protocol::recordingwire {

inline nlohmann::json capability(const ServerConfig& cfg, bool available) {
    std::vector<int64_t> timeframes;
    for (const auto tf : cfg.heatmap.timeframesMs)
        if (tf >= 60'000 && tf % 60'000 == 0 && (tf < 3'600'000 || tf % 3'600'000 == 0))
            timeframes.push_back(tf);
    return {{"available", available}, {"layers", {"near", "deep"}},
            {"timeframes_ms", timeframes}, {"size_floor", cfg.recording.sizeFloor},
            {"codes_per_octave", cfg.recording.codesPerOctave}};
}

inline recording::Hmc2Reader& threadReader(const std::filesystem::path& root) {
    thread_local std::unique_ptr<recording::Hmc2Reader> reader;
    thread_local std::filesystem::path readerRoot;
    if (!reader || readerRoot != root) {
        reader = std::make_unique<recording::Hmc2Reader>(root);
        readerRoot = root;
    }
    return *reader;
}

struct Request {
    std::string symbol;
    int64_t timeframeMs = 0, endTimeMs = 0;
    int count = 0, rows = 0;
    double priceMin = 0, priceMax = 0;
    std::optional<double> displayTick;
    std::string requestId;
    uint64_t bandGeneration = 0;
};

inline std::optional<Request> parseRequest(const nlohmann::json& j) {
    try {
        Request q;
        q.symbol = j.at("symbol").get<std::string>();
        q.timeframeMs = j.at("timeframe_ms").get<int64_t>();
        q.endTimeMs = j.value("end_time", int64_t{0});
        q.count = j.at("count").get<int>();
        q.rows = j.at("rows").get<int>();
        q.priceMin = j.at("price_min").get<double>();
        q.priceMax = j.at("price_max").get<double>();
        if (j.contains("display_tick")) q.displayTick = j.at("display_tick").get<double>();
        if (j.contains("request_id")) q.requestId = j.at("request_id").get<std::string>();
        if (j.contains("band_generation")) q.bandGeneration = j.at("band_generation").get<uint64_t>();
        if (q.symbol.empty() || q.symbol.size() > 128 || q.timeframeMs <= 0 || q.endTimeMs < 0 ||
            q.count <= 0 || q.rows <= 0 || q.rows > 16384 ||
            !std::isfinite(q.priceMin) || !std::isfinite(q.priceMax) ||
            q.priceMin < 0 || q.priceMax <= q.priceMin || q.priceMax > 1e12 ||
            (q.displayTick && (!std::isfinite(*q.displayTick) || *q.displayTick <= 0)) ||
            q.requestId.size() > 128) return std::nullopt;
        q.count = std::min({q.count, SentinelProtocol::kMaxHeatmapHistoryColumns,
                            2'000'000 / q.rows});
        return q;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

inline recording::BuildRequest buildRequest(const Request& q) {
    recording::BuildRequest out;
    out.symbol = q.symbol;
    out.tfMs = q.timeframeMs;
    out.endMs = q.endTimeMs;
    out.count = static_cast<uint32_t>(q.count);
    out.priceLo = q.priceMin;
    out.priceHi = q.priceMax;
    out.rows = static_cast<uint32_t>(q.rows);
    out.displayTick = q.displayTick;
    if (q.displayTick) {
        // Ask the builder to inspect padding rows too, so its validity mask can
        // prove which extra rows were covered. Payload values there are zeroed below.
        const double tick = *q.displayTick;
        const double high = (std::floor(q.priceMin / tick) + q.rows - 0.25) * tick;
        if (std::isfinite(high) && high <= 1e12)
            out.priceHi = std::max(out.priceHi, high);
    }
    return out;
}

inline void emptyPaddingValues(const Request& q, recording::BuildResult& page) {
    if (!q.displayTick || page.band.rows != static_cast<uint32_t>(q.rows) || page.band.tick <= 0) return;
    const double edge = q.priceMax / page.band.tick;
    const double nearest = std::round(edge);
    const double top = std::abs(edge - nearest) <=
        8 * std::numeric_limits<double>::epsilon() * std::max(1.0, std::abs(edge)) ? nearest : edge;
    const int originalRows = static_cast<int>(std::ceil(top) - std::round(page.band.lo / page.band.tick));
    const int padding = std::clamp(q.rows - originalRows, 0, q.rows);
    for (auto& col : page.columns) {
        std::fill_n(col.cells.begin(), std::min(padding, static_cast<int>(col.cells.size())), 0);
        std::fill_n(col.quantities.begin(), std::min(padding, static_cast<int>(col.quantities.size())), 0);
    }
}

inline const char* statusName(recording::BuildStatus status) {
    switch (status) {
    case recording::BuildStatus::Complete: return "complete";
    case recording::BuildStatus::Budget: return "budget";
    case recording::BuildStatus::Cancelled: return "cancelled";
    case recording::BuildStatus::InvalidRequest: return "invalid_request";
    case recording::BuildStatus::IncompatibleGrid: return "incompatible_grid";
    case recording::BuildStatus::IoError: return "io_error";
    }
    return "io_error";
}

inline QByteArray encodeU16(const std::vector<uint16_t>& values, int padTop, int rows) {
    QByteArray bytes(rows * 2, '\0');
    auto* dst = reinterpret_cast<uchar*>(bytes.data());
    for (size_t i = 0; i < values.size() && static_cast<int>(i) + padTop < rows; ++i)
        qToLittleEndian(values[i], dst + (i + padTop) * 2);
    return bytes.toBase64();
}

inline QByteArray padValidity(const std::vector<uint8_t>& source, int sourceRows, int rows) {
    QByteArray bytes((rows + 7) / 8, '\0');
    const int padTop = rows - sourceRows;
    for (int i = 0; i < sourceRows; ++i) {
        if (i / 8 < static_cast<int>(source.size()) && (source[i / 8] & (1u << (i % 8)))) {
            const int dst = padTop + i;
            bytes[dst / 8] = static_cast<char>(bytes[dst / 8] | (1u << (dst % 8)));
        }
    }
    // Rows outside the builder's band have no coverage proof, so remain unknown.
    return bytes.toBase64();
}

inline nlohmann::json buildChunk(const Request& q, const recording::BuildResult& page) {
    const int sourceRows = static_cast<int>(page.band.rows);
    const int padTop = q.rows - sourceRows;
    nlohmann::json out = {
        {"type", "heatmap_history_chunk"},
        {"schema_version", SentinelProtocol::kHeatmapSchemaVersion},
        {"source", "recording"}, {"symbol", q.symbol}, {"timeframe_ms", q.timeframeMs},
        {"request_end_time", q.endTimeMs}, {"request_id", q.requestId},
        {"band_generation", q.bandGeneration}, {"status", statusName(page.status)},
        {"message", page.message}, {"encoding", "base64"}, {"format", "u16"},
        {"value_encoding", "absolute_log_size"}, {"liquidity_encoding", "base64"},
        {"liquidity_format", "u16"}, {"size_floor", page.sizeScale.floor},
        {"codes_per_octave", page.sizeScale.codesPerOctave}, {"layer", page.layer},
        {"band_lo", page.band.lo}, {"band_tick", page.band.tick}, {"band_rows", q.rows},
        {"grid_height", q.rows}, {"grid_width", q.count},
        {"scanned_start", page.scannedStartMs}, {"scanned_end", page.scannedEndMs},
        {"next_end", page.nextEnd}, {"exhausted", page.exhausted},
        {"oldest_available_ms", page.oldestAvailableMs.value_or(0)},
        {"latest_available_ms", page.latestAvailableMs.value_or(0)}
    };
    out["columns"] = nlohmann::json::array();
    if (padTop < 0) return out;
    for (const auto& col : page.columns) {
        out["columns"].push_back({
            {"time_start", col.bucketStartMs}, {"time_end", col.bucketStartMs + q.timeframeMs},
            {"min_price", page.band.lo}, {"max_price", page.band.lo + q.rows * page.band.tick},
            {"tick_size", page.band.tick}, {"column", encodeU16(col.cells, padTop, q.rows).toStdString()},
            {"liquidity_column", encodeU16(col.quantities, padTop, q.rows).toStdString()},
            {"liquidity_scale", col.quantityScale},
            {"validity", padValidity(col.validity, sourceRows, q.rows).toStdString()},
            {"observed_ms", col.observedMs}, {"flags", col.flags}
        });
    }
    return out;
}

inline nlohmann::json viewMessage(const recording::LiveView& v) {
    return {{"type", "heatmap_recording_view"}, {"symbol", v.symbol}, {"layer", v.layer},
        {"timeframe_ms", v.tfMs}, {"band_lo", v.band.lo}, {"band_tick", v.band.tick},
        {"band_rows", v.band.rows}, {"band_generation", v.generation}};
}
inline std::optional<recording::LiveView> parseView(const nlohmann::json& j) {
    try {
        recording::LiveView v;
        v.symbol = j.at("symbol").get<std::string>();
        v.layer = j.at("layer").get<std::string>();
        v.tfMs = j.at("timeframe_ms").get<int64_t>();
        v.band = {j.at("band_lo").get<double>(), j.at("band_tick").get<double>(),
                  j.at("band_rows").get<uint32_t>()};
        v.generation = j.at("band_generation").get<uint64_t>();
        if (v.symbol.empty() || v.symbol.size() > 128 || (v.layer != "near" && v.layer != "deep") ||
            v.tfMs < 60'000 || v.tfMs > 86'400'000 || v.tfMs % 60'000 ||
            (v.tfMs >= 3'600'000 && (v.tfMs % 3'600'000 || v.layer != "deep")) ||
            !v.band.rows || v.band.rows > 16384 || !std::isfinite(v.band.lo) || v.band.lo < 0 ||
            !std::isfinite(v.band.tick) || v.band.tick <= 0 ||
            !std::isfinite(v.band.lo + v.band.tick * v.band.rows) ||
            v.band.lo + v.band.tick * v.band.rows > 1e12 ||
            std::abs(v.band.lo / v.band.tick - std::round(v.band.lo / v.band.tick)) > 1e-6)
            return std::nullopt;
        return v;
    } catch (const std::exception&) { return std::nullopt; }
}
inline nlohmann::json buildLive(const recording::LiveView& v, const recording::BuildResult& page) {
    Request q;
    q.symbol = v.symbol; q.timeframeMs = v.tfMs; q.rows = v.band.rows;
    q.count = 2; q.bandGeneration = v.generation;
    auto out = buildChunk(q, page);
    out["type"] = "heatmap_recording_live";
    return out;
}

inline nlohmann::json error(const std::string& symbol, const std::string& message,
                            const std::string& requestId, uint64_t bandGeneration) {
    return {{"type", "error"}, {"context", "heatmap_history_request"}, {"symbol", symbol},
            {"message", message}, {"request_id", requestId}, {"band_generation", bandGeneration}};
}
} // namespace protocol::recordingwire
