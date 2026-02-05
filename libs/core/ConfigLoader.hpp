/*
Sentinel — ConfigLoader
Role: Load sentinel.yaml into server/client config structs before app initialization.
Threading: Called from main() before any threads start.
*/
#pragma once

#include <string>
#include <vector>
#include "config/ConfigTypes.hpp"

class ConfigLoader {
public:
    // Server: load config file into struct.
    // Returns true if file was loaded, false if not found (not an error).
    static bool loadServerConfig(const std::string& configPath, ServerConfig* outConfig = nullptr);

    // Client: load config file into struct.
    // Returns true if file was loaded, false if not found (not an error).
    static bool loadClientConfig(const std::string& configPath, ClientConfig* outConfig);

    // Get list of config files that were loaded (for logging)
    static std::vector<std::string> getLoadedFiles();

private:
    static std::vector<std::string> s_loadedFiles;
};
