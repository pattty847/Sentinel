#include "servermodel/RecordingLive.hpp"
#include "heatmap/RecordingLoader.hpp"
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
    r->flags = (provisional ? kProvisional : 0) | (observed < 60000 ? kPartial : 0);
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
    // The base is the configured cadence: the default publish interval's, the
    // clamp ends' (250 and 5000 ms) and the former fixed 1 s.
    EXPECT_EQ(kLiveCadenceDefaultMs, 250);
    EXPECT_EQ(liveCadenceMs(250), 125);
    EXPECT_EQ(liveCadenceMs(5000), 2500);
    EXPECT_EQ(LiveCadence{}.baseMs, kLiveCadenceDefaultMs);
    for (const int64_t base : {kLiveCadenceDefaultMs, liveCadenceMs(250), liveCadenceMs(5000), int64_t{1000}}) {
        SCOPED_TRACE(base);
        LiveCadence c{base};
        EXPECT_TRUE(c.due(0));
        c.completed(0, true);
        EXPECT_FALSE(c.due(base - 1));
        EXPECT_TRUE(c.due(base));
        int64_t now = base, delay = base;
        for (int refusal = 0; refusal < 6; ++refusal) { // doubles from the base, capped at 5 s
            delay = std::min<int64_t>(kLiveCadenceMaxMs, delay * 2);
            c.completed(now, false);
            EXPECT_EQ(c.nextMs, now + delay);
            now = c.nextMs;
        }
        EXPECT_EQ(c.delayMs, kLiveCadenceMaxMs);
        c.completed(now, true); // acceptance resets to the base
        EXPECT_EQ(c.nextMs, now + base);
        EXPECT_EQ(c.delayMs, base);
    }
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

struct RecordingLiveTest {
    static void clock(LiveService& service, std::function<int64_t()> clock,
                      std::function<void(const char*)> hook = {}) {
        service.shutdown(); service.setClockForTest(std::move(clock));
        service.setRawWorkHookForTest(std::move(hook)); service.start();
    }
    static void poll(LiveService& service) { service.pollForTest(); }
};

TEST(RecordingRawTail, SortedEntriesSharedEncodingAndConservativeRanges) {
    LiveCache cache;
    auto unsorted = std::make_shared<Hmc2Record>(*record(60, 2, 1000, true, epoch+59*60000));
    unsorted->entries = {{104, true, encodeSize(3), 0}, {99, true, encodeSize(4), 0},
                        {99, false, encodeSize(2), 0}, {102, false, encodeSize(1), 0}};
    cache.publish(record(57, 2));
    cache.publish(record(59, 3, 60000, true, epoch+59*60000));
    cache.publish(unsorted);
    const auto snapshot = cache.snapshot("BTC-USD", "near");
    RawTailBuilder builder;
    const auto withFinal = builder.build("BTC-USD", "hmc2.near", snapshot, epoch);
    const auto again = builder.build("BTC-USD", "hmc2.near", snapshot, epoch+56*60000);
    EXPECT_EQ(withFinal.bytes, again.bytes);
    const auto withoutFinal = builder.build("BTC-USD", "hmc2.near", snapshot, epoch+58*60000);
    EXPECT_EQ(withoutFinal.bytes, builder.build("BTC-USD", "hmc2.near", snapshot, epoch+59*60000).bytes);
    EXPECT_EQ(builder.encodings(), 2);
    const auto decoded = heatmap::decodeChunk(*withFinal.bytes);
    ASSERT_EQ(decoded.columns.columns.size(), 3);
    const auto& entries = decoded.columns.columns.back().native.front().entries;
    ASSERT_EQ(entries.size(), 4);
    EXPECT_EQ(entries[0].row(), entries[1].row());
    EXPECT_FALSE(entries[0].isAsk()); EXPECT_TRUE(entries[1].isAsk());
    EXPECT_LT(entries[1].row(), entries[2].row()); EXPECT_LT(entries[2].row(), entries[3].row());
    EXPECT_EQ(heatmap::bucketState(decoded.columns, epoch+58*60000), heatmap::BucketState::NotLoaded);
    EXPECT_EQ(withFinal.finalThroughMs, epoch+58*60000);
    EXPECT_EQ(withoutFinal.finalThroughMs, 0);
}

TEST(RecordingRawTail, FinalsOncePerSubscriberAndSinceResubscribeResends) {
    QTemporaryDir dir;
    LiveService service(dir.path().toStdString());
    std::atomic<int64_t> now{0};
    RecordingLiveTest::clock(service, [&] { return now.load(); });
    service.publish(record(0, 2));
    service.publish(record(1, 3, 1000, true));
    std::vector<heatmap::ChunkFrame> a, b, reconnect;
    auto sink = [](auto& frames) {
        return [&frames](const auto&, const auto&, const RawTailFrame& frame) {
            frames.push_back(heatmap::decodeChunk(*frame.bytes)); return true;
        };
    };
    auto first = service.subscribeRaw({"BTC-USD", {"hmc2.near"}, 1, epoch}, sink(a));
    auto second = service.subscribeRaw({"BTC-USD", {"hmc2.near"}, 2, epoch+60000}, sink(b));
    ASSERT_TRUE(first); ASSERT_TRUE(second);
    RecordingLiveTest::poll(service);
    ASSERT_EQ(a.size(), 1); ASSERT_EQ(b.size(), 1);
    EXPECT_EQ(a[0].columns.columns.size(), 2); EXPECT_EQ(b[0].columns.columns.size(), 1);
    service.publish(record(1, 4, 2000, true));
    now = kLiveCadenceDefaultMs - 1; RecordingLiveTest::poll(service);
    EXPECT_EQ(a.size(), 1); EXPECT_EQ(b.size(), 1);
    now = kLiveCadenceDefaultMs; RecordingLiveTest::poll(service);
    ASSERT_EQ(a.size(), 2); ASSERT_EQ(b.size(), 2);
    ASSERT_EQ(a[1].columns.columns.size(), 1); ASSERT_EQ(b[1].columns.columns.size(), 1);
    EXPECT_TRUE(a[1].columns.columns.front().flags & kProvisional);
    EXPECT_EQ(service.diagnostics().rawEncodings, 3); // two initial variants, one shared update
    first->active = false;
    auto resub = service.subscribeRaw({"BTC-USD", {"hmc2.near"}, 3, epoch}, sink(reconnect));
    RecordingLiveTest::poll(service);
    ASSERT_EQ(reconnect.size(), 1);
    ASSERT_EQ(reconnect[0].columns.columns.size(), 2);
    EXPECT_EQ(reconnect[0].columns.columns.front().bucketStartMs, epoch);
    EXPECT_FALSE(reconnect[0].columns.columns.front().flags & kProvisional);
    service.shutdown();
}

// 2026-10-01 (owner: live rate 500 ms): the recorder publishes every 500 ms and
// the worker turns about every 105 ms (100 ms wait plus work; S6d measured
// frames 1.057 s apart at the former 1 s). In steady state (open-minute
// provisionals only: no finals to deliver, no byte-budget refusal) each
// publication must go out at the first turn after it. A worker cadence equal to
// the publish interval rounds up to 525 ms here, falls behind the recorder,
// skips publications and lets the age walk through a whole interval.
TEST(RecordingRawTail, SteadyStatePublicationGoesOutWithinOneWorkerTurn) {
    QTemporaryDir dir;
    LiveService service(dir.path().toStdString(), liveCadenceMs(kLivePublishDefaultMs));
    std::atomic<int64_t> now{-1};
    RecordingLiveTest::clock(service, [&] { return now.load(); });
    std::vector<std::pair<int64_t, int64_t>> sent; // (turn time, observedMs of the open minute)
    auto sub = service.subscribeRaw({"BTC-USD", {"hmc2.near"}, 1, epoch}, [&](auto&, auto&, const RawTailFrame& frame) {
        sent.emplace_back(now.load(), heatmap::decodeChunk(*frame.bytes).columns.columns.back().observedMs);
        return true;
    });
    ASSERT_TRUE(sub);
    constexpr int64_t kTurnMs = 105, kSpanMs = 30'000;
    int64_t nextPublish = 0, publications = 0;
    for (int64_t t = 0; t < kSpanMs; t += kTurnMs) {
        for (; nextPublish <= t; nextPublish += kLivePublishDefaultMs, ++publications)
            service.publish(record(0, 2, uint32_t(nextPublish + 1), true)); // observed = publish time + 1
        now = t; RecordingLiveTest::poll(service);
    }
    service.shutdown();
    ASSERT_EQ(int64_t(sent.size()), publications) << "a frame per publication: 2 per second, none skipped";
    int64_t maxAge = 0;
    for (const auto& [at, observed] : sent) maxAge = std::max(maxAge, at - (observed - 1));
    EXPECT_LT(maxAge, kTurnMs) << "each publication goes out at the first worker turn after it";
}

TEST(RecordingRawTail, BusyCoalescesLatestAndBacksOffWithoutLosingFinals) {
    QTemporaryDir dir;
    LiveService service(dir.path().toStdString());
    std::atomic<int64_t> now{0};
    RecordingLiveTest::clock(service, [&] { return now.load(); });
    bool busy = false;
    std::vector<heatmap::ChunkFrame> delivered;
    std::vector<int64_t> attempts;
    auto subscription = service.subscribeRaw({"BTC-USD", {"hmc2.near"}, 1, epoch},
        [&](const auto&, const auto&, const RawTailFrame& frame) {
            attempts.push_back(now.load());
            if (busy) return false;
            delivered.push_back(heatmap::decodeChunk(*frame.bytes)); return true;
        });
    service.publish(record(0, 2)); service.publish(record(1, 3, 1000, true));
    RecordingLiveTest::poll(service);
    ASSERT_EQ(delivered.size(), 1);
    busy = true;
    constexpr int64_t B = kLiveCadenceDefaultMs; // the configured base
    constexpr int64_t retry = 7 * B + std::min<int64_t>(kLiveCadenceMaxMs, 8 * B);
    service.publish(record(1, 3)); service.publish(record(2, 4, 1000, true));
    now = B; RecordingLiveTest::poll(service); // refused -> 2B
    now = 3 * B - 1; RecordingLiveTest::poll(service); EXPECT_EQ(attempts.size(), 2);
    now = 3 * B; RecordingLiveTest::poll(service); // refused -> 4B
    service.publish(record(2, 4)); service.publish(record(3, 5, 1000, true));
    now = 7 * B - 1; RecordingLiveTest::poll(service); EXPECT_EQ(attempts.size(), 3);
    now = 7 * B; RecordingLiveTest::poll(service); // refused -> 8B (5 s cap)
    service.publish(record(3, 5)); service.publish(record(4, 6, 2345, true));
    busy = false;
    now = retry - 1; RecordingLiveTest::poll(service); EXPECT_EQ(delivered.size(), 1);
    now = retry; RecordingLiveTest::poll(service);
    EXPECT_EQ(attempts, (std::vector<int64_t>{0, B, 3 * B, 7 * B, retry}));
    ASSERT_EQ(delivered.size(), 2);
    const auto& latest = delivered.back();
    ASSERT_EQ(latest.columns.columns.size(), 4);
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(latest.columns.columns[i].bucketStartMs, epoch+(i+1)*60000);
        EXPECT_FALSE(latest.columns.columns[i].flags & kProvisional);
    }
    EXPECT_EQ(latest.columns.columns.back().bucketStartMs, epoch+4*60000);
    EXPECT_EQ(latest.columns.columns.back().observedMs, 2345);
    EXPECT_EQ(latest.state.revision, 8);
    service.publish(record(4, 7, 3456, true));
    now = retry + B - 1; RecordingLiveTest::poll(service); EXPECT_EQ(delivered.size(), 2);
    now = retry + B; RecordingLiveTest::poll(service);
    ASSERT_EQ(delivered.size(), 3);
    EXPECT_EQ(delivered.back().columns.columns.size(), 1); // all refused finals were accepted exactly once
    service.shutdown();
}

TEST(RecordingRawTail, RetentionAndOnePreviousChunkBoundLeaveOlderFinalsForChunks) {
    LiveCache cache;
    for (int i = 0; i < 30; ++i) cache.publish(record(i, 2));
    cache.publish(record(30, 3, 1000, true));
    RawTailBuilder builder;
    const auto replay = heatmap::decodeChunk(*builder.build("BTC-USD", "hmc2.near", cache.snapshot("BTC-USD", "near"), epoch).bytes);
    ASSERT_EQ(replay.columns.columns.size(), 17);
    EXPECT_EQ(replay.columns.startMs, epoch+14*60000);
    EXPECT_EQ(heatmap::bucketState(replay.columns, epoch+13*60000), heatmap::BucketState::NotLoaded);
    cache.publish(record(180, 4, 1000, true)); // old held finals are now outside the bounded extent
    const auto jumped = heatmap::decodeChunk(*builder.build("BTC-USD", "hmc2.near", cache.snapshot("BTC-USD", "near"), epoch).bytes);
    EXPECT_EQ(jumped.columns.columns.size(), 1);
    EXPECT_EQ(jumped.key.startMs, epoch+180*60000);
}

TEST(RecordingRawTail, SeparateSubscriptionCapAndShutdownWithDeliveryInFlight) {
    QTemporaryDir dir;
    LiveService service(dir.path().toStdString());
    auto released = service.subscribeRaw({"BTC-USD", {"hmc2.near"}, 0, epoch}, [](auto&, auto&, auto&) { return true; });
    std::weak_ptr<LiveService::RawSubscription> weak = released;
    RecordingLiveTest::poll(service);
    released.reset();
    RecordingLiveTest::poll(service);
    EXPECT_TRUE(weak.expired()); // scratch job storage must not become a subscription owner
    std::vector<std::shared_ptr<LiveService::Subscription>> legacy;
    for (int i = 0; i < 64; ++i) legacy.push_back(service.subscribe(view(), [](auto&, auto&) { return true; }));
    std::vector<std::shared_ptr<LiveService::RawSubscription>> raw;
    for (size_t i = 0; i < LiveService::kMaxRawSubscriptions; ++i) {
        auto sub = service.subscribeRaw({"BTC-USD", {"hmc2.near"}, i, epoch}, [](auto&, auto&, auto&) { return true; });
        ASSERT_TRUE(sub); raw.push_back(sub);
    }
    EXPECT_FALSE(service.subscribeRaw({"BTC-USD", {"hmc2.near"}, 129, epoch}, [](auto&, auto&, auto&) { return true; }));
    for (const auto& sub : raw) sub->active = false;
    for (const auto& sub : legacy) sub->active = false;
    std::promise<void> entered, release;
    auto gate = release.get_future().share();
    auto sub = service.subscribeRaw({"BTC-USD", {"hmc2.near"}, 130, epoch}, [&](auto&, auto&, auto&) {
        entered.set_value(); gate.wait(); return true;
    });
    service.publish(record(0, 3, 1000, true));
    entered.get_future().get();
    auto stopped = std::async(std::launch::async, [&] { service.shutdown(); });
    while (sub->active.load()) std::this_thread::yield();
    EXPECT_EQ(stopped.wait_for(std::chrono::milliseconds(0)), std::future_status::timeout);
    release.set_value(); stopped.get();
    EXPECT_FALSE(service.subscribeRaw({"BTC-USD", {"hmc2.near"}, 131, epoch}, [](auto&, auto&, auto&) { return true; }));
    service.start(); EXPECT_FALSE(sub->active.load()); service.shutdown();
}

TEST(RecordingRawTail, RolloverPreservesCompletePendingAndPartialOpenFlags) {
    LiveCache cache;
    RawTailBuilder builder;
    cache.publish(record(59, 2, 59000, true));
    auto build = [&] { return heatmap::decodeChunk(*builder.build("BTC-USD", "hmc2.near",
        cache.snapshot("BTC-USD", "near"), epoch).bytes); };
    EXPECT_TRUE(build().columns.columns.back().flags & kPartial);
    // Recorder closes minute 59, then waits for its next publishOpen at minute
    // 60. The latest provisional is complete throughout that rollover window.
    cache.publish(record(59, 2, 60000, true));
    const auto rollover = build();
    ASSERT_EQ(rollover.columns.columns.size(), 1);
    EXPECT_EQ(rollover.columns.endMs, epoch+60*60000);
    EXPECT_EQ(rollover.columns.columns.back().observedMs, 60000);
    EXPECT_EQ(rollover.columns.columns.back().flags, kProvisional);
    cache.publish(record(60, 3, 1000, true, epoch+59*60000));
    const auto next = build();
    ASSERT_EQ(next.columns.columns.size(), 2);
    EXPECT_EQ(next.columns.columns.front().flags, kProvisional);
    EXPECT_EQ(next.columns.columns.back().flags, kProvisional | kPartial);
}

TEST(RecordingRawTail, ByteBudgetAccountsForAllAdmittedEnvelopes) {
    LiveWriteBudget budget;
    EXPECT_TRUE(budget.tryAcquire(100));
    EXPECT_TRUE(budget.tryAcquire(kRawLiveByteBudget-100));
    EXPECT_EQ(budget.bytes(), kRawLiveByteBudget);
    EXPECT_FALSE(budget.tryAcquire(1));
    EXPECT_FALSE(budget.tryAcquire(std::numeric_limits<size_t>::max()));
    budget.release(100);
    EXPECT_EQ(budget.bytes(), kRawLiveByteBudget-100);
    EXPECT_TRUE(budget.tryAcquire(100));
    budget.release(kRawLiveByteBudget-100);
    EXPECT_EQ(budget.bytes(), 100);
    budget.release(100);
    EXPECT_EQ(budget.bytes(), 0);
}

TEST(RecordingRawTail, EmptyRevisionIsConsumedWithoutBackoff) {
    QTemporaryDir dir;
    LiveService service(dir.path().toStdString());
    std::atomic<int64_t> now{-1};
    RecordingLiveTest::clock(service, [&] { return now.load(); });
    service.publish(record(0, 2));
    std::vector<heatmap::ChunkFrame> frames;
    auto sub = service.subscribeRaw({"BTC-USD", {"hmc2.near"}, 1, epoch+60000},
        [&](auto&, auto&, const RawTailFrame& frame) {
            frames.push_back(heatmap::decodeChunk(*frame.bytes)); return true;
        });
    now = 0; RecordingLiveTest::poll(service);
    EXPECT_EQ(service.diagnostics().rawBuilds, 1);
    EXPECT_TRUE(frames.empty());
    now = 2000; RecordingLiveTest::poll(service);
    EXPECT_EQ(service.diagnostics().rawBuilds, 1); // consumed, even after an old refusal's delay
    service.publish(record(1, 3, 1000, true));
    now = 2001; RecordingLiveTest::poll(service);
    ASSERT_EQ(frames.size(), 1); // no 2/4 second backoff from empty work
    EXPECT_EQ(frames.back().state.revision, 2);
    service.shutdown();
}

TEST(RecordingRawTail, FinalColumnMatchesRecordingLoaderIncludingFilteredEntryBase) {
    for (const auto observed : {30000, 60000}) {
        auto final = std::make_shared<Hmc2Record>(*record(0, 2, observed));
        // A filtered row still participates in fromRecording's base selection.
        final->entries = {{70, false, encodeSize(9), 0}, {99, false, encodeSize(2), 0},
                          {99, true, encodeSize(3), 0}, {104, true, encodeSize(4), 0}};
        final->header.configHash = 42;
        final->header.rowTickUnits = 25;
        final->header.priceScale = 100;
        LiveCache cache;
        cache.publish(final);
        cache.publish(record(1, 3, 1000, true));
        RawTailBuilder builder;
        auto actual = heatmap::decodeChunk(*builder.build("BTC-USD", "hmc2.near",
            cache.snapshot("BTC-USD", "near"), epoch).bytes);
        auto expected = actual;
        expected.columns.columns.front() = heatmap::fromRecording(*final);
        // Comparing encoded frames covers every column/grid/coverage/entry field.
        EXPECT_EQ(heatmap::encodeChunk(actual), heatmap::encodeChunk(expected));
    }
}

TEST(RecordingRawTail, SharedVariantCacheHasAtMostSeventeenFramesPerRevision) {
    LiveCache cache;
    for (int i = 0; i < 16; ++i) cache.publish(record(i, 2));
    cache.publish(record(16, 3, 1000, true));
    RawTailBuilder builder;
    const auto snapshot = cache.snapshot("BTC-USD", "near");
    std::set<const void*> frames;
    for (int pass = 0; pass < 2; ++pass) for (int since = 0; since <= 17; ++since)
        frames.insert(builder.build("BTC-USD", "hmc2.near", snapshot, epoch+since*60000).bytes.get());
    EXPECT_EQ(frames.size(), 17);
    EXPECT_EQ(builder.encodings(), 17);
    cache.publish(record(16, 4, 2000, true));
    const auto updated = builder.build("BTC-USD", "hmc2.near", cache.snapshot("BTC-USD", "near"), epoch);
    EXPECT_EQ(builder.encodings(), 18);
    EXPECT_EQ(heatmap::decodeChunk(*updated.bytes).columns.columns.back().observedMs, 2000);
}

namespace {
struct RawMessageCapture {
    static inline RawMessageCapture* active = nullptr;
    std::mutex mutex;
    std::vector<QString> messages;
    QtMessageHandler previous;
    RawMessageCapture() {
        active = this;
        previous = qInstallMessageHandler([](QtMsgType type, const QMessageLogContext&, const QString& message) {
            if (type == QtWarningMsg || type == QtCriticalMsg) {
                std::lock_guard lock(active->mutex);
                active->messages.push_back(message);
            }
        });
    }
    ~RawMessageCapture() { qInstallMessageHandler(previous); active = nullptr; }
    bool contains(const QString& part) {
        std::lock_guard lock(mutex);
        return std::any_of(messages.begin(), messages.end(), [&](const auto& message) { return message.contains(part); });
    }
};
RecordPtr largeRawRecord(int minute, size_t count, bool provisional) {
    auto r = std::make_shared<Hmc2Record>(*record(minute, 2, 60000, provisional, epoch));
    r->entries.clear();
    r->entries.reserve(count);
    uint32_t random = uint32_t(minute+1);
    auto next = [&] { random ^= random << 13; random ^= random >> 17; random ^= random << 5; return random; };
    int64_t row = 0;
    for (size_t i = 0; i < count; ++i) {
        row += 1 + next()%4096;
        r->entries.push_back({row, false, uint16_t(1+next()%32767), 0});
    }
    r->bidRowLo = 0; r->bidRowHi = row;
    r->askRowLo = 1; r->askRowHi = 0;
    return r;
}
}

TEST(RecordingRawTail, OversizedWorkerTrimsPendingLogsAndKeepsFinalMarkers) {
    RawMessageCapture logs;
    QTemporaryDir dir;
    LiveService service(dir.path().toStdString());
    std::atomic<int64_t> now{-1};
    RecordingLiveTest::clock(service, [&] { return now.load(); });
    service.publish(record(0, 2));
    for (int i = 1; i <= 4; ++i) {
        auto r = std::make_shared<Hmc2Record>(*largeRawRecord(i, 120000, true));
        r->committedThroughMs = epoch+60000;
        service.publish(r);
    }
    std::vector<heatmap::ChunkFrame> frames;
    std::vector<int64_t> markers;
    LiveWriteBudget budget;
    auto sub = service.subscribeRaw({"BTC-USD", {"hmc2.near"}, 1, epoch},
        [&](auto&, auto&, const RawTailFrame& frame) {
            const auto bytes = frame.bytes->size()+14;
            if (!budget.tryAcquire(bytes)) return false;
            frames.push_back(heatmap::decodeChunk(*frame.bytes));
            markers.push_back(frame.finalThroughMs);
            budget.release(bytes); return true;
        });
    now = 0; RecordingLiveTest::poll(service);
    ASSERT_EQ(frames.size(), 1);
    const auto& columns = frames.front().columns;
    EXPECT_LT(columns.columns.size(), 5); // fixture really exceeded the byte limit
    EXPECT_EQ(columns.columns.front().bucketStartMs, epoch); // final prefix retained
    EXPECT_EQ(columns.columns.back().bucketStartMs, epoch+4*60000);
    EXPECT_EQ(heatmap::bucketState(columns, epoch+60000), heatmap::BucketState::NotLoaded);
    EXPECT_EQ(markers.front(), epoch+60000);
    EXPECT_TRUE(logs.contains("symbol=BTC-USD source=hmc2.near bytes="));
    service.publish(record(4, 3, 60000, true, epoch+60000));
    now = kLiveCadenceDefaultMs; RecordingLiveTest::poll(service);
    ASSERT_EQ(frames.size(), 2);
    for (const auto& column : frames.back().columns.columns) EXPECT_TRUE(column.flags & kProvisional);
    EXPECT_EQ(markers.back(), 0); // retained final delivered once
    service.shutdown();

    // A single very large final cannot fit. It must not advance the marker;
    // the available open minute can still be sent and the final comes via chunks.
    LiveService other(dir.path().toStdString());
    now = -1;
    RecordingLiveTest::clock(other, [&] { return now.load(); });
    other.publish(largeRawRecord(0, 400000, false));
    other.publish(record(1, 2, 1000, true));
    frames.clear(); markers.clear();
    auto second = other.subscribeRaw({"BTC-USD", {"hmc2.near"}, 2, epoch}, sub->deliver);
    now = 0; RecordingLiveTest::poll(other);
    ASSERT_EQ(frames.size(), 1);
    ASSERT_EQ(frames.front().columns.columns.size(), 1);
    EXPECT_EQ(frames.front().columns.columns.front().bucketStartMs, epoch+60000);
    EXPECT_EQ(markers.front(), 0);
    other.shutdown();
}

TEST(RecordingRawTail, AllocationFailureAcrossRawWorkerBacksOffAndRecovers) {
    RawMessageCapture logs;
    for (const std::string stage : {"collect", "state", "snapshot"}) {
        SCOPED_TRACE(stage);
        QTemporaryDir dir;
        LiveService service(dir.path().toStdString());
        std::atomic<int64_t> now{-1};
        std::atomic_bool fail{true};
        RecordingLiveTest::clock(service, [&] { return now.load(); }, [&](const char* at) {
            if (stage == at && fail) throw std::bad_alloc();
        });
        service.publish(record(0, 2, 1000, true));
        std::atomic<int> deliveries{0};
        auto sub = service.subscribeRaw({"BTC-USD", {"hmc2.near"}, 1, epoch}, [&](auto&, auto&, auto&) {
            ++deliveries; return true;
        });
        now = 0; RecordingLiveTest::poll(service);
        EXPECT_EQ(service.diagnostics().rawFailures, 1);
        EXPECT_EQ(deliveries, 0);
        constexpr int64_t B = kLiveCadenceDefaultMs; // failures back off 2B, then 4B
        now = 2 * B - 1; RecordingLiveTest::poll(service);
        EXPECT_EQ(service.diagnostics().rawFailures, 1);
        now = 2 * B; RecordingLiveTest::poll(service);
        EXPECT_EQ(service.diagnostics().rawFailures, 2);
        now = 6 * B - 1; RecordingLiveTest::poll(service);
        EXPECT_EQ(service.diagnostics().rawFailures, 2);
        fail = false;
        now = 6 * B; RecordingLiveTest::poll(service);
        EXPECT_EQ(deliveries, 1);
        EXPECT_TRUE(sub->active);
        service.shutdown();
    }
    EXPECT_TRUE(logs.contains("Raw heatmap live worker retry:"));
    EXPECT_TRUE(logs.contains(QString("delayMs=%1").arg(4 * kLiveCadenceDefaultMs)));
}
