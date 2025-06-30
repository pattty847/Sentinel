#pragma once
#include <fmt/format.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <thread>
#include <sstream>

namespace Sentinel {
namespace Log {

enum class Level { TRACE=0, DEBUG, INFO, WARN, ERROR };

inline Level parseLevel(const char* env) {
    if(!env) return Level::INFO;
    std::string_view s(env);
    if (s == "trace") return Level::TRACE;
    if (s == "debug") return Level::DEBUG;
    if (s == "info")  return Level::INFO;
    if (s == "warn")  return Level::WARN;
    return Level::ERROR;
}

inline Level& runtimeLevel() {
    static Level level = []{
#ifdef NDEBUG
        Level def = Level::INFO;
#else
        Level def = Level::DEBUG;
#endif
        if(const char* env = std::getenv("SENTINEL_LOG"))
            return parseLevel(env);
        return def;
    }();
    return level;
}

inline const char* toString(Level lvl) {
    switch(lvl) {
        case Level::TRACE: return "TRACE";
        case Level::DEBUG: return "DEBUG";
        case Level::INFO:  return "INFO";
        case Level::WARN:  return "WARN";
        default:           return "ERROR";
    }
}

inline std::string baseName(const char* file) {
    std::string_view f(file);
    size_t pos = f.find_last_of("/\\");
    return std::string(pos==std::string_view::npos ? f : f.substr(pos+1));
}

template<class... Args>
inline void log(Level lvl, std::string_view category, const char* file, int line, std::string_view fmt, Args&&... args) {
    if (lvl < runtimeLevel()) return;
    auto now = std::chrono::system_clock::now();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
    std::ostringstream tidss; tidss << std::this_thread::get_id();
    auto msg = fmt::format(fmt, std::forward<Args>(args)...);
    fmt::print("[{:%Y-%m-%d %H:%M:%S}.{:06}][{}][{}][{}:{}] {}\n",
               std::chrono::floor<std::chrono::seconds>(now),
               us%1000000,
               toString(lvl), category, baseName(file), line, msg);
}

}
}

#define LOG_IMPL(level, cat, fmt, ...) ::Sentinel::Log::log(::Sentinel::Log::Level::level, cat, __FILE__, __LINE__, fmt __VA_OPT__(, ) __VA_ARGS__)
#define LOG_T(cat, fmt, ...) LOG_IMPL(TRACE, cat, fmt, __VA_ARGS__)
#define LOG_D(cat, fmt, ...) LOG_IMPL(DEBUG, cat, fmt, __VA_ARGS__)
#define LOG_I(cat, fmt, ...) LOG_IMPL(INFO,  cat, fmt, __VA_ARGS__)
#define LOG_W(cat, fmt, ...) LOG_IMPL(WARN,  cat, fmt, __VA_ARGS__)
#define LOG_E(cat, fmt, ...) LOG_IMPL(ERROR, cat, fmt, __VA_ARGS__)

#define LOG_EVERY_N(level, N, cat, fmt, ...) \ 
    do { static int LOG_##level##_CNT = 0; if(++LOG_##level##_CNT % (N) == 0) LOG_IMPL(level, cat, fmt __VA_OPT__(,) __VA_ARGS__); } while(0)
#define LOG_FIRST_N(level, N, cat, fmt, ...) \ 
    do { static int LOG_##level##_FIRST_CNT = 0; if(LOG_##level##_FIRST_CNT < (N)) { ++LOG_##level##_FIRST_CNT; LOG_IMPL(level, cat, fmt __VA_OPT__(,) __VA_ARGS__); } } while(0)

