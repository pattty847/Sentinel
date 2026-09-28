#include "SentinelLogging.hpp"

#include <QProcessEnvironment>

#include <array>
#include <chrono>
#include <cstring>

Q_LOGGING_CATEGORY(logApp, "sentinel.app")
Q_LOGGING_CATEGORY(logData, "sentinel.data")
Q_LOGGING_CATEGORY(logRender, "sentinel.render")
Q_LOGGING_CATEGORY(logDebug, "sentinel.debug")

namespace sentinel::log_throttle {

std::optional<std::int64_t> intervalOverrideMs(const QProcessEnvironment& env,
                                               const char* category) {
    const QString prefix = QStringLiteral("SENTINEL_LOG_") + QString::fromLatin1(category).toUpper();
    const QString msKey = prefix + QStringLiteral("_INTERVAL_MS");
    const QString legacyKey = prefix + QStringLiteral("_INTERVAL");

    std::optional<std::int64_t> ms;
    std::optional<std::int64_t> legacy;
    const QStringList keys = env.keys();
    for (const QString& key : keys) {
        const QString upper = key.toUpper();
        std::optional<std::int64_t>* slot = upper == msKey       ? &ms
                                          : upper == legacyKey   ? &legacy
                                                                 : nullptr;
        if (!slot) continue;
        bool ok = false;
        const qlonglong value = env.value(key).trimmed().toLongLong(&ok);
        if (ok && value >= 0) *slot = value;
    }
    return ms ? ms : legacy;
}

std::int64_t resolveIntervalMs(const char* category, std::int64_t siteDefaultMs) {
    static constexpr std::array<const char*, 4> kCategories{"App", "Data", "Render", "Debug"};
    static const auto overrides = [] {
        const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        std::array<std::optional<std::int64_t>, kCategories.size()> out;
        for (std::size_t i = 0; i < kCategories.size(); ++i) {
            out[i] = intervalOverrideMs(env, kCategories[i]);
        }
        return out;
    }();

    for (std::size_t i = 0; i < kCategories.size(); ++i) {
        if (std::strcmp(category, kCategories[i]) == 0) {
            return overrides[i].value_or(siteDefaultMs);
        }
    }
    return siteDefaultMs;
}

std::int64_t nowMs() noexcept {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

} // namespace sentinel::log_throttle
