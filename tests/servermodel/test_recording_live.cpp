#include "servermodel/RecordingLive.hpp"
#include <gtest/gtest.h>
#include <QTemporaryDir>
#include <condition_variable>
#include <thread>
using namespace recording;
namespace {
constexpr int64_t epoch = kHmc2MinMs;
RecordPtr record(int minute, double quantity, uint32_t observed = 60000, bool provisional = false) {
    auto r = std::make_shared<Hmc2Record>();
    r->header.symbol = "BTC-USD"; r->header.layer = "near";
    r->bucketStartMs = epoch + minute * 60000;
    r->observedMs = observed;
    r->flags = provisional ? kProvisional : 0;
    r->bidRowLo = r->askRowLo = 90; r->bidRowHi = r->askRowHi = 110;
    r->midOpen = r->midClose = r->midMin = r->midMax = 100;
    r->entries.push_back({99, false, encodeSize(quantity), encodeSize(quantity)});
    return r;
}
LiveView view(int64_t tf = 60000) { return {"BTC-USD", "near", tf, {90, 1, 20}, 42}; }
double quantity(const ServedColumn &c) { return decodeSize(c.cells[10] & 0x7fff); }
}
TEST(RecordingLive, OneMinuteAndFiveMinuteWeightedFormingBuckets) {
    QTemporaryDir dir;
    Hmc2Reader reader(dir.path().toStdString());
    LiveCache cache;
    ASSERT_TRUE(cache.publish(record(0, 2)));
    ASSERT_TRUE(cache.publish(record(1, 4)));
    ASSERT_TRUE(cache.publish(record(2, 8, 30000, true)));
    LiveBuilder one(view()), five(view(300000));
    auto a = one.build(reader, cache.snapshot("BTC-USD", "near"));
    ASSERT_EQ(a.columns.size(), 2); // previous committed + current forming
    EXPECT_EQ(a.columns.back().bucketStartMs, epoch + 120000);
    EXPECT_NEAR(quantity(a.columns.back()), 8, .01);
    auto b = five.build(reader, cache.snapshot("BTC-USD", "near"));
    ASSERT_EQ(b.columns.size(), 1);
    EXPECT_EQ(b.columns[0].observedMs, 150000);
    EXPECT_TRUE(b.columns[0].flags & kProvisional);
    EXPECT_NEAR(quantity(b.columns[0]), 4, .01);
    ASSERT_TRUE(cache.publish(record(2, 8)));
    auto final = five.build(reader, cache.snapshot("BTC-USD", "near"));
    ASSERT_EQ(final.columns.size(), 1);
    EXPECT_EQ(final.columns[0].observedMs, 180000);
    EXPECT_NEAR(quantity(final.columns[0]), 14.0 / 3, .01);
    EXPECT_FALSE(final.columns[0].flags & kProvisional);
    auto repeat = five.build(reader, cache.snapshot("BTC-USD", "near"));
    EXPECT_EQ(repeat.columns[0].cells, final.columns[0].cells);
    EXPECT_EQ(repeat.columns[0].observedMs, 180000); // never add a commit twice
}
TEST(RecordingLive, ColdWarmupReadsOnceAndLaterCommitsAreIncremental) {
    QTemporaryDir dir;
    Hmc2Store store(dir.path().toStdString());
    for (int i = 0; i < 3; ++i) store.append(*record(i, 2));
    Hmc2Reader reader(dir.path().toStdString());
    LiveCache cache;
    cache.publish(record(3, 6, 30000, true));
    LiveBuilder builder(view(300000));
    auto a = builder.build(reader, cache.snapshot("BTC-USD", "near"));
    ASSERT_EQ(a.status, BuildStatus::Complete);
    ASSERT_EQ(a.columns.size(), 1);
    EXPECT_EQ(a.columns[0].observedMs, 210000);
    EXPECT_NEAR(quantity(a.columns[0]), 18.0 / 7, .01);
    EXPECT_GT(a.sourceRecords, 0);
    cache.publish(record(3, 6, 45000, true));
    auto b = builder.build(reader, cache.snapshot("BTC-USD", "near"));
    EXPECT_EQ(b.sourceRecords, 0);
    EXPECT_EQ(b.columns[0].observedMs, 225000);
    cache.publish(record(3, 6));
    cache.publish(record(4, 10, 30000, true));
    auto c = builder.build(reader, cache.snapshot("BTC-USD", "near"));
    EXPECT_EQ(c.sourceRecords, 0);
    EXPECT_EQ(c.columns[0].observedMs, 270000);
    EXPECT_NEAR(quantity(c.columns[0]), 34.0 / 9, .01);
}
TEST(RecordingLive, CacheBoundAndStalePublication) {
    LiveCache cache;
    for (int i = 0; i < 100; ++i) cache.publish(record(i, 2));
    auto s = cache.snapshot("BTC-USD", "near");
    EXPECT_EQ(s.committed.size(), LiveCache::kMaxRecords);
    EXPECT_FALSE(cache.publish(record(98, 50, 2000, true)));
    EXPECT_FALSE(cache.publish(record(98, 50)));
    for (int i = 0; i < 100; ++i) cache.publish(record(100, 2, 1000 + i, true));
    s = cache.snapshot("BTC-USD", "near");
    EXPECT_EQ(s.committed.size(), LiveCache::kMaxRecords);
    EXPECT_EQ(s.provisional->observedMs, 1099);
}
TEST(RecordingLive, DelayedCommitCorrectsPriorOutputBucket) {
    QTemporaryDir dir;
    Hmc2Reader reader(dir.path().toStdString());
    LiveCache cache;
    for (int i = 0; i < 4; ++i) cache.publish(record(i, 2));
    cache.publish(record(4, 6, 59000, true));
    LiveBuilder builder(view(300000));
    auto a = builder.build(reader, cache.snapshot("BTC-USD", "near"));
    ASSERT_EQ(a.columns.size(), 1);
    cache.publish(record(5, 8, 1000, true));
    cache.publish(record(4, 6)); // lateness tail commits after open bucket advances
    auto b = builder.build(reader, cache.snapshot("BTC-USD", "near"));
    ASSERT_EQ(b.columns.size(), 2);
    EXPECT_EQ(b.columns[0].observedMs, 300000);
    EXPECT_FALSE(b.columns[0].flags & kProvisional);
    EXPECT_NEAR(quantity(b.columns[0]), 2.8, .01);
    EXPECT_EQ(b.columns[1].bucketStartMs, epoch + 300000);
    EXPECT_EQ(b.columns[1].observedMs, 1000);
}
TEST(RecordingLive, CadenceBacksOffAndRecoversWithoutPendingQueue) {
    LiveCadence c;
    EXPECT_TRUE(c.due(0));
    c.completed(0, true);
    EXPECT_FALSE(c.due(999));
    EXPECT_TRUE(c.due(1000));
    c.completed(1000, false);
    EXPECT_EQ(c.nextMs, 3000);
    c.completed(3000, false);
    EXPECT_EQ(c.nextMs, 7000);
    c.completed(7000, false);
    EXPECT_EQ(c.nextMs, 12000);
    c.completed(12000, true);
    EXPECT_EQ(c.nextMs, 13000);
}
TEST(RecordingLive, ServiceDeliversOffProducerAndRegistrationIsBounded) {
    QTemporaryDir dir;
    LiveService service(dir.path().toStdString());
    std::mutex mutex;
    std::condition_variable wake;
    int delivered = 0;
    const auto producer = std::this_thread::get_id();
    auto subscription = service.subscribe(view(), [&](const auto &v, const auto &page) {
        EXPECT_NE(std::this_thread::get_id(), producer);
        EXPECT_EQ(v.generation, 42);
        EXPECT_EQ(page.columns.size(), 1);
        { std::lock_guard lock(mutex); ++delivered; }
        wake.notify_one();
        return true;
    });
    service.publish(record(0, 2, 1000, true));
    std::unique_lock lock(mutex);
    ASSERT_TRUE(wake.wait_for(lock, std::chrono::seconds(3), [&] { return delivered > 0; }));
    lock.unlock();
    subscription->active.store(false);
    std::vector<std::shared_ptr<LiveService::Subscription>> views;
    for (int i = 0; i < 64; ++i) {
        auto s = service.subscribe(view(), [](const auto &, const auto &) { return true; });
        ASSERT_TRUE(s); views.push_back(s);
    }
    EXPECT_FALSE(service.subscribe(view(), [](const auto &, const auto &) { return true; }));
}

TEST(RecordingLive, OversizeLatestCommitDoesNotLeaveAHoleInRetainedSuffix) {
    LiveCache cache;
    cache.publish(record(0, 1));
    auto large = std::make_shared<Hmc2Record>(*record(1, 2));
    large->entries.resize(LiveCache::kMaxEntries + 1, large->entries.front());
    ASSERT_TRUE(cache.publish(large));
    auto s = cache.snapshot("BTC-USD", "near");
    ASSERT_EQ(s.committed.size(), 1);
    EXPECT_EQ(s.committed.front()->bucketStartMs, epoch + 60000);
    cache.publish(record(2, 3));
    s = cache.snapshot("BTC-USD", "near");
    ASSERT_EQ(s.committed.size(), 1);
    EXPECT_EQ(s.committed.front()->bucketStartMs, epoch + 120000);
}

TEST(RecordingLive, CongestedDeliveryCoalescesToLatestRevision) {
    QTemporaryDir dir;
    LiveService service(dir.path().toStdString());
    std::mutex mutex;
    std::condition_variable wake;
    std::vector<uint64_t> observations;
    auto subscription = service.subscribe(view(), [&](const auto &, const auto &page) {
        std::lock_guard lock(mutex);
        observations.push_back(page.columns.back().observedMs);
        wake.notify_one();
        return observations.size() > 1; // first write reservation was congested
    });
    service.publish(record(0, 2, 1000, true));
    {
        std::unique_lock lock(mutex);
        ASSERT_TRUE(wake.wait_for(lock, std::chrono::seconds(3), [&] { return observations.size() == 1; }));
    }
    for (int i = 1; i <= 100; ++i) service.publish(record(0, 2, 1000 + i, true));
    {
        std::unique_lock lock(mutex);
        ASSERT_TRUE(wake.wait_for(lock, std::chrono::seconds(4), [&] { return observations.size() >= 2; }));
        EXPECT_EQ(observations.size(), 2);
        EXPECT_EQ(observations[0], 1000);
        EXPECT_EQ(observations[1], 1100);
    }
    subscription->active.store(false);
}
