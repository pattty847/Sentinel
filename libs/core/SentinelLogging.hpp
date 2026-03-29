#pragma once

#include <QLoggingCategory>
#include <QDebug>
#include <QString>
#include <string>
#include <cstdlib>
#include <fstream>
#include <mutex>

// Qt6: disambiguate QDebug << std::string
inline QDebug operator<<(QDebug debug, const std::string& str) {
    debug << QString::fromStdString(str);
    return debug;
}

Q_DECLARE_LOGGING_CATEGORY(logApp)
Q_DECLARE_LOGGING_CATEGORY(logData)
Q_DECLARE_LOGGING_CATEGORY(logRender)
Q_DECLARE_LOGGING_CATEGORY(logDebug)

namespace sentinel::log_throttle {
    inline constexpr int kApp    = 1;
    inline constexpr int kData   = 20;
    inline constexpr int kRender = 100;
    inline constexpr int kDebug  = 1;
}

namespace sentinel::log_file {
    inline void appendLine(const char* path, const QString& line) {
        static std::mutex ioMutex;
        std::lock_guard<std::mutex> lock(ioMutex);
        std::ofstream out(path, std::ios::app);
        if (!out.is_open()) {
            return;
        }
        out << line.toStdString() << '\n';
    }
}

// Throttle interval overridable via SENTINEL_LOG_<Cat>_INTERVAL
#define SLOG_THROTTLED(cat, defaultInterval, ...)                                      \
    do {                                                                               \
        static std::atomic<uint32_t> _counter{0};                                      \
        static int _interval = []() {                                                  \
            if (const char* env = std::getenv("SENTINEL_LOG_" #cat "_INTERVAL")) {     \
                int envVal = std::atoi(env);                                           \
                if (envVal > 0 && envVal < (defaultInterval)) return envVal;           \
            }                                                                          \
            return (defaultInterval);                                                  \
        }();                                                                           \
        if ((++_counter % _interval) == 0) {                                           \
            qCDebug(log##cat) << __VA_ARGS__;                                          \
        }                                                                              \
    } while(false)

#define sLog_App(...)     SLOG_THROTTLED(App, sentinel::log_throttle::kApp, __VA_ARGS__)
#define sLog_Data(...)    SLOG_THROTTLED(Data, sentinel::log_throttle::kData, __VA_ARGS__)
#define sLog_Render(...)  SLOG_THROTTLED(Render, sentinel::log_throttle::kRender, __VA_ARGS__)
#define sLog_Debug(...)   SLOG_THROTTLED(Debug, sentinel::log_throttle::kDebug, __VA_ARGS__)
#define sLog_AppN(n, ...)    SLOG_THROTTLED(App, n, __VA_ARGS__)
#define sLog_DataN(n, ...)   SLOG_THROTTLED(Data, n, __VA_ARGS__)
#define sLog_RenderN(n, ...) SLOG_THROTTLED(Render, n, __VA_ARGS__)
#define sLog_DebugN(n, ...)  SLOG_THROTTLED(Debug, n, __VA_ARGS__)
#define sLog_Warning(...)  qCWarning(logApp) << __VA_ARGS__
#define sLog_Error(...)    qCCritical(logApp) << __VA_ARGS__
