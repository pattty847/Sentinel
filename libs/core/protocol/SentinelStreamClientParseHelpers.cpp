#include "SentinelStreamClientParseHelpers.hpp"

namespace protocol::clientparse {

ServerConfig parseServerConfig(const nlohmann::json& msg) {
    ServerConfig cfg;
    if (msg.contains("timeframes_ms") && msg["timeframes_ms"].is_array()) {
        cfg.heatmap.timeframesMs.clear();
        for (const auto& item : msg["timeframes_ms"]) {
            const int64_t tf = item.get<int64_t>();
            if (tf > 0) {
                cfg.heatmap.timeframesMs.push_back(tf);
            }
        }
    }
    if (msg.contains("heatmap") && msg["heatmap"].is_object()) {
        const auto& hm = msg["heatmap"];
        cfg.heatmap.gridWidth = hm.value("grid_width", cfg.heatmap.gridWidth);
        cfg.heatmap.gridHeight = hm.value("grid_height", cfg.heatmap.gridHeight);
        cfg.heatmap.tickSize = hm.value("tick_size", cfg.heatmap.tickSize);
        cfg.heatmap.recenterDelta = hm.value("recenter_delta", cfg.heatmap.recenterDelta);
        cfg.heatmap.bandFast = hm.value("band_fast", cfg.heatmap.bandFast);
        cfg.heatmap.bandMedium = hm.value("band_medium", cfg.heatmap.bandMedium);
        cfg.heatmap.bandSlow = hm.value("band_slow", cfg.heatmap.bandSlow);
        cfg.heatmap.intensityMode = hm.value("intensity_mode", cfg.heatmap.intensityMode);
        cfg.heatmap.intensityMaxMode = hm.value("intensity_max_mode", cfg.heatmap.intensityMaxMode);
        cfg.heatmap.intensityMaxDecay = hm.value("intensity_max_decay", cfg.heatmap.intensityMaxDecay);
        cfg.heatmap.intensityLogScale = hm.value("intensity_log_scale", cfg.heatmap.intensityLogScale);
        cfg.heatmap.intensityPower = hm.value("intensity_power", cfg.heatmap.intensityPower);
        cfg.heatmap.intensityFloor = hm.value("intensity_floor", cfg.heatmap.intensityFloor);
        cfg.heatmap.debugSliceLog = hm.value("debug_slice_log", cfg.heatmap.debugSliceLog);
        cfg.heatmap.activeTimeframeMs = hm.value("active_timeframe_ms", cfg.heatmap.activeTimeframeMs);
    }
    if (msg.contains("orderbook") && msg["orderbook"].is_object()) {
        const auto& ob = msg["orderbook"];
        cfg.orderbook.tickSize = ob.value("tick_size", cfg.orderbook.tickSize);
        cfg.orderbook.bandPct = ob.value("band_pct", cfg.orderbook.bandPct);
    }
    if (msg.contains("candles") && msg["candles"].is_object()) {
        const auto& cd = msg["candles"];
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

