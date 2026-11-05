#include "LevelTestSupport.hpp"
#include "levelstore/SealStore.hpp"
#include "levelstore/LevelLog.hpp"
#include <gtest/gtest.h>
#include <fstream>
#include <set>
using namespace sentinel::levelstore;
using namespace levelstore_test;
namespace {
constexpr int64_t Hour = 3600000, Day = 86400000;
D2Hour hour(int64_t start) {
    D2Hour h;
    h.start = {start, true, {{true, 9900, 100}, {false, 10100, 200}}, 2, 2};
    for (int i = 0; i < 60; ++i) {
        MinuteBlock b;
        b.bucketStartMs = start + i * 60000;
        b.observedAtStart = true;
        b.recorderObservedMs = 60000;
        b.priceDigits = 2;
        b.sizeDigits = 2;
        h.blocks.push_back(b);
    }
    return h;
}
std::map<std::string, std::string> tree(const fs::path &root) {
    std::map<std::string, std::string> result;
    for (const auto &f : fs::recursive_directory_iterator(root))
        if (f.is_regular_file()) {
            std::ifstream in(f.path(), std::ios::binary);
            result[fs::relative(f.path(), root).string()] = {std::istreambuf_iterator<char>(in), {}};
        }
    return result;
}
} // namespace
TEST(SealStore, HourAndDayAreReadBackLosslesslyAndIdempotently) {
    TempDir dir;
    SealStore store(dir.path, "BTC-USD");
    for (int i = 0; i < 24; ++i) {
        auto h = hour(i * Hour);
        store.publish(h);
        EXPECT_EQ(store.read(i * Hour).timeline, h);
    }
    ASSERT_TRUE(fs::exists(dir.path / "BTC-USD/seals/0.day"));
    const auto before = tree(dir.path);
    store.publish(hour(23 * Hour));
    EXPECT_EQ(tree(dir.path), before);
}
TEST(SealStore, CorruptAndTruncatedSealsFailClosedThenRebuildFromLog) {
    TempDir dir;
    SealStore store(dir.path, "BTC-USD");
    const auto h = hour(0);
    store.publish(h);
    const auto before = tree(dir.path);
    auto path = dir.path / "BTC-USD/seals/0.hour";
    {
        std::fstream out(path, std::ios::in | std::ios::out | std::ios::binary);
        out.seekp(20);
        out.put('\xff');
    }
    EXPECT_THROW(store.read(0), std::exception);
    store.publish(h);
    EXPECT_EQ(tree(dir.path), before);
    fs::resize_file(path, 30);
    EXPECT_THROW(store.read(0), std::exception);
    store.publish(h);
    EXPECT_EQ(tree(dir.path), before);
}
TEST(SealStore, EveryPublicationPointRetriesToIdenticalBytes) {
    TempDir baseline;
    SealStore normal(baseline.path, "BTC-USD");
    normal.publish(hour(0));
    std::set<std::string> points;
    {
        TempDir probe;
        SealStore enumerate(probe.path, "BTC-USD", [&](const char *p) { points.insert(p); });
        enumerate.publish(hour(0));
    }
    ASSERT_GE(points.size(), 8);
    for (const auto &point : points) {
        TempDir dir;
        bool fired = false;
        SealStore fault(dir.path, "BTC-USD", [&](const char *p) {
            if (!fired && point == p) {
                fired = true;
                throw std::runtime_error("injected seal fault");
            }
        });
        EXPECT_THROW(fault.publish(hour(0)), std::exception) << point;
        ASSERT_TRUE(fired);
        SealStore restart(dir.path, "BTC-USD");
        restart.cut(0);
        restart.publish(hour(0));
        EXPECT_EQ(tree(dir.path), tree(baseline.path)) << point;
    }
}
TEST(SealStore, RetentionPinsUnsealedDaysAndRefusesPrunedRecovery) {
    TempDir dir;
    LevelLog log(dir.path, "BTC-USD");
    SealStore seals(dir.path, "BTC-USD");
    DayBinding binding{"BTC-USD", 0, Day, 2};
    log.onDayStart(binding, hour(0).start);
    for (const auto &b : hour(0).blocks)
        log.onBlock(binding, b);
    log.sync();
    auto path = LevelLogReader::hourPath(dir.path, "BTC-USD", 0);
    seals.prune(4 * Day);
    EXPECT_TRUE(fs::exists(path));
    seals.publish(hour(0));
    seals.prune(4 * Day);
    EXPECT_FALSE(fs::exists(path));
    EXPECT_THROW(seals.requireRetained(0), std::exception);
    EXPECT_NO_THROW(seals.requireRetained(2 * Day));
    EXPECT_EQ(seals.read(0).timeline, hour(0));
}
TEST(SealStore, EveryDayAndRetentionWritePointRetriesToEveryFileIdentity) {
    TempDir seed;
    SealStore sealer(seed.path, "BTC-USD");
    LevelLog log(seed.path, "BTC-USD");
    DayBinding binding{"BTC-USD", 0, Day, 2};
    log.onDayStart(binding, hour(0).start);
    for (int h = 0; h < 24; ++h) {
        for (const auto &b : hour(h * Hour).blocks)
            log.onBlock(binding, b);
        sealer.publish(hour(h * Hour));
    }
    log.sync();
    for (bool prune : {false, true}) {
        TempDir baseline;
        fs::copy(seed.path, baseline.path, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
        std::vector<std::string> points;
        SealStore complete(baseline.path, "BTC-USD", [&](const char *name) { points.emplace_back(name); });
        if (prune)
            complete.prune(4 * Day);
        else
            complete.publishDay(0);
        ASSERT_FALSE(points.empty());
        for (size_t at = 0; at < points.size(); ++at) {
            TempDir attempt;
            fs::copy(seed.path, attempt.path, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
            size_t calls = 0;
            bool hit = false;
            SealStore faulty(attempt.path, "BTC-USD", [&](const char *) {
                if (calls++ == at) {
                    hit = true;
                    throw std::runtime_error("injected persistence stop");
                }
            });
            EXPECT_THROW(
                {
                    if (prune)
                        faulty.prune(4 * Day);
                    else
                        faulty.publishDay(0);
                },
                std::exception);
            ASSERT_TRUE(hit);
            SealStore restart(attempt.path, "BTC-USD");
            restart.recoverRetention();
            if (prune)
                restart.prune(4 * Day);
            else
                restart.publishDay(0);
            EXPECT_EQ(tree(attempt.path), tree(baseline.path)) << prune << ":" << at << ":" << points[at];
        }
        RecordProperty(prune ? "prune_fault_points" : "day_fault_points", std::to_string(points.size()));
    }
    sealer.verifyDay(0);
    {
        std::ofstream out(seed.path / "BTC-USD/seals/0.day", std::ios::binary | std::ios::trunc);
        out << "torn";
    }
    EXPECT_THROW(sealer.verifyDay(0), std::exception);
    sealer.publishDay(0);
    EXPECT_NO_THROW(sealer.verifyDay(0));
}
