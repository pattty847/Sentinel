/*
Sentinel — ConfigLoader
Role: Load sentinel.yaml and map settings to server env or client structs before app initialization.
Threading: Called from main() before any threads start.
*/
#pragma once

#include <string>
#include <vector>
#include "config/ConfigTypes.hpp"

class ConfigLoader {
public:
    // Server: load config file and set environment variables.
    // Returns true if file was loaded, false if not found (not an error).
    static bool loadServerConfig(const std::string& configPath, ServerConfig* outConfig = nullptr);

    // Client: load config file into struct (no environment side effects).
    // Returns true if file was loaded, false if not found (not an error).
    static bool loadClientConfig(const std::string& configPath, ClientConfig* outConfig);

    // Back-compat: legacy helper for server-only startup.
    static bool loadAndSetEnv(const std::string& configPath = "sentinel.yaml");

    // Back-compat: legacy helper for server-only startup.
    static bool loadUserOverrides(const std::string& configPath = ".sentinel.yaml");

    // Get list of config files that were loaded (for logging)
    static std::vector<std::string> getLoadedFiles();

private:
    static void setEnvFromYaml(const std::string& filePath);
    static void processNode(const std::string& prefix, void* node);
    static std::vector<std::string> s_loadedFiles;
};
