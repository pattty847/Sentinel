#pragma once

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>

namespace tpo_debug {

inline int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

inline bool enabled() {
    static const bool on = (std::getenv("SENTINEL_TPO_DEBUG_FILE") != nullptr);
    return on;
}

inline const char* sessionId() {
    static const char* id = []() -> const char* {
        if (const char* env = std::getenv("SENTINEL_TPO_DEBUG_SESSION")) {
            return env;
        }
        return "tpo-debug-session";
    }();
    return id;
}

inline const char* runId() {
    static const char* id = []() -> const char* {
        if (const char* env = std::getenv("SENTINEL_TPO_DEBUG_RUN")) {
            return env;
        }
        return "tpo-baseline";
    }();
    return id;
}

inline const char* logPath() {
    static const char* path = []() -> const char* {
        if (const char* env = std::getenv("SENTINEL_TPO_DEBUG_PATH")) {
            return env;
        }
        return ".cursor/debug.log";
    }();
    return path;
}

inline void append(const char* location,
                   const char* message,
                   const char* hypothesisId,
                   const std::string& dataJson) {
    if (!enabled()) {
        return;
    }
    static std::mutex ioMutex;
    std::lock_guard<std::mutex> lock(ioMutex);
    std::ofstream out(logPath(), std::ios::app);
    if (!out.is_open()) {
        return;
    }
    out << "{\"sessionId\":\"" << sessionId()
        << "\",\"runId\":\"" << runId()
        << "\",\"hypothesisId\":\"" << hypothesisId
        << "\",\"location\":\"" << location
        << "\",\"message\":\"" << message
        << "\",\"data\":" << dataJson
        << ",\"timestamp\":" << nowMs()
        << "}\n";
}

}  // namespace tpo_debug
