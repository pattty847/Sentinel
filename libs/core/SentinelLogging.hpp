#pragma once

#include <QLoggingCategory>
#include <QDebug>
#include <QString>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <fstream>
#include <mutex>

class QProcessEnvironment;

// Qt6: disambiguate QDebug << std::string
inline QDebug operator<<(QDebug debug, const std::string& str) {
    debug << QString::fromStdString(str);
    return debug;
}

Q_DECLARE_LOGGING_CATEGORY(logApp)
Q_DECLARE_LOGGING_CATEGORY(logData)
Q_DECLARE_LOGGING_CATEGORY(logRender)
Q_DECLARE_LOGGING_CATEGORY(logDebug)
Q_DECLARE_LOGGING_CATEGORY(logProbe)

// Probes: named, off-by-default value dumps for debugging a specific behavior.
// Enable with SENTINEL_PROBES=<names> (comma separated, case-insensitive).
// A name enables itself and every probe under it: "tpo" enables "tpo.ingest";
// "all" enables every probe. Output: "[tpo.ingest] start=... rows=..." in the
// probe category. Write values as key=value so a reader can grep them.
namespace sentinel::logging {
    // True if filter (a SENTINEL_PROBES value) enables probe name.
    bool probeFilterMatches(const QString& filter, const char* name);
    // True if the process SENTINEL_PROBES enables probe name. Env is read once.
    bool probeEnabled(const char* name);
}

// sLog_App/Data/Render/Debug print every call. To silence a noisy category use
// Qt's category rules (QT_LOGGING_RULES="sentinel.render.debug=false").
//
// Opt-in rate limit for a line that really runs per frame or per message:
// sLog_*N(ms, ...) is a time-based throttle per call site. The first call at a
// site always prints; after that the site prints at most once per interval and
// appends "(suppressed N)" when calls were dropped since its last printed line.
// An interval <= 1 ms prints every call (legacy sLog_*N(1, ...) sites).
namespace sentinel::log_throttle {
    // Default per-category intervals in milliseconds (0 = unthrottled).
    inline constexpr std::int64_t kApp    = 0;
    inline constexpr std::int64_t kData   = 0;
    inline constexpr std::int64_t kRender = 0;
    inline constexpr std::int64_t kDebug  = 0;

    // Override for one category from an environment, or nullopt if unset/invalid.
    // Keys are matched case-insensitively: SENTINEL_LOG_<CAT>_INTERVAL_MS wins over
    // the legacy SENTINEL_LOG_<CAT>_INTERVAL; both are read as milliseconds.
    std::optional<std::int64_t> intervalOverrideMs(const QProcessEnvironment& env,
                                                   const char* category);

    // Effective interval for a site: the process-env override for the category
    // if set, else siteDefaultMs. Env is read once per category per process.
    std::int64_t resolveIntervalMs(const char* category, std::int64_t siteDefaultMs);

    std::int64_t nowMs() noexcept;

    struct Site {
        std::atomic<std::int64_t> nextAllowedMs{std::numeric_limits<std::int64_t>::min()};
        std::atomic<std::uint32_t> suppressed{0};

        // True if this call should print; suppressedOut gets the dropped-call count.
        bool admit(std::int64_t intervalMs, std::int64_t now,
                   std::uint32_t& suppressedOut) noexcept {
            std::int64_t next = nextAllowedMs.load(std::memory_order_relaxed);
            if (now < next ||
                !nextAllowedMs.compare_exchange_strong(next, now + intervalMs,
                                                       std::memory_order_relaxed)) {
                suppressed.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            suppressedOut = suppressed.exchange(0, std::memory_order_relaxed);
            return true;
        }
    };

    struct Suppressed {
        std::uint32_t count;
    };

    inline QDebug operator<<(QDebug debug, Suppressed s) {
        if (s.count != 0) {
            QDebugStateSaver saver(debug);
            debug.nospace() << "(suppressed " << s.count << ')';
        }
        return debug;
    }
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

// Interval (ms) overridable per category via SENTINEL_LOG_<CAT>_INTERVAL_MS
// (case-insensitive); the override applies to every site in the category.
#define SLOG_THROTTLED(cat, defaultIntervalMs, ...)                                    \
    do {                                                                               \
        if (!log##cat().isDebugEnabled()) break;                                       \
        static const std::int64_t _interval =                                          \
            sentinel::log_throttle::resolveIntervalMs(#cat, (defaultIntervalMs));      \
        std::uint32_t _suppressed = 0;                                                 \
        if (_interval > 1) {                                                           \
            static sentinel::log_throttle::Site _site;                                 \
            if (!_site.admit(_interval, sentinel::log_throttle::nowMs(), _suppressed)) \
                break;                                                                 \
        }                                                                              \
        qCDebug(log##cat) << __VA_ARGS__                                               \
                          << sentinel::log_throttle::Suppressed{_suppressed};          \
    } while(false)

#define sLog_App(...)     SLOG_THROTTLED(App, sentinel::log_throttle::kApp, __VA_ARGS__)
#define sLog_Data(...)    SLOG_THROTTLED(Data, sentinel::log_throttle::kData, __VA_ARGS__)
#define sLog_Render(...)  SLOG_THROTTLED(Render, sentinel::log_throttle::kRender, __VA_ARGS__)
#define sLog_Debug(...)   SLOG_THROTTLED(Debug, sentinel::log_throttle::kDebug, __VA_ARGS__)
// n is the site's interval in milliseconds (<= 1 prints every call).
#define sLog_AppN(n, ...)    SLOG_THROTTLED(App, n, __VA_ARGS__)
#define sLog_DataN(n, ...)   SLOG_THROTTLED(Data, n, __VA_ARGS__)
#define sLog_RenderN(n, ...) SLOG_THROTTLED(Render, n, __VA_ARGS__)
#define sLog_DebugN(n, ...)  SLOG_THROTTLED(Debug, n, __VA_ARGS__)
// name must be a string literal, e.g. sLog_Probe("tpo.ingest", "rows=" << rows).
#define sLog_Probe(name, ...)                                                          \
    do {                                                                               \
        static const bool _probeOn = sentinel::logging::probeEnabled(name);            \
        if (_probeOn) {                                                                \
            qCDebug(logProbe).noquote() << "[" name "]" << __VA_ARGS__;                \
        }                                                                              \
    } while(false)

#define sLog_Warning(...)  qCWarning(logApp) << __VA_ARGS__
#define sLog_Error(...)    qCCritical(logApp) << __VA_ARGS__
