#include "SentinelLogging.hpp"

#include <QProcessEnvironment>
#include <QStringList>

#include <gtest/gtest.h>

#include <chrono>
#include <thread>
#include <vector>

namespace lt = sentinel::log_throttle;

namespace {

QStringList* g_captured = nullptr;

void captureHandler(QtMsgType, const QMessageLogContext&, const QString& msg) {
    if (g_captured) g_captured->append(msg);
}

class CaptureLog {
public:
    CaptureLog() {
        QLoggingCategory::setFilterRules(QStringLiteral("sentinel.*=true"));
        g_captured = &lines;
        previous = qInstallMessageHandler(captureHandler);
    }
    ~CaptureLog() {
        qInstallMessageHandler(previous);
        g_captured = nullptr;
    }
    QStringList lines;

private:
    QtMessageHandler previous = nullptr;
};

} // namespace

TEST(LogThrottleSite, FirstCallPrintsThenOnePerInterval) {
    lt::Site site;
    std::uint32_t suppressed = 99;

    ASSERT_TRUE(site.admit(1000, 5000, suppressed));
    EXPECT_EQ(suppressed, 0u);

    for (int i = 0; i < 7; ++i) {
        EXPECT_FALSE(site.admit(1000, 5000 + i * 100, suppressed));
    }

    ASSERT_TRUE(site.admit(1000, 6000, suppressed));
    EXPECT_EQ(suppressed, 7u);

    EXPECT_FALSE(site.admit(1000, 6999, suppressed));
    ASSERT_TRUE(site.admit(1000, 7000, suppressed));
    EXPECT_EQ(suppressed, 1u);
}

TEST(LogThrottleSite, ConcurrentCallersAdmitExactlyOnePerWindow) {
    lt::Site site;
    constexpr int kThreads = 8;
    constexpr int kCallsPerThread = 1000;
    std::atomic<int> admitted{0};

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&] {
            std::uint32_t suppressed = 0;
            for (int i = 0; i < kCallsPerThread; ++i) {
                if (site.admit(1000, 42, suppressed)) admitted.fetch_add(1);
            }
        });
    }
    for (auto& th : threads) th.join();

    EXPECT_EQ(admitted.load(), 1);
    std::uint32_t suppressed = 0;
    ASSERT_TRUE(site.admit(1000, 1042, suppressed));
    EXPECT_EQ(suppressed, static_cast<std::uint32_t>(kThreads * kCallsPerThread - 1));
}

TEST(LogThrottleEnv, OverrideKeysAreCaseInsensitiveAndMsWinsOverLegacy) {
    QProcessEnvironment env;
    EXPECT_FALSE(lt::intervalOverrideMs(env, "Render").has_value());

    env.insert(QStringLiteral("SENTINEL_LOG_Render_INTERVAL"), QStringLiteral("1"));
    EXPECT_EQ(lt::intervalOverrideMs(env, "Render"), 1);

    env.insert(QStringLiteral("sentinel_log_render_interval_ms"), QStringLiteral(" 250 "));
    EXPECT_EQ(lt::intervalOverrideMs(env, "Render"), 250);

    EXPECT_FALSE(lt::intervalOverrideMs(env, "Data").has_value());
}

TEST(LogThrottleEnv, InvalidOrNegativeValuesAreIgnored) {
    QProcessEnvironment env;
    env.insert(QStringLiteral("SENTINEL_LOG_DATA_INTERVAL_MS"), QStringLiteral("fast"));
    EXPECT_FALSE(lt::intervalOverrideMs(env, "Data").has_value());

    env.insert(QStringLiteral("SENTINEL_LOG_DATA_INTERVAL_MS"), QStringLiteral("-5"));
    env.insert(QStringLiteral("SENTINEL_LOG_DATA_INTERVAL"), QStringLiteral("0"));
    EXPECT_EQ(lt::intervalOverrideMs(env, "Data"), 0);
}

TEST(LogThrottleMacro, PrintsFirstThenAppendsSuppressedCount) {
    if (lt::resolveIntervalMs("Data", 50) != 50) {
        GTEST_SKIP() << "SENTINEL_LOG_DATA_INTERVAL[_MS] is set in the environment";
    }
    // The site is a process-lifetime static; wait out any window left by a repeat run.
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    CaptureLog capture;

    auto logTick = [](int i) { sLog_DataN(50, "tick" << i); };
    for (int i = 0; i < 5; ++i) logTick(i);
    ASSERT_EQ(capture.lines.size(), 1);
    EXPECT_EQ(capture.lines[0], QStringLiteral("tick 0"));

    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    logTick(5);
    ASSERT_EQ(capture.lines.size(), 2);
    EXPECT_EQ(capture.lines[1], QStringLiteral("tick 5 (suppressed 4)"));
}

TEST(LogThrottleMacro, IntervalOfOnePrintsEveryCall) {
    if (lt::resolveIntervalMs("Data", 1) != 1) {
        GTEST_SKIP() << "SENTINEL_LOG_DATA_INTERVAL[_MS] is set in the environment";
    }
    CaptureLog capture;

    for (int i = 0; i < 5; ++i) sLog_DataN(1, std::string("frame ") + std::to_string(i));
    ASSERT_EQ(capture.lines.size(), 5);
    // QDebug quotes the QString that the std::string overload produces.
    EXPECT_EQ(capture.lines[4], QStringLiteral("\"frame 4\""));
}
