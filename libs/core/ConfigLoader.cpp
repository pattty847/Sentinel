/*
Sentinel — ConfigLoader
*/
#include "ConfigLoader.hpp"

#include <yaml-cpp/yaml.h>
#include <iostream>
#include <fstream>
#include <algorithm>
#include <sstream>

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
        if (serverRoot["orderbook"]) {
            auto ob = serverRoot["orderbook"];
            readScalar(ob, "tick_size", cfg.orderbook.tickSize);
            readScalar(ob, "band_pct", cfg.orderbook.bandPct);
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

    if (serverRoot && serverRoot["mdc"]) {
        auto mdc = serverRoot["mdc"];
        readScalar(mdc, "host", cfg.mdc.host);
        readScalar(mdc, "port", cfg.mdc.port);
        readScalar(mdc, "target", cfg.mdc.target);
        readScalar(mdc, "use_jwt", cfg.mdc.useJwt);
        readScalar(mdc, "ssl_ca_bundle", cfg.mdc.sslCaBundle);
    } else if (serverRoot) {
        readScalar(serverRoot, "host", cfg.mdc.host);
        readScalar(serverRoot, "port", cfg.mdc.port);
        readScalar(serverRoot, "target", cfg.mdc.target);
        readScalar(serverRoot, "use_jwt", cfg.mdc.useJwt);
        readScalar(serverRoot, "ssl_ca_bundle", cfg.mdc.sslCaBundle);
    }
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
        readScalar(heatmapNode, "gamma", cfg.heatmap.gamma);
        readScalar(heatmapNode, "contrast", cfg.heatmap.contrast);
        readScalar(heatmapNode, "shader_floor", cfg.heatmap.shaderFloor);
        readScalar(heatmapNode, "label_px", cfg.heatmap.labelPx);
        readScalar(heatmapNode, "client_cache_columns", cfg.heatmap.clientCacheColumns);
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
        std::cout << "Loaded server config: " << configPath << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "ERROR: Failed to load server config " << configPath << ": " << e.what() << std::endl;
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
        std::cout << "Loaded client config: " << configPath << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "ERROR: Failed to load client config " << configPath << ": " << e.what() << std::endl;
        return false;
    }
}

std::vector<std::string> ConfigLoader::getLoadedFiles() {
    return s_loadedFiles;
}
