#include "SentinelStreamClientParseHelpers.hpp"
#include <utility>
#include <cmath>
#include "../flow/FlowCodec.hpp"
#include <QDateTime>

namespace protocol::clientparse {

Trade parseTrade(const nlohmann::json& msg) {
    Trade t{};
    t.product_id = msg.value("product_id", "");
    t.trade_id = msg.value("trade_id", "");
    t.price = msg.value("price", 0.0);
    t.size = msg.value("size", 0.0);
    // Existing servers forward Coinbase's maker side despite the DTO enum name.
    // Explicit basis lets a future normalized source avoid a second inversion.
    const auto side = msg.value("side", "");
    const auto basis = msg.value("side_basis", "maker");
    t.side = AggressorSide::Unknown;
    if (basis == "maker" || basis == "aggressor") {
        if (side == "buy") t.side = basis == "maker" ? AggressorSide::Sell : AggressorSide::Buy;
        if (side == "sell") t.side = basis == "maker" ? AggressorSide::Buy : AggressorSide::Sell;
    }
    const auto time = QDateTime::fromString(QString::fromStdString(msg.value("time", "")), Qt::ISODateWithMs);
    if (time.isValid()) t.timestamp = std::chrono::system_clock::time_point(std::chrono::milliseconds(time.toMSecsSinceEpoch()));
    return t; // missing/invalid time stays zero; never invent an exchange timestamp
}

ServerConfig parseServerConfig(const nlohmann::json& msg) {
    ServerConfig cfg;
    auto mark = [&cfg](const nlohmann::json& object, const char* wire, const char* field) {
        if (object.contains(wire)) cfg.advertisedFields.insert(field);
    };
    if (msg.contains("trade_overlays") && msg["trade_overlays"].is_object()) {
        const auto& overlay = msg["trade_overlays"];
        mark(overlay, "retention_columns", "tradeOverlays.retentionColumns");
        cfg.tradeOverlays.retentionColumns = overlay.value("retention_columns", cfg.tradeOverlays.retentionColumns);
        cfg.tradeOverlays.gridHeight = overlay.value("grid_height", cfg.tradeOverlays.gridHeight);
        cfg.tradeOverlays.tickSize = overlay.value("tick_size", cfg.tradeOverlays.tickSize);
    }
    mark(msg, "default_symbols", "defaultSymbols");
    if (msg.contains("chart") && msg["chart"].is_object()) {
        const auto& chart = msg["chart"];
        mark(chart, "timeframes_ms", "chart.configuredTimeframesMs");
        mark(chart, "default_timeframe_ms", "chart.defaultTimeframeMs");
        mark(chart, "served_timeframes_ms", "chart.servedTimeframesMs");
        cfg.chart.defaultTimeframeMs = chart.value("default_timeframe_ms", cfg.chart.defaultTimeframeMs);
        if (chart.contains("timeframes_ms") && chart["timeframes_ms"].is_array()) {
            cfg.chart.timeframesMs.clear();
            for (const auto& item : chart["timeframes_ms"])
                if (item.is_number_integer() && item.get<int64_t>() > 0)
                    cfg.chart.timeframesMs.push_back(item.get<int64_t>());
        }
        if (chart.contains("served_timeframes_ms") && chart["served_timeframes_ms"].is_array()) {
            for (const auto& item : chart["served_timeframes_ms"])
                if (item.is_number_integer() && item.get<int64_t>() > 0)
                    cfg.chart.servedTimeframesMs.push_back(item.get<int64_t>());
        }
    }
    if (msg.contains("orderbook") && msg["orderbook"].is_object()) {
        const auto& ob = msg["orderbook"];
        mark(ob, "tick_size", "orderbook.tickSize");
        mark(ob, "band_pct", "orderbook.bandPct");
        cfg.orderbook.tickSize = ob.value("tick_size", cfg.orderbook.tickSize);
        cfg.orderbook.bandPct = ob.value("band_pct", cfg.orderbook.bandPct);
    }
    if (msg.contains("recording") && msg["recording"].is_object()) {
        const auto& rec = msg["recording"];
        mark(rec, "available", "recording.available");
        mark(rec, "layers", "recording.layers");
        mark(rec, "timeframes_ms", "recording.timeframesMs");
        mark(rec, "size_floor", "recording.sizeFloor");
        mark(rec, "codes_per_octave", "recording.codesPerOctave");
        cfg.recording.available = rec.value("available", false);
        cfg.recording.sizeFloor = rec.value("size_floor", cfg.recording.sizeFloor);
        cfg.recording.codesPerOctave = rec.value("codes_per_octave", cfg.recording.codesPerOctave);
        if (rec.contains("layers") && rec["layers"].is_array()) {
            for (const auto& item : rec["layers"])
                if (item.is_string()) cfg.recording.layers.push_back(item.get<std::string>());
        }
        if (rec.contains("timeframes_ms") && rec["timeframes_ms"].is_array()) {
            for (const auto& item : rec["timeframes_ms"])
                if (item.is_number_integer() && item.get<int64_t>() > 0)
                    cfg.recording.timeframesMs.push_back(item.get<int64_t>());
        }
    }
    if (msg.contains("candles") && msg["candles"].is_object()) {
        const auto& cd = msg["candles"];
        for (const auto& [wire, field] : {std::pair{"update_bps_fast", "candles.bpsFast"},
                 {"update_bps_slow", "candles.bpsSlow"}, {"update_tick_mult_fast", "candles.tickMultFast"},
                 {"update_tick_mult_slow", "candles.tickMultSlow"}, {"update_silence_ms_fast", "candles.silenceMsFast"},
                 {"update_silence_ms_slow", "candles.silenceMsSlow"}, {"update_volume_fast", "candles.volumeFast"},
                 {"update_volume_slow", "candles.volumeSlow"}, {"update_tick_size", "candles.tickSize"}}) mark(cd, wire, field);
        cfg.candles.bpsFast = cd.value("update_bps_fast", cfg.candles.bpsFast);
        cfg.candles.bpsSlow = cd.value("update_bps_slow", cfg.candles.bpsSlow);
        cfg.candles.tickMultFast = cd.value("update_tick_mult_fast", cfg.candles.tickMultFast);
        cfg.candles.tickMultSlow = cd.value("update_tick_mult_slow", cfg.candles.tickMultSlow);
        cfg.candles.silenceMsFast = cd.value("update_silence_ms_fast", cfg.candles.silenceMsFast);
        cfg.candles.silenceMsSlow = cd.value("update_silence_ms_slow", cfg.candles.silenceMsSlow);
        cfg.candles.volumeFast = cd.value("update_volume_fast", cfg.candles.volumeFast);
        cfg.candles.volumeSlow = cd.value("update_volume_slow", cfg.candles.volumeSlow);
        cfg.candles.tickSize = cd.value("update_tick_size", cfg.candles.tickSize);
    }
    if (msg.contains("default_symbols") && msg["default_symbols"].is_array()) {
        cfg.defaultSymbols.clear();
        for (const auto& item : msg["default_symbols"]) {
            if (item.is_string()) {
                cfg.defaultSymbols.push_back(item.get<std::string>());
            }
        }
    }
    return cfg;
}

VolumeProfileSessionBounds parseVolumeProfileSessionBounds(const nlohmann::json& msg) {
    return {
        msg.contains("session_start_ms")
            ? msg.value("session_start_ms", static_cast<int64_t>(0))
            : msg.value("session_start", static_cast<int64_t>(0)),
        msg.contains("session_end_ms")
            ? msg.value("session_end_ms", static_cast<int64_t>(0))
            : msg.value("session_end", static_cast<int64_t>(0))
    };
}

SentinelStreamClient::CandleBar parseCandleBar(const nlohmann::json& item) {
    SentinelStreamClient::CandleBar bar;
    bar.timeStartMs = item.value("time_start_ms", static_cast<int64_t>(0));
    bar.timeEndMs = item.value("time_end_ms", static_cast<int64_t>(0));
    bar.open = item.value("open", 0.0);
    bar.high = item.value("high", 0.0);
    bar.low = item.value("low", 0.0);
    bar.close = item.value("close", 0.0);
    bar.volume = item.value("volume", 0.0);
    bar.isClosed = item.value("is_closed", false);
    return bar;
}

std::vector<OrderBookLevel> parseOrderBookLevels(const nlohmann::json& levels) {
    std::vector<OrderBookLevel> out;
    if (!levels.is_array()) {
        return out;
    }
    out.reserve(levels.size());
    for (const auto& level : levels) {
        if (level.contains("p") && level.contains("q")) {
            out.push_back({level["p"], level["q"]});
        }
    }
    return out;
}

std::vector<BookLevelUpdate> parseL2Updates(const nlohmann::json& deltas) {
    std::vector<BookLevelUpdate> out;
    if (!deltas.is_array()) {
        return out;
    }
    out.reserve(deltas.size());
    for (const auto& d : deltas) {
        const std::string side = d.value("side", "");
        const bool isBid = (side == "bid");
        const double price = d.value("price", 0.0);
        const double size = d.value("size", 0.0);
        out.push_back({isBid, price, size});
    }
    return out;
}

} // namespace protocol::clientparse

namespace protocol::clientparse {
namespace {
using Client = SentinelStreamClient;
using Json = nlohmann::json;
void requireFlow(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(reason);
}
uint64_t flowUnsigned(const Json& j, const char* key, uint64_t max = UINT64_MAX) {
    const auto& value = j.at(key);
    requireFlow(value.is_number_unsigned() || (value.is_number_integer() && value.get<int64_t>() >= 0),
                "invalid unsigned flow field");
    const auto n = value.get<uint64_t>();
    requireFlow(n <= max, "flow integer overflow");
    return n;
}
Client::FlowStatus flowStatus(const Json& value) {
    const auto s = value.get<std::string>();
    if (s == "complete") return Client::FlowStatus::Complete;
    if (s == "partial") return Client::FlowStatus::Partial;
    if (s == "building") return Client::FlowStatus::Building;
    if (s == "refused") return Client::FlowStatus::Refused;
    throw std::runtime_error("invalid flow status");
}
Client::FlowBatch flowHeader(const Json& j, bool history) {
    requireFlow(j.at("schema_version") == 1, "unsupported flow schema");
    requireFlow(j.at("type") == (history ? "flow_history_chunk" : "flow_bar"), "invalid flow type");
    Client::FlowBatch out;
    out.history = history;
    out.id = flowUnsigned(j, history ? "req" : "sub");
    out.symbol = QString::fromStdString(j.at("symbol").get<std::string>());
    out.timeframeMs = flowUnsigned(j, "timeframe_ms", INT64_MAX);
    requireFlow(!out.symbol.isEmpty() && out.symbol.size() <= 256, "invalid flow symbol");
    requireFlow(validFlowTimeframe(out.timeframeMs), "invalid flow timeframe");
    return out;
}
Client::FlowRecord flowRecord(const Json& meta, std::span<const uint8_t> bytes, int64_t tf) {
    Client::FlowRecord out;
    out.revision = flowUnsigned(meta, "revision", UINT32_MAX);
    out.complete = meta.at("complete").get<bool>();
    const auto source = meta.at("source").get<std::string>();
    requireFlow(source == "live" || source == "store", "invalid flow source");
    out.source = source == "store" ? Client::FlowSource::Store : Client::FlowSource::Live;
    const int64_t start = flowUnsigned(meta, "bucket_start_ms", INT64_MAX - tf);
    out.bar = sentinel::flow::decodeFlowBar(bytes);
    requireFlow(start % tf == 0 && out.bar.startMs == start && out.bar.timeframeMs == tf,
                "flow envelope/bar mismatch");
    return out;
}
uint32_t flowLength(std::span<const uint8_t> frame, size_t& pos) {
    requireFlow(pos <= frame.size() && frame.size() - pos >= 4, "truncated flow length");
    uint32_t n = 0;
    for (unsigned i = 0; i < 4; ++i) n |= uint32_t(frame[pos++]) << (8 * i);
    requireFlow(n <= frame.size() - pos, "truncated flow payload");
    return n;
}
} // namespace

SentinelStreamClient::FlowBatch parseFlowLive(const nlohmann::json& msg) {
    auto out = flowHeader(msg, false);
    requireFlow(msg.at("encoding") == "FLB1/base64" && msg.at("source") == "live", "invalid live flow encoding/source");
    const auto encoded = msg.at("bar").get<std::string>();
    requireFlow(encoded.size() <= protocol::SentinelProtocol::kFlowHistoryMaxFrameBytes, "live flow too large");
    const auto bytes = QByteArray::fromBase64Encoding(QByteArray::fromStdString(encoded),
                                                     QByteArray::AbortOnBase64DecodingErrors);
    requireFlow(bool(bytes), "invalid flow base64");
    auto record = flowRecord(msg, {reinterpret_cast<const uint8_t*>(bytes.decoded.constData()),
                                  size_t(bytes.decoded.size())}, out.timeframeMs);
    const double tickUnits = record.bar.nativeTick * record.bar.priceScale;
    requireFlow(std::isfinite(tickUnits) && tickUnits >= 1 && tickUnits < double(INT64_MAX), "invalid flow grid");
    requireFlow(flowUnsigned(msg, "price_scale", INT64_MAX) == uint64_t(record.bar.priceScale) &&
                flowUnsigned(msg, "row_tick_units", INT64_MAX) ==
                    uint64_t(std::llround(tickUnits)), "flow grid mismatch");
    out.bars.push_back(std::move(record));
    return out;
}

SentinelStreamClient::FlowBatch parseFlowHistory(std::span<const uint8_t> frame) {
    requireFlow(frame.size() >= 8 && frame.size() <= protocol::SentinelProtocol::kFlowHistoryMaxFrameBytes,
                "invalid flow frame size");
    requireFlow(std::equal(frame.begin(), frame.begin() + 4, "FHE1"), "invalid flow magic");
    size_t pos = 4;
    const auto length = flowLength(frame, pos);
    const auto header = Json::parse(frame.begin() + pos, frame.begin() + pos + length);
    pos += length;
    auto out = flowHeader(header, true);
    out.status = flowStatus(header.at("status"));
    out.oldestMs = flowUnsigned(header, "oldest_ms", INT64_MAX);
    out.sealedThroughMs = flowUnsigned(header, "sealed_through_ms", INT64_MAX);
    out.error = QString::fromStdString(header.value("error", ""));
    const auto& ranges = header.at("ranges");
    const auto& bars = header.at("bars");
    requireFlow(ranges.is_array() && ranges.size() <= 512 && bars.is_array() && bars.size() <= 512,
                "invalid flow entry count");
    for (const auto& r : ranges) {
        Client::FlowRange range;
        range.fromMs = flowUnsigned(r, "from_ms", INT64_MAX);
        range.toMs = flowUnsigned(r, "to_ms", INT64_MAX);
        range.status = flowStatus(r.at("status"));
        range.priceScale = r.contains("price_scale") ? flowUnsigned(r, "price_scale", INT64_MAX) : 0;
        range.rowTickUnits = r.contains("row_tick_units") ? flowUnsigned(r, "row_tick_units", INT64_MAX) : 0;
        requireFlow(range.toMs > range.fromMs && (out.ranges.empty() || out.ranges.back().toMs <= range.fromMs),
                    "invalid flow range order");
        out.ranges.push_back(range);
    }
    for (const auto& meta : bars) {
        const auto n = flowLength(frame, pos);
        auto record = flowRecord(meta, frame.subspan(pos, n), out.timeframeMs);
        pos += n;
        requireFlow(out.bars.empty() || out.bars.back().bar.startMs < record.bar.startMs, "invalid flow bar order");
        requireFlow(record.source != Client::FlowSource::Store ||
                    record.bar.startMs + out.timeframeMs <= out.sealedThroughMs, "unsealed store bar");
        out.bars.push_back(std::move(record));
    }
    requireFlow(pos == frame.size(), "trailing flow bytes");
    requireFlow(out.status != Client::FlowStatus::Refused || out.bars.empty(), "refused flow has bars");
    return out;
}
} // namespace protocol::clientparse
