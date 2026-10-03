/*
Sentinel — ConfigLoader
*/
#include "ConfigLoader.hpp"
#include "SentinelLogging.hpp"

#include <yaml-cpp/yaml.h>
#include <fstream>
#include <algorithm>
#include <sstream>
#include <cmath>

std::vector<std::string> ConfigLoader::s_loadedFiles;

namespace {
template <typename T>
bool readScalar(const YAML::Node& node, const char* key, T& out) {
    if (!node || !node[key]) {
        return false;
    }
    out = node[key].as<T>();
    return true;
}

template <typename T>
void readMdcScalar(const YAML::Node& root, const YAML::Node& mdc,
                   const std::string& filePath, bool wrapped, const char* key, T& out) {
    const char* source = "retained";
    if (readScalar(root, key, out)) source = wrapped ? "server" : "root";
    if (readScalar(mdc, key, out)) source = wrapped ? "server.mdc" : "mdc";
    sLog_App("Effective MDC config: file=" << filePath << " key=" << key
             << " source=" << source << " value=" << out);
}

std::vector<std::string> parseSymbolList(const std::string& spec) {
    std::string normalized = spec;
    std::replace(normalized.begin(), normalized.end(), ',', ' ');
    std::istringstream iss(normalized);
    std::vector<std::string> out;
    std::string token;
    while (iss >> token) {
        if (!token.empty()) {
            out.push_back(token);
        }
    }
    return out;
}

std::vector<std::string> parseSymbolList(const YAML::Node& node) {
    std::vector<std::string> out;
    if (!node) {
        return out;
    }
    if (node.IsSequence()) {
        out.reserve(node.size());
        for (const auto& item : node) {
            if (!item.IsScalar()) {
                continue;
            }
            const auto symbol = item.as<std::string>();
            if (!symbol.empty()) {
                out.push_back(symbol);
            }
        }
        return out;
    }
    if (node.IsScalar()) {
        return parseSymbolList(node.as<std::string>());
    }
    return out;
}

std::vector<int64_t> parseTimeframes(const YAML::Node& node) {
    std::vector<int64_t> out;
    if (!node) {
        return out;
    }
    if (node.IsSequence()) {
        for (const auto& item : node) {
            const int64_t tf = item.as<int64_t>();
            if (tf > 0) {
                out.push_back(tf);
            }
        }
    }
    if (!out.empty()) {
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
    }
    return out;
}

void parseServerConfig(const std::string& filePath, ServerConfig& cfg) {
    YAML::Node root = YAML::LoadFile(filePath);
    YAML::Node serverNode = root["server"];
    YAML::Node serverRoot = serverNode ? serverNode : root;
    YAML::Node heatmapNode;
    if (serverNode && serverNode["heatmap"]) {
        heatmapNode = serverNode["heatmap"];
    } else {
        heatmapNode = root["heatmap"];
    }

    if (heatmapNode) {
        readScalar(heatmapNode, "grid_width", cfg.heatmap.gridWidth);
        readScalar(heatmapNode, "grid_height", cfg.heatmap.gridHeight);
        readScalar(heatmapNode, "tick_size", cfg.heatmap.tickSize);
        readScalar(heatmapNode, "timeframe", cfg.heatmap.activeTimeframeMs);
        if (!readScalar(heatmapNode, "recenter_delta", cfg.heatmap.recenterDelta)) {
            readScalar(heatmapNode, "recenter", cfg.heatmap.recenterDelta);
        }
        readScalar(heatmapNode, "band_fast", cfg.heatmap.bandFast);
        readScalar(heatmapNode, "band_medium", cfg.heatmap.bandMedium);
        readScalar(heatmapNode, "band_slow", cfg.heatmap.bandSlow);
        readScalar(heatmapNode, "intensity_mode", cfg.heatmap.intensityMode);
        readScalar(heatmapNode, "intensity_max_mode", cfg.heatmap.intensityMaxMode);
        readScalar(heatmapNode, "intensity_max_decay", cfg.heatmap.intensityMaxDecay);
        readScalar(heatmapNode, "intensity_log_scale", cfg.heatmap.intensityLogScale);
        readScalar(heatmapNode, "intensity_power", cfg.heatmap.intensityPower);
        readScalar(heatmapNode, "intensity_floor", cfg.heatmap.intensityFloor);
        readScalar(heatmapNode, "debug_slice_log", cfg.heatmap.debugSliceLog);
        readScalar(heatmapNode, "persistence_enabled", cfg.heatmap.persistenceEnabled);
        readScalar(heatmapNode, "persistence_dir", cfg.heatmap.persistenceDir);
        readScalar(heatmapNode, "persistence_fsync_every_n_records", cfg.heatmap.persistenceFsyncEveryNRecords);
        readScalar(heatmapNode, "persistence_fsync_every_ms", cfg.heatmap.persistenceFsyncEveryMs);
        readScalar(heatmapNode, "persistence_retention_days", cfg.heatmap.persistenceRetentionDays);
        if (heatmapNode["timeframes"]) {
            auto parsed = parseTimeframes(heatmapNode["timeframes"]);
            if (!parsed.empty()) {
                cfg.heatmap.timeframesMs = std::move(parsed);
            }
        } else if (heatmapNode["timeframes_ms"]) {
            auto parsed = parseTimeframes(heatmapNode["timeframes_ms"]);
            if (!parsed.empty()) {
                cfg.heatmap.timeframesMs = std::move(parsed);
            }
        }
    }

    if (serverRoot) {
        readScalar(serverRoot, "stream_port", cfg.streamPort);
        if (serverRoot["default_symbols"]) {
            const auto symbols = parseSymbolList(serverRoot["default_symbols"]);
            if (!symbols.empty()) {
                cfg.defaultSymbols = symbols;
            }
        }
        if (serverRoot["trade_overlays"]) {
            const auto node = serverRoot["trade_overlays"];
            readScalar(node, "grid_width", cfg.tradeOverlays.gridWidth);
            readScalar(node, "grid_height", cfg.tradeOverlays.gridHeight);
            readScalar(node, "tick_size", cfg.tradeOverlays.tickSize);
            readScalar(node, "footprint_timeframe_ms", cfg.tradeOverlays.footprintTimeframeMs);
        }
        if (serverRoot["orderbook"]) {
            auto ob = serverRoot["orderbook"];
            readScalar(ob, "tick_size", cfg.orderbook.tickSize);
            readScalar(ob, "band_pct", cfg.orderbook.bandPct);
        }
        if (serverRoot["recording"]) {
            auto rec = serverRoot["recording"];
            readScalar(rec, "enabled", cfg.recording.enabled);
            readScalar(rec, "dir", cfg.recording.dir);
            readScalar(rec, "fallback_dir", cfg.recording.fallbackDir);
            readScalar(rec, "price_scale", cfg.recording.priceScale);
            readScalar(rec, "size_floor", cfg.recording.sizeFloor);
            readScalar(rec, "codes_per_octave", cfg.recording.codesPerOctave);
            readScalar(rec, "near_tick", cfg.recording.nearTick);
            readScalar(rec, "near_pct", cfg.recording.nearPct);
            readScalar(rec, "deep_tick", cfg.recording.deepTick);
            readScalar(rec, "deep_low_frac", cfg.recording.deepLowFrac);
            readScalar(rec, "deep_high_mult", cfg.recording.deepHighMult);
            readScalar(rec, "lateness_ms", cfg.recording.latenessMs);
            if (readScalar(rec, "live_publish_ms", cfg.recording.livePublishMs))
                cfg.recording.livePublishMs = std::clamp(cfg.recording.livePublishMs,
                    ServerRecordingConfig::kLivePublishMinMs, ServerRecordingConfig::kLivePublishMaxMs);
        }
        if (serverRoot["candles"]) {
            auto candles = serverRoot["candles"];
            readScalar(candles, "update_bps_fast", cfg.candles.bpsFast);
            readScalar(candles, "update_bps_slow", cfg.candles.bpsSlow);
            readScalar(candles, "update_tick_mult_fast", cfg.candles.tickMultFast);
            readScalar(candles, "update_tick_mult_slow", cfg.candles.tickMultSlow);
            readScalar(candles, "update_silence_ms_fast", cfg.candles.silenceMsFast);
            readScalar(candles, "update_silence_ms_slow", cfg.candles.silenceMsSlow);
            readScalar(candles, "update_volume_fast", cfg.candles.volumeFast);
            readScalar(candles, "update_volume_slow", cfg.candles.volumeSlow);
            readScalar(candles, "update_tick_size", cfg.candles.tickSize);
        }
        if (serverRoot["trading"]) {
            auto trading = serverRoot["trading"];
            readScalar(trading, "mode", cfg.trading.mode);
            readScalar(trading, "slippage_bps", cfg.trading.slippageBps);
        }
        if (serverRoot["tls"]) {
            auto tls = serverRoot["tls"];
            readScalar(tls, "cert_file", cfg.tls.certFile);
            readScalar(tls, "key_file",  cfg.tls.keyFile);
        }
    }

    const YAML::Node mdc = serverRoot ? serverRoot["mdc"] : YAML::Node{};
    const bool wrapped = bool(serverNode);
    readMdcScalar(serverRoot, mdc, filePath, wrapped, "host", cfg.mdc.host);
    readMdcScalar(serverRoot, mdc, filePath, wrapped, "port", cfg.mdc.port);
    readMdcScalar(serverRoot, mdc, filePath, wrapped, "target", cfg.mdc.target);
    readMdcScalar(serverRoot, mdc, filePath, wrapped, "use_jwt", cfg.mdc.useJwt);
    readMdcScalar(serverRoot, mdc, filePath, wrapped, "ssl_ca_bundle", cfg.mdc.sslCaBundle);
    readMdcScalar(serverRoot, mdc, filePath, wrapped, "connect_timeout_ms", cfg.mdc.connectTimeoutMs);
    readMdcScalar(serverRoot, mdc, filePath, wrapped, "close_timeout_ms", cfg.mdc.closeTimeoutMs);
    readMdcScalar(serverRoot, mdc, filePath, wrapped, "max_connections", cfg.mdc.maxConnections);
    if (cfg.mdc.maxConnections < 1)
        throw std::runtime_error("server.mdc.max_connections must be at least 1");
}

void parseClientConfig(const std::string& filePath, ClientConfig& cfg) {
    YAML::Node root = YAML::LoadFile(filePath);
    YAML::Node clientNode = root["client"];
    YAML::Node heatmapNode;
    if (clientNode && clientNode["heatmap"]) {
        heatmapNode = clientNode["heatmap"];
    } else {
        heatmapNode = root["heatmap"];
    }

    if (heatmapNode) {
        readScalar(heatmapNode, "renderer", cfg.heatmap.renderer);
        readScalar(heatmapNode, "tick_mode", cfg.heatmap.tickMode);
        readScalar(heatmapNode, "manual_tick", cfg.heatmap.manualTick);
        readScalar(heatmapNode, "min_row_px", cfg.heatmap.minRowPx);
        readScalar(heatmapNode, "hysteresis", cfg.heatmap.hysteresis);
        readScalar(heatmapNode, "crossfade_ms", cfg.heatmap.crossfadeMs);
        readScalar(heatmapNode, "show_band_edges", cfg.heatmap.showBandEdges);
        readScalar(heatmapNode, "palette_preset", cfg.heatmap.palettePreset);
        // A malformed gradient (wrong shape, non-numeric position, non-scalar
        // color) keeps the default instead of failing the whole client config.
        const auto readGradient = [&](const char *key, auto &out) {
            const auto node = heatmapNode[key];
            if (!node) return;
            if (!node.IsSequence() || node.size() < 2 || node.size() > 16) {
                sLog_Warning("Ignored heatmap." << key << ": expected 2..16 {position, color} stops");
                return;
            }
            std::vector<std::pair<double, std::string>> stops;
            for (const auto &stop : node) {
                const auto position = stop.IsMap() ? stop["position"] : YAML::Node();
                const auto color = stop.IsMap() ? stop["color"] : YAML::Node();
                double value = 0;
                if (!position || !position.IsScalar() || !color || !color.IsScalar() ||
                    !YAML::convert<double>::decode(position, value) || !std::isfinite(value)) {
                    sLog_Warning("Ignored heatmap." << key << ": malformed stop");
                    return;
                }
                stops.emplace_back(value, color.Scalar());
            }
            out = std::move(stops); // chartDefaults validates positions/colors
        };
        readGradient("bid_gradient", cfg.heatmap.bidGradient);
        readGradient("ask_gradient", cfg.heatmap.askGradient);
        readScalar(heatmapNode, "opacity", cfg.heatmap.opacity);
        readScalar(heatmapNode, "gpu_cap_bytes", cfg.heatmap.gpuCapBytes);
        readScalar(heatmapNode, "upload_budget_bytes", cfg.heatmap.uploadBudgetBytes);
        readScalar(heatmapNode, "decoded_chunk_bytes", cfg.heatmap.decodedChunkBytes);
        readScalar(heatmapNode, "span_source_bytes", cfg.heatmap.spanSourceBytes);
        readScalar(heatmapNode, "cpu_ceiling_bytes", cfg.heatmap.cpuCeilingBytes);
        readScalar(heatmapNode, "prefetch_tiles", cfg.heatmap.prefetchTiles);
        readScalar(heatmapNode, "live_min_interval_ms", cfg.heatmap.liveMinIntervalMs);
        readScalar(heatmapNode, "show_telemetry", cfg.heatmap.showTelemetry);
        if (cfg.heatmap.renderer != "legacy" && cfg.heatmap.renderer != "gpu")
            cfg.heatmap.renderer = "gpu";
        readScalar(heatmapNode, "source", cfg.heatmap.source);
        if (cfg.heatmap.source != "legacy" && cfg.heatmap.source != "recording")
            cfg.heatmap.source = "legacy";
        readScalar(heatmapNode, "gamma", cfg.heatmap.gamma);
        readScalar(heatmapNode, "contrast", cfg.heatmap.contrast);
        readScalar(heatmapNode, "shader_floor", cfg.heatmap.shaderFloor);
        readScalar(heatmapNode, "label_px", cfg.heatmap.labelPx);
        readScalar(heatmapNode, "client_cache_columns", cfg.heatmap.clientCacheColumns);
        readScalar(heatmapNode, "initial_column_px", cfg.heatmap.initialColumnPx);
        readScalar(heatmapNode, "initial_price_pct", cfg.heatmap.initialPricePct);
        readScalar(heatmapNode, "target_row_px", cfg.heatmap.targetRowPx);
        readScalar(heatmapNode, "cell_aspect", cfg.heatmap.cellAspect);
        readScalar(heatmapNode, "sensitivity_min", cfg.heatmap.sensitivityMin);
        readScalar(heatmapNode, "sensitivity_max", cfg.heatmap.sensitivityMax);
    }

    YAML::Node guiNode;
    if (clientNode && clientNode["gui"]) {
        guiNode = clientNode["gui"];
    } else {
        guiNode = root["gui"];
    }

    if (guiNode) {
        readScalar(guiNode, "api_port", cfg.gui.apiPort);
        readScalar(guiNode, "screenshot_dir", cfg.gui.screenshotDir);
        readScalar(guiNode, "msdf_font", cfg.gui.msdfFontPath);
        readScalar(guiNode, "axis_label_px", cfg.gui.axisLabelPx);
        readScalar(guiNode, "default_order_qty", cfg.gui.defaultOrderQty);
    }

    YAML::Node tpoNode = (clientNode && clientNode["tpo"]) ? clientNode["tpo"] : root["tpo"];
    if (tpoNode) {
        readScalar(tpoNode, "layout", cfg.tpo.layout);
        readScalar(tpoNode, "theme", cfg.tpo.theme);
        readScalar(tpoNode, "session", cfg.tpo.session);
        readScalar(tpoNode, "period_minutes", cfg.tpo.periodMinutes);
        readScalar(tpoNode, "sessions", cfg.tpo.sessions);
        readScalar(tpoNode, "row_px", cfg.tpo.rowPx);
    }

    if (clientNode && clientNode["server"]) {
        auto server = clientNode["server"];
        readScalar(server, "host",    cfg.server.host);
        readScalar(server, "port",    cfg.server.port);
        readScalar(server, "ca_file", cfg.server.caFile);
    }
}
}

bool ConfigLoader::loadServerConfig(const std::string& configPath, ServerConfig* outConfig) {
    std::ifstream file(configPath);
    if (!file.good()) {
        return false;
    }

    try {
        ServerConfig cfg;
        if (outConfig) {
            cfg = *outConfig;
        }
        parseServerConfig(configPath, cfg);
        if (outConfig) {
            *outConfig = cfg;
        }
        s_loadedFiles.push_back(configPath);
        sLog_App("Loaded server config: path=" << configPath);
        return true;
    } catch (const std::exception& e) {
        sLog_Error("Failed to load server config: path=" << configPath << " error=" << e.what());
        return false;
    }
}

bool ConfigLoader::loadClientConfig(const std::string& configPath, ClientConfig* outConfig) {
    if (!outConfig) {
        return false;
    }
    std::ifstream file(configPath);
    if (!file.good()) {
        return false;
    }

    try {
        parseClientConfig(configPath, *outConfig);
        s_loadedFiles.push_back(configPath);
        sLog_App("Loaded client config: path=" << configPath);
        return true;
    } catch (const std::exception& e) {
        sLog_Error("Failed to load client config: path=" << configPath << " error=" << e.what());
        return false;
    }
}

std::vector<std::string> ConfigLoader::getLoadedFiles() {
    return s_loadedFiles;
}
