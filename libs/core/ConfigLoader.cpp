/*
Sentinel — ConfigLoader
*/
#include "ConfigLoader.hpp"

#include <yaml-cpp/yaml.h>
#include <iostream>
#include <fstream>
#include <cstdlib>

std::vector<std::string> ConfigLoader::s_loadedFiles;

bool ConfigLoader::loadAndSetEnv(const std::string& configPath) {
    std::ifstream file(configPath);
    if (!file.good()) {
        return false;
    }

    try {
        setEnvFromYaml(configPath);
        s_loadedFiles.push_back(configPath);
        return true;
    } catch (const std::exception& e) {
        std::cerr << "ERROR: Failed to load config " << configPath << ": " << e.what() << std::endl;
        return false;
    }
}

bool ConfigLoader::loadUserOverrides(const std::string& configPath) {
    return loadAndSetEnv(configPath);
}

std::vector<std::string> ConfigLoader::getLoadedFiles() {
    return s_loadedFiles;
}

void ConfigLoader::setEnvFromYaml(const std::string& filePath) {
    YAML::Node root = YAML::LoadFile(filePath);

    // Process each top-level section
    if (root["heatmap"]) {
        auto hm = root["heatmap"];
        if (hm["timeframe"]) {
#ifdef _WIN32
            _putenv_s("SENTINEL_HEATMAP_TF", std::to_string(hm["timeframe"].as<int>()).c_str());
#else
            setenv("SENTINEL_HEATMAP_TF", std::to_string(hm["timeframe"].as<int>()).c_str(), 0);
#endif
        }
        if (hm["grid_width"]) {
#ifdef _WIN32
            _putenv_s("SENTINEL_HEATMAP_GRID_WIDTH", std::to_string(hm["grid_width"].as<int>()).c_str());
#else
            setenv("SENTINEL_HEATMAP_GRID_WIDTH", std::to_string(hm["grid_width"].as<int>()).c_str(), 0);
#endif
        }
        if (hm["grid_height"]) {
#ifdef _WIN32
            _putenv_s("SENTINEL_HEATMAP_GRID_HEIGHT", std::to_string(hm["grid_height"].as<int>()).c_str());
#else
            setenv("SENTINEL_HEATMAP_GRID_HEIGHT", std::to_string(hm["grid_height"].as<int>()).c_str(), 0);
#endif
        }
        if (hm["initial_viewport_pct"]) {
#ifdef _WIN32
            _putenv_s("SENTINEL_HEATMAP_INITIAL_VIEWPORT_PCT", std::to_string(hm["initial_viewport_pct"].as<int>()).c_str());
#else
            setenv("SENTINEL_HEATMAP_INITIAL_VIEWPORT_PCT", std::to_string(hm["initial_viewport_pct"].as<int>()).c_str(), 0);
#endif
        }
        if (hm["initial_price_pct"]) {
#ifdef _WIN32
            _putenv_s("SENTINEL_HEATMAP_INITIAL_PRICE_PCT", std::to_string(hm["initial_price_pct"].as<int>()).c_str());
#else
            setenv("SENTINEL_HEATMAP_INITIAL_PRICE_PCT", std::to_string(hm["initial_price_pct"].as<int>()).c_str(), 0);
#endif
        }
        if (hm["tick_size"]) {
#ifdef _WIN32
            _putenv_s("SENTINEL_HEATMAP_TICK_SIZE", std::to_string(hm["tick_size"].as<double>()).c_str());
#else
            setenv("SENTINEL_HEATMAP_TICK_SIZE", std::to_string(hm["tick_size"].as<double>()).c_str(), 0);
#endif
        }
        if (hm["gamma"]) {
#ifdef _WIN32
            _putenv_s("SENTINEL_HEATMAP_GAMMA", std::to_string(hm["gamma"].as<double>()).c_str());
#else
            setenv("SENTINEL_HEATMAP_GAMMA", std::to_string(hm["gamma"].as<double>()).c_str(), 0);
#endif
        }
        if (hm["contrast"]) {
#ifdef _WIN32
            _putenv_s("SENTINEL_HEATMAP_CONTRAST", std::to_string(hm["contrast"].as<double>()).c_str());
#else
            setenv("SENTINEL_HEATMAP_CONTRAST", std::to_string(hm["contrast"].as<double>()).c_str(), 0);
#endif
        }
        if (hm["label_px"]) {
#ifdef _WIN32
            _putenv_s("SENTINEL_HEATMAP_LABEL_PX", std::to_string(hm["label_px"].as<int>()).c_str());
#else
            setenv("SENTINEL_HEATMAP_LABEL_PX", std::to_string(hm["label_px"].as<int>()).c_str(), 0);
#endif
        }
    }

    if (root["server"]) {
        auto srv = root["server"];
        if (srv["host"]) {
#ifdef _WIN32
            _putenv_s("SENTINEL_MDC_HOST", srv["host"].as<std::string>().c_str());
#else
            setenv("SENTINEL_MDC_HOST", srv["host"].as<std::string>().c_str(), 0);
#endif
        }
        if (srv["port"]) {
#ifdef _WIN32
            _putenv_s("SENTINEL_MDC_PORT", std::to_string(srv["port"].as<int>()).c_str());
#else
            setenv("SENTINEL_MDC_PORT", std::to_string(srv["port"].as<int>()).c_str(), 0);
#endif
        }
        if (srv["default_symbols"]) {
#ifdef _WIN32
            _putenv_s("SENTINEL_SERVER_DEFAULT_SYMBOLS", srv["default_symbols"].as<std::string>().c_str());
#else
            setenv("SENTINEL_SERVER_DEFAULT_SYMBOLS", srv["default_symbols"].as<std::string>().c_str(), 0);
#endif
        }
    }

    if (root["gui"]) {
        auto gui = root["gui"];
        if (gui["screenshot_dir"]) {
#ifdef _WIN32
            _putenv_s("SENTINEL_GUI_SCREENSHOT_DIR", gui["screenshot_dir"].as<std::string>().c_str());
#else
            setenv("SENTINEL_GUI_SCREENSHOT_DIR", gui["screenshot_dir"].as<std::string>().c_str(), 0);
#endif
        }
        if (gui["api_port"]) {
#ifdef _WIN32
            _putenv_s("SENTINEL_GUI_API_PORT", std::to_string(gui["api_port"].as<int>()).c_str());
#else
            setenv("SENTINEL_GUI_API_PORT", std::to_string(gui["api_port"].as<int>()).c_str(), 0);
#endif
        }
    }
}
