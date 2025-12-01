#pragma once
#include <string>

namespace Sentinel {

// Main version components
constexpr int VERSION_MAJOR = 1;
constexpr int VERSION_MINOR = 0;
constexpr int VERSION_PATCH = 0;

// Pre-release identifier (empty for stable releases)
constexpr const char* VERSION_PRERELEASE = "alpha";

// Build metadata (set by CMake if needed)
#ifndef SENTINEL_BUILD_HASH
#define SENTINEL_BUILD_HASH ""
#endif

#ifndef SENTINEL_BUILD_DATE
#define SENTINEL_BUILD_DATE __DATE__
#endif

// Version functions
std::string getVersionString();
std::string getFullVersionString();
std::string getBuildInfo();

} // namespace Sentinel