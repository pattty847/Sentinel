/*
Sentinel — ConfigLoader
Role: Load sentinel.yaml and set environment variables before app initialization.
Threading: Called from main() before any threads start.
*/
#pragma once

#include <string>
#include <vector>

class ConfigLoader {
public:
    // Load config file and set environment variables
    // Returns true if file was loaded, false if not found (not an error)
    static bool loadAndSetEnv(const std::string& configPath = "sentinel.yaml");

    // Load .sentinel.yaml override file (user-specific, not in git)
    static bool loadUserOverrides(const std::string& configPath = ".sentinel.yaml");

    // Get list of config files that were loaded (for logging)
    static std::vector<std::string> getLoadedFiles();

private:
    static void setEnvFromYaml(const std::string& filePath);
    static void processNode(const std::string& prefix, void* node);
    static std::vector<std::string> s_loadedFiles;
};
