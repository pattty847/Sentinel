/*
Sentinel — ConfigLoader
*/
#include "ConfigLoader.hpp"

#include <yaml-cpp/yaml.h>
#include <iostream>
#include <fstream>
#include <cstdlib>
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
    } else if (node.IsScalar()) {
        const std::string spec = node.as<std::string>();
        std::string normalized = spec;
        std::replace(normalized.begin(), normalized.end(), ',', ' ');
        std::istringstream iss(normalized);
        std::string token;
        while (iss >> token) {
            try {
                const int64_t tf = std::stoll(token);
                if (tf > 0) {
                    out.push_back(tf);
                }
            } catch (...) {
            }
        }
    }
    if (!out.empty()) {
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
    }
    return out;
}

void applyEnv(const char* key, const std::string& value) {
#ifdef _WIN32
    _putenv_s(key, value.c_str());
#else
    setenv(key, value.c_str(), 0);
#endif
}

void parseServerConfig(const std::string& filePath, ServerConfig& cfg) {
    YAML::Node root = YAML::LoadFile(filePath);
    YAML::Node serverNode = root["server"];
    YAML::Node heatmapNode;
    if (serverNode && serverNode["heatmap"]) {
        heatmapNode = serverNode["heatmap"];
    } else {
        heatmapNode = root["heatmap"];
    }

    if (heatmapNode) {
        if (readScalar(heatmapNode, "grid_width", cfg.heatmap.gridWidth)) {
            applyEnv("SENTINEL_HEATMAP_GRID_WIDTH", std::to_string(cfg.heatmap.gridWidth));
        }
        if (readScalar(heatmapNode, "grid_height", cfg.heatmap.gridHeight)) {
            applyEnv("SENTINEL_HEATMAP_GRID_HEIGHT", std::to_string(cfg.heatmap.gridHeight));
            applyEnv("SENTINEL_HEATMAP_GRID", std::to_string(cfg.heatmap.gridHeight));
        }
        if (readScalar(heatmapNode, "tick_size", cfg.heatmap.tickSize)) {
            applyEnv("SENTINEL_HEATMAP_TICK_SIZE", std::to_string(cfg.heatmap.tickSize));
        }
        if (readScalar(heatmapNode, "timeframe", cfg.heatmap.activeTimeframeMs)) {
            applyEnv("SENTINEL_HEATMAP_TF", std::to_string(cfg.heatmap.activeTimeframeMs));
        }
        if (readScalar(heatmapNode, "recenter_delta", cfg.heatmap.recenterDelta)) {
            applyEnv("SENTINEL_HEATMAP_RECENTER_DELTA", std::to_string(cfg.heatmap.recenterDelta));
        } else if (readScalar(heatmapNode, "recenter", cfg.heatmap.recenterDelta)) {
            applyEnv("SENTINEL_HEATMAP_RECENTER_DELTA", std::to_string(cfg.heatmap.recenterDelta));
        }
        if (heatmapNode["timeframes"]) {
            auto parsed = parseTimeframes(heatmapNode["timeframes"]);
            if (!parsed.empty()) {
                cfg.heatmap.timeframesMs = std::move(parsed);
                std::ostringstream oss;
                for (size_t i = 0; i < cfg.heatmap.timeframesMs.size(); ++i) {
                    if (i > 0) oss << ' ';
                    oss << cfg.heatmap.timeframesMs[i];
                }
                applyEnv("SENTINEL_HEATMAP_TIMEFRAMES", oss.str());
            }
        } else if (heatmapNode["timeframes_ms"]) {
            auto parsed = parseTimeframes(heatmapNode["timeframes_ms"]);
            if (!parsed.empty()) {
                cfg.heatmap.timeframesMs = std::move(parsed);
                std::ostringstream oss;
                for (size_t i = 0; i < cfg.heatmap.timeframesMs.size(); ++i) {
                    if (i > 0) oss << ' ';
                    oss << cfg.heatmap.timeframesMs[i];
                }
                applyEnv("SENTINEL_HEATMAP_TIMEFRAMES", oss.str());
            }
        }
    }

    if (serverNode) {
        if (serverNode["default_symbols"]) {
            const auto symbols = parseSymbolList(serverNode["default_symbols"].as<std::string>());
            if (!symbols.empty()) {
                cfg.defaultSymbols = symbols;
                std::ostringstream oss;
                for (size_t i = 0; i < cfg.defaultSymbols.size(); ++i) {
                    if (i > 0) oss << ' ';
                    oss << cfg.defaultSymbols[i];
                }
                applyEnv("SENTINEL_SERVER_DEFAULT_SYMBOLS", oss.str());
            }
        }
        if (serverNode["orderbook"]) {
            auto ob = serverNode["orderbook"];
            if (readScalar(ob, "tick_size", cfg.orderbook.tickSize)) {
                applyEnv("SENTINEL_ORDERBOOK_TICK_SIZE", std::to_string(cfg.orderbook.tickSize));
            }
            if (readScalar(ob, "band_pct", cfg.orderbook.bandPct)) {
                applyEnv("SENTINEL_ORDERBOOK_BAND_PCT", std::to_string(cfg.orderbook.bandPct));
            }
        }
        if (serverNode["candles"]) {
            auto candles = serverNode["candles"];
            if (readScalar(candles, "update_bps_fast", cfg.candles.bpsFast)) {
                applyEnv("SENTINEL_CANDLE_UPDATE_BPS_FAST", std::to_string(cfg.candles.bpsFast));
            }
            if (readScalar(candles, "update_bps_slow", cfg.candles.bpsSlow)) {
                applyEnv("SENTINEL_CANDLE_UPDATE_BPS_SLOW", std::to_string(cfg.candles.bpsSlow));
            }
            if (readScalar(candles, "update_tick_mult_fast", cfg.candles.tickMultFast)) {
                applyEnv("SENTINEL_CANDLE_UPDATE_TICK_MULT_FAST", std::to_string(cfg.candles.tickMultFast));
            }
            if (readScalar(candles, "update_tick_mult_slow", cfg.candles.tickMultSlow)) {
                applyEnv("SENTINEL_CANDLE_UPDATE_TICK_MULT_SLOW", std::to_string(cfg.candles.tickMultSlow));
            }
            if (readScalar(candles, "update_silence_ms_fast", cfg.candles.silenceMsFast)) {
                applyEnv("SENTINEL_CANDLE_UPDATE_SILENCE_MS_FAST", std::to_string(cfg.candles.silenceMsFast));
            }
            if (readScalar(candles, "update_silence_ms_slow", cfg.candles.silenceMsSlow)) {
                applyEnv("SENTINEL_CANDLE_UPDATE_SILENCE_MS_SLOW", std::to_string(cfg.candles.silenceMsSlow));
            }
            if (readScalar(candles, "update_volume_fast", cfg.candles.volumeFast)) {
                applyEnv("SENTINEL_CANDLE_UPDATE_VOLUME_FAST", std::to_string(cfg.candles.volumeFast));
            }
            if (readScalar(candles, "update_volume_slow", cfg.candles.volumeSlow)) {
                applyEnv("SENTINEL_CANDLE_UPDATE_VOLUME_SLOW", std::to_string(cfg.candles.volumeSlow));
            }
            if (readScalar(candles, "update_tick_size", cfg.candles.tickSize)) {
                applyEnv("SENTINEL_CANDLE_UPDATE_TICK_SIZE", std::to_string(cfg.candles.tickSize));
            }
        }
    }

    if (serverNode && serverNode["mdc"]) {
        auto mdc = serverNode["mdc"];
        if (mdc["host"]) {
            applyEnv("SENTINEL_MDC_HOST", mdc["host"].as<std::string>());
        } else if (serverNode["host"]) {
            applyEnv("SENTINEL_MDC_HOST", serverNode["host"].as<std::string>());
        }
        if (mdc["port"]) {
            applyEnv("SENTINEL_MDC_PORT", std::to_string(mdc["port"].as<int>()));
        } else if (serverNode["port"]) {
            applyEnv("SENTINEL_MDC_PORT", std::to_string(serverNode["port"].as<int>()));
        }
        if (mdc["ssl_ca_bundle"]) {
            applyEnv("SENTINEL_SSL_CA_BUNDLE", mdc["ssl_ca_bundle"].as<std::string>());
        }
    } else if (serverNode) {
        if (serverNode["host"]) {
            applyEnv("SENTINEL_MDC_HOST", serverNode["host"].as<std::string>());
        }
        if (serverNode["port"]) {
            applyEnv("SENTINEL_MDC_PORT", std::to_string(serverNode["port"].as<int>()));
        }
        if (serverNode["ssl_ca_bundle"]) {
            applyEnv("SENTINEL_SSL_CA_BUNDLE", serverNode["ssl_ca_bundle"].as<std::string>());
        }
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
    }

    if (clientNode && clientNode["server"]) {
        auto server = clientNode["server"];
        readScalar(server, "host", cfg.server.host);
        readScalar(server, "port", cfg.server.port);
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

bool ConfigLoader::loadAndSetEnv(const std::string& configPath) {
    return loadServerConfig(configPath, nullptr);
}

bool ConfigLoader::loadUserOverrides(const std::string& configPath) {
    return loadServerConfig(configPath, nullptr);
}

std::vector<std::string> ConfigLoader::getLoadedFiles() {
    return s_loadedFiles;
}

void ConfigLoader::setEnvFromYaml(const std::string& filePath) {
    ServerConfig cfg;
    parseServerConfig(filePath, cfg);
}
