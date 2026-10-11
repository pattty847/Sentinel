#include "SentinelStreamClientParseHelpers.hpp"
#include <utility>
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
        cfg.tradeOverlays.gridWidth = overlay.value("grid_width", cfg.tradeOverlays.gridWidth);
        cfg.tradeOverlays.gridHeight = overlay.value("grid_height", cfg.tradeOverlays.gridHeight);
        cfg.tradeOverlays.tickSize = overlay.value("tick_size", cfg.tradeOverlays.tickSize);
        cfg.tradeOverlays.footprintTimeframeMs = overlay.value("footprint_timeframe_ms", cfg.tradeOverlays.footprintTimeframeMs);
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
