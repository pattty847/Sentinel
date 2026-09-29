#include "servermodel/RecordingLive.hpp"
#include <gtest/gtest.h>
#include <QTemporaryDir>
#include <condition_variable>
#include <thread>
#include <future>
#include <fstream>
#include <set>
using namespace recording;
namespace {
constexpr int64_t epoch = kHmc2MinMs;
RecordPtr record(int minute, double quantity, uint32_t observed = 60000, bool provisional = false, int64_t through = -1) {
    auto r = std::make_shared<Hmc2Record>();
    r->header.symbol = "BTC-USD"; r->header.layer = "near";
    r->bucketStartMs = epoch + minute * 60000;
    r->observedMs = observed;
    r->committedThroughMs = through < 0 ? r->bucketStartMs + (provisional ? 0 : 60000) : through;
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
    EXPECT_TRUE(final.columns[0].flags & kProvisional); // 5m output has only three closed minutes
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
    EXPECT_EQ(s.provisional.rbegin()->second->observedMs, 1099);
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

TEST(RecordingLive, PendingAndOpenMinutesNeverRegressFiveMinuteOrHourObservation) {
    for (const int64_t tf : {300000, 3600000}) {
        QTemporaryDir dir;
        Hmc2Reader reader(dir.path().toStdString());
        LiveCache cache;
        cache.publish(record(0, 2));
        cache.publish(record(1, 4, 59000, true, epoch + 60000));
        LiveBuilder builder(view(tf));
        auto before = builder.build(reader, cache.snapshot("BTC-USD", "near"));
        ASSERT_EQ(before.columns.size(), 1);
        EXPECT_EQ(before.columns[0].observedMs, 119000);
        cache.publish(record(1, 4, 60000, true, epoch + 60000)); // finished, not committed
        cache.publish(record(2, 8, 1000, true, epoch + 60000));
        auto snapshot = cache.snapshot("BTC-USD", "near");
        EXPECT_EQ(snapshot.provisional.size(), 2);
        auto pending = builder.build(reader, snapshot);
        ASSERT_EQ(pending.columns.size(), 1);
        EXPECT_EQ(pending.columns[0].observedMs, 121000);
        EXPECT_TRUE(pending.columns[0].flags & kProvisional);
        cache.publish(record(1, 4));
        auto committed = builder.build(reader, cache.snapshot("BTC-USD", "near"));
        EXPECT_EQ(committed.columns[0].observedMs, pending.columns[0].observedMs);
        EXPECT_EQ(committed.columns[0].cells, pending.columns[0].cells);
    }
}

TEST(RecordingLive, PreviousOutputStaysProvisionalUntilItsCommitWatermark) {
    for (const int minutes : {5, 60}) {
        QTemporaryDir dir;
        Hmc2Store store(dir.path().toStdString());
        Hmc2Reader reader(dir.path().toStdString());
        LiveCache cache;
        for (int i = 0; i < minutes - 1; ++i) {
            const auto r = record(i, 2);
            store.append(*r);
            cache.publish(r);
        }
        const auto through = epoch + (minutes - 1) * 60000;
        cache.publish(record(minutes - 1, 2, 60000, true, through));
        cache.publish(record(minutes, 4, 1000, true, through));
        LiveBuilder builder(view(minutes * 60000));
        auto pending = builder.build(reader, cache.snapshot("BTC-USD", "near"));
        ASSERT_EQ(pending.columns.size(), 2);
        EXPECT_EQ(pending.columns[0].observedMs, minutes * 60000);
        EXPECT_TRUE(pending.columns[0].flags & kProvisional);
        cache.publish(record(minutes - 1, 2));
        auto final = builder.build(reader, cache.snapshot("BTC-USD", "near"));
        ASSERT_EQ(final.columns.size(), 2);
        EXPECT_EQ(final.columns[0].observedMs, pending.columns[0].observedMs);
        EXPECT_EQ(final.columns[0].cells, pending.columns[0].cells);
        EXPECT_FALSE(final.columns[0].flags & kProvisional);
    }
}

TEST(RecordingLive, ShutdownWaitsForInflightDeliveryAndRejectsRegistrations) {
    QTemporaryDir dir;
    LiveService service(dir.path().toStdString());
    std::promise<void> entered, release;
    auto released = release.get_future().share();
    auto subscription = service.subscribe(view(), [&](const auto&, const auto&) {
        entered.set_value();
        released.wait(); // emulates a transport handoff delayed during drain
        return true;
    });
    service.publish(record(0, 2, 1000, true));
    ASSERT_EQ(entered.get_future().wait_for(std::chrono::seconds(3)), std::future_status::ready);
    auto stopped = std::async(std::launch::async, [&] { service.shutdown(); });
    EXPECT_EQ(stopped.wait_for(std::chrono::milliseconds(100)), std::future_status::timeout);
    EXPECT_FALSE(subscription->active.load());
    EXPECT_FALSE(service.subscribe(view(), [](const auto&, const auto&) { return true; }));
    release.set_value();
    EXPECT_EQ(stopped.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    service.shutdown(); // idempotent, including the destructor
}

TEST(RecordingLive, IndependentLiveSlotAndRegistrationRateLimit) {
    LiveWriteSlot slot;
    EXPECT_TRUE(slot.tryAcquire());
    EXPECT_FALSE(slot.tryAcquire());
    slot.release();
    EXPECT_TRUE(slot.tryAcquire());
    LiveRegistrationGate gate;
    EXPECT_TRUE(gate.admit(0));
    for (int i = 1; i < 250; ++i) EXPECT_FALSE(gate.admit(i));
    EXPECT_TRUE(gate.admit(250));
}

TEST(RecordingLive, EightIdenticalViewsShareOneBuildAndKeepTheirGenerations) {
    QTemporaryDir dir;
    LiveService service(dir.path().toStdString());
    std::mutex mutex;
    std::condition_variable wake;
    std::set<uint64_t> generations;
    std::vector<std::shared_ptr<LiveService::Subscription>> subscriptions;
    for (int i = 0; i < 8; ++i) {
        auto v = view(300000);
        v.generation = 10 + i;
        subscriptions.push_back(service.subscribe(v, [&](const auto& actual, const auto& page) {
            EXPECT_EQ(page.columns.back().observedMs, 1000);
            { std::lock_guard lock(mutex); generations.insert(actual.generation); }
            wake.notify_one();
            return true;
        }));
    }
    service.publish(record(0, 2, 1000, true));
    {
        std::unique_lock lock(mutex);
        ASSERT_TRUE(wake.wait_for(lock, std::chrono::seconds(3), [&] { return generations.size() == 8; }));
    }
    service.shutdown();
    EXPECT_EQ(service.diagnostics().builds, 1);
    EXPECT_EQ(service.diagnostics().deliveries, 8);
}

TEST(RecordingLive, InvalidProjectionIsDeliveredAsAnErrorBeforeViewDeactivation) {
    QTemporaryDir dir;
    LiveService service(dir.path().toStdString());
    auto invalid = view();
    invalid.band.tick = 0;
    std::promise<BuildStatus> error;
    auto subscription = service.subscribe(invalid, [&](const auto& v, const auto& page) {
        EXPECT_EQ(v.generation, invalid.generation);
        error.set_value(page.status);
        return true;
    });
    service.publish(record(0, 2, 1000, true));
    auto reply = error.get_future();
    ASSERT_EQ(reply.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    EXPECT_EQ(reply.get(), BuildStatus::InvalidRequest);
    service.shutdown();
    EXPECT_FALSE(subscription->active.load());
}

TEST(RecordingLive, SeriesCapacityRaisesOnlyOneDeferredWarning) {
    LiveCache cache;
    for (size_t i = 0; i < LiveCache::kMaxSeries; ++i) {
        auto r = std::make_shared<Hmc2Record>(*record(0, 2));
        r->header.symbol = std::to_string(i);
        ASSERT_TRUE(cache.publish(r));
    }
    EXPECT_FALSE(cache.takeCapacityWarning());
    EXPECT_FALSE(cache.publish(record(0, 2)));
    const auto warning = cache.takeCapacityWarning();
    ASSERT_TRUE(warning);
    EXPECT_EQ(warning->first, "BTC-USD");
    EXPECT_FALSE(cache.publish(record(1, 2)));
    EXPECT_FALSE(cache.takeCapacityWarning());
}

TEST(RecordingLive, FinalProjectionIsCachedAndCanBeOmittedAfterDelivery) {
    for (const int64_t tf : {60000, 300000, 86400000}) {
        QTemporaryDir dir;
        Hmc2Reader reader(dir.path().toStdString());
        LiveCache cache;
        const int minutes = tf / 60000;
        cache.publish(record(minutes - 1, 2));
        cache.publish(record(minutes, 4, 1000, true));
        LiveBuilder builder(view(tf));
        const auto first = builder.build(reader, cache.snapshot("BTC-USD", "near"));
        ASSERT_EQ(first.columns.size(), 2);
        ASSERT_FALSE(first.columns.front().flags & kProvisional);
        const auto finalBucket = first.columns.front().bucketStartMs;
        cache.publish(record(minutes, 4, 2000, true));
        const auto next = builder.build(reader, cache.snapshot("BTC-USD", "near"), finalBucket);
        ASSERT_EQ(next.columns.size(), 1);
        EXPECT_EQ(next.columns.front().observedMs, 2000);
        EXPECT_EQ(next.sourceRecords, 0);
        // A newly joined or congested viewer can still retrieve the cached final.
        const auto missed = builder.build(reader, cache.snapshot("BTC-USD", "near"));
        ASSERT_EQ(missed.columns.size(), 2);
        EXPECT_EQ(missed.columns.front().cells, first.columns.front().cells);
        EXPECT_EQ(missed.columns.front().observedMs, first.columns.front().observedMs);
    }
}

TEST(RecordingLive, FinalsAreDeliveredOncePerViewerIncludingCongestedAndNewViewers) {
    QTemporaryDir dir;
    LiveService service(dir.path().toStdString());
    std::mutex mutex;
    std::condition_variable wake;
    int fastCalls = 0, slowCalls = 0, fastFinals = 0, slowFinals = 0, lateFinals = 0;
    auto callback = [&](int& calls, int& finals, bool congested) {
        return [&, congested](const auto&, const auto& page) {
            std::lock_guard lock(mutex);
            ++calls;
            const bool accepted = !congested || calls > 1;
            if (accepted) for (const auto& c : page.columns) if (!(c.flags & kProvisional)) ++finals;
            wake.notify_one();
            return accepted;
        };
    };
    auto fast = service.subscribe(view(300000), callback(fastCalls, fastFinals, false));
    auto slow = service.subscribe(view(300000), callback(slowCalls, slowFinals, true));
    service.publish(record(4, 2));
    service.publish(record(5, 4, 1000, true));
    {
        std::unique_lock lock(mutex);
        ASSERT_TRUE(wake.wait_for(lock, std::chrono::seconds(3), [&] { return fastCalls && slowCalls; }));
    }
    for (int i = 2; i <= 4; ++i) {
        service.publish(record(5, 4, i * 1000, true));
        std::unique_lock lock(mutex);
        ASSERT_TRUE(wake.wait_for(lock, std::chrono::seconds(4), [&] { return fastCalls >= i; }));
    }
    int lateCalls = 0;
    auto late = service.subscribe(view(300000), callback(lateCalls, lateFinals, false));
    {
        std::unique_lock lock(mutex);
        ASSERT_TRUE(wake.wait_for(lock, std::chrono::seconds(4), [&] { return slowFinals && lateFinals; }));
        EXPECT_EQ(fastFinals, 1);
        EXPECT_EQ(slowFinals, 1);
        EXPECT_EQ(lateFinals, 1);
    }
    service.shutdown();
}

TEST(RecordingLive, IoErrorRetriesSameRevisionWithoutDeactivatingSubscription) {
    QTemporaryDir dir;
    const auto obstruction = std::filesystem::path(dir.path().toStdString()) / "BTC-USD" / "near-60000";
    std::filesystem::create_directories(obstruction.parent_path());
    { std::ofstream file(obstruction); file << 'x'; }
    LiveService service(dir.path().toStdString());
    std::promise<BuildStatus> delivered;
    auto subscription = service.subscribe(view(300000), [&](const auto&, const auto& page) {
        delivered.set_value(page.status);
        return true;
    });
    service.publish(record(3, 4, 1000, true));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!service.diagnostics().builds && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ASSERT_EQ(service.diagnostics().builds, 1);
    auto result = delivered.get_future();
    EXPECT_EQ(result.wait_for(std::chrono::milliseconds(200)), std::future_status::timeout);
    EXPECT_TRUE(subscription->active.load());
    EXPECT_EQ(service.diagnostics().builds, 1); // retry obeys backoff
    std::filesystem::remove(obstruction);
    ASSERT_EQ(result.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    EXPECT_EQ(result.get(), BuildStatus::Complete);
    EXPECT_TRUE(subscription->active.load());
    service.shutdown();
    EXPECT_EQ(service.diagnostics().builds, 2);
}

TEST(RecordingLive, StartAfterShutdownRestoresDeliveryWithoutRevivingOldViews) {
    QTemporaryDir dir;
    LiveService service(dir.path().toStdString());
    auto old = service.subscribe(view(), [](const auto&, const auto&) { return true; });
    service.shutdown();
    service.publish(record(0, 2, 1000, true)); // producer can continue while transport is stopped
    service.start();
    service.start();
    EXPECT_FALSE(old->active.load());
    std::promise<uint64_t> delivered;
    auto current = service.subscribe(view(), [&](const auto&, const auto& page) {
        delivered.set_value(page.columns.back().observedMs);
        return true;
    });
    ASSERT_TRUE(current);
    auto result = delivered.get_future();
    ASSERT_EQ(result.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    EXPECT_EQ(result.get(), 1000);
    service.shutdown();
}

TEST(RecordingLive, TransientReadFailurePreservesPreviouslyWarmedPrefix) {
    QTemporaryDir dir;
    Hmc2Store store(dir.path().toStdString());
    Hmc2Reader reader(dir.path().toStdString());
    for (int i = 0; i < 2; ++i) store.append(*record(i, 2));
    LiveCache cache;
    cache.publish(record(2, 2, 1000, true));
    LiveBuilder builder(view(300000));
    auto warm = builder.build(reader, cache.snapshot("BTC-USD", "near"));
    ASSERT_EQ(warm.status, BuildStatus::Complete);
    ASSERT_EQ(warm.columns.size(), 1);
    EXPECT_EQ(warm.columns.back().observedMs, 121000);
    for (int i = 2; i < 4; ++i) store.append(*record(i, 2));
    cache.publish(record(4, 2, 1000, true));
    reader.beforeCandidateForTest([] { throw std::runtime_error("transient fixture read failure"); });
    auto failed = builder.build(reader, cache.snapshot("BTC-USD", "near"));
    EXPECT_EQ(failed.status, BuildStatus::IoError);
    auto recovered = builder.build(reader, cache.snapshot("BTC-USD", "near"));
    ASSERT_EQ(recovered.status, BuildStatus::Complete);
    ASSERT_EQ(recovered.columns.size(), 1);
    EXPECT_EQ(recovered.sourceRecords, 2); // previous two minutes were not warmed again
    EXPECT_EQ(recovered.columns.back().observedMs, 241000);
    EXPECT_NEAR(quantity(recovered.columns.back()), 2, .01);
}

TEST(RecordingLive, DeepFiveLadderBandMatchesHistoryAcrossGridChangeAndProvisionalCommit) {
    QTemporaryDir dir;
    const auto root = std::filesystem::path(dir.path().toStdString());
    auto deep = [](int offset, int64_t native, double size) {
        auto r = std::make_shared<Hmc2Record>(*record(offset, size));
        r->header.layer = "deep";
        r->header.rowTickUnits = native;
        r->header.configHash = Hmc2Store::configHash(r->header, .25, 4);
        r->bidRowLo = r->askRowLo = 0;
        r->bidRowHi = r->askRowHi = 100;
        r->entries = {{10000 / native, false, encodeSize(size), 0}};
        return r;
    };
    auto old = deep(0, 1000, 2), next = deep(1, 500, 6);
    { Hmc2Store store(root); store.append(*old); store.append(*next); }
    for (const auto tf : {60000, 300000}) {
        for (const auto tick : {25., 50.}) {
            BuildRequest q;
            q.symbol = "BTC-USD"; q.tfMs = tf;
            q.priceLo = 100; q.priceHi = 100 + tick; q.rows = 1;
            q.budgets = {};
            Hmc2Reader reader(root);
            auto history = buildPage(reader, q);
            ASSERT_EQ(history.status, BuildStatus::Complete);
            ASSERT_EQ(history.band.tick, tick);
            ASSERT_EQ(history.layer, "deep");
            LiveBuilder live({q.symbol, history.layer, tf, history.band, 42});
            LiveCache cache;
            cache.publish(old);
            auto forming = std::make_shared<Hmc2Record>(*next);
            forming->flags |= kProvisional;
            forming->committedThroughMs = next->bucketStartMs;
            cache.publish(forming);
            auto page = live.build(reader, cache.snapshot(q.symbol, "deep"));
            ASSERT_EQ(page.status, BuildStatus::Complete);
            ASSERT_EQ(page.columns.size(), history.columns.size());
            EXPECT_TRUE(page.columns.back().flags & kProvisional);
            auto compare = [&] {
                for (size_t i = 0; i < page.columns.size(); ++i) {
                    EXPECT_EQ(page.columns[i].cells, history.columns[i].cells);
                    EXPECT_EQ(page.columns[i].quantities, history.columns[i].quantities);
                    EXPECT_EQ(page.columns[i].quantityScale, history.columns[i].quantityScale);
                    EXPECT_EQ(page.columns[i].validity, history.columns[i].validity);
                    EXPECT_EQ(page.columns[i].observedMs, history.columns[i].observedMs);
                }
            };
            compare();
            cache.publish(next);
            page = live.build(reader, cache.snapshot(q.symbol, "deep"));
            ASSERT_EQ(page.status, BuildStatus::Complete);
            ASSERT_EQ(page.columns.size(), history.columns.size());
            compare();
            // Cold live warmup reads the old generation from disk before the new commit.
            LiveCache cold;
            cold.publish(next);
            LiveBuilder warmed({q.symbol, history.layer, tf, history.band, 43});
            page = warmed.build(reader, cold.snapshot(q.symbol, "deep"));
            ASSERT_EQ(page.status, BuildStatus::Complete);
            ASSERT_EQ(page.columns.size(), history.columns.size());
            compare();
        }
    }
}
