#include "Version.hpp"
#include <sstream>

namespace Sentinel {

std::string getVersionString() {
    std::ostringstream oss;
    oss << VERSION_MAJOR << "." << VERSION_MINOR << "." << VERSION_PATCH;
    
    if (VERSION_PRERELEASE[0] != '\0') {
        oss << "-" << VERSION_PRERELEASE;
    }
    
    return oss.str();
}

std::string getFullVersionString() {
    std::ostringstream oss;
    oss << "Sentinel v" << getVersionString();
    
    std::string buildHash = SENTINEL_BUILD_HASH;
    if (!buildHash.empty()) {
        oss << " (" << buildHash.substr(0, 7) << ")";
    }
    
    return oss.str();
}

std::string getBuildInfo() {
    std::ostringstream oss;
    oss << "Built on " << SENTINEL_BUILD_DATE;
    
    std::string buildHash = SENTINEL_BUILD_HASH;
    if (!buildHash.empty()) {
        oss << " from commit " << buildHash;
    }
    
    return oss.str();
}

} // namespace Sentinel