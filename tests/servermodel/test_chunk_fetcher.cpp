#include "protocol/SentinelStreamClient.hpp" // shared-frame metatype
#include "protocol/SentinelStreamClientTransport.hpp"
#include "FakeChunkTransport.hpp"
#include "heatmap/ChunkFetcher.hpp"
#include "heatmap/LocalChunkTransport.hpp"
#include "servermodel/RecordingChunks.hpp"
#include <QCoreApplication>
#include <QEventLoop>
#include <QTemporaryDir>
#include <gtest/gtest.h>
#include <future>
#include <condition_variable>
#include <atomic>
#include <set>

namespace {
using namespace heatmap;
constexpr int64_t epoch = recording::kHmc2MinMs;
ChunkKey key(int hour = 0, const std::string &source = "hmc2.deep", int64_t level = kMinuteMs) {
    return {"BTC-USD", source, level, epoch + hour * kHourMs};
}
ChunkFramePtr body(const ChunkKey &key, int minutes = 30, uint64_t salt = 0) {
    auto frame = std::make_shared<ChunkFrame>();
    frame->key = key;
    const auto end = recording::chunkEndMs(key);
    const auto through = std::min(end, key.startMs + minutes * kMinuteMs);
    recording::BookRecorder::Watermarks marks{through, through};
    frame->state = recording::chunkState(key, marks, minutes + 1);
    frame->columns = {key.symbol, recording::hmc2Layer(key), key.levelMs, key.startMs, end};
    if (through > key.startMs) frame->columns.scannedRanges = {{key.startMs, through}};
    if (minutes) {
        NativeColumn native;
        native.grid = {17, 100, 100.0};
        native.observedMs = key.levelMs;
        native.baseRow = 100;
        native.coverage[0] = {{100, 110, uint64_t(key.levelMs)}};
        native.coverage[1] = native.coverage[0];
        native.entries = {{packRowSide(101, 100, false), recording::encodeSize(1.0 + salt)},
                          {packRowSide(109, 100, true), recording::encodeSize(2.0 + salt)}};
        frame->columns.columns.push_back({key.startMs, uint64_t(key.levelMs), 0, {std::move(native)}});
    }
    frame->contentHash = chunkContentHash(encodeChunk(*frame));
    return frame;
}
ChunkFramePtr unchanged(const ChunkFramePtr &frame) {
    auto out = std::make_shared<ChunkFrame>();
    out->kind = ChunkKind::NotModified;
    out->key = frame->key;
    out->state = frame->state;
    out->contentHash = frame->contentHash;
    return out;
}
ChunkAvailability available(int64_t through = epoch + 30 * kMinuteMs) {
    ChunkAvailability a;
    a.symbol = "BTC-USD";
    a.chunkWireVersion = kChunkWireVersion;
    for (const auto &source : kChunkSources) {
        protocol::chunkwire::SourceInfo s;
        s.id = source.id;
        s.levels.push_back({kMinuteMs, kHourMs, through, epoch, through - kMinuteMs});
        if (source.hourLevel) s.levels.push_back({kHourMs, kDayMs, epoch, epoch, epoch});
        a.sources.push_back(std::move(s));
    }
    return a;
}
std::shared_ptr<const StoredChunk> put(ChunkStore &store, const ChunkFramePtr &frame) {
    return store.put(frame->key, {frame, &frame->columns}, frame->state, frame->contentHash);
}
// All fake completions and fetcher scheduling are MetaCall events, never timers.
// Drain enough turns for transport -> fetcher -> scheduler/listener -> chart.
void drain() {
    for (int i = 0; i < 20; ++i) QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
}
// Timeout is only a deadlock watchdog. Completion is driven by queued signals.
bool await(QEventLoop &loop, const std::function<bool()> &done) {
    if (done()) return true;
    QTimer watchdog;
    watchdog.setSingleShot(true);
    QObject::connect(&watchdog, &QTimer::timeout, &loop, &QEventLoop::quit);
    watchdog.start(5000);
    loop.exec();
    return done();
}
struct WorkerGate {
    std::promise<void> entered;
    std::mutex mutex;
    std::condition_variable_any changed;
    bool open = false;
    void wait(std::stop_token stop) {
        entered.set_value();
        std::unique_lock lock(mutex);
        changed.wait(lock, stop, [&] { return open; });
    }
    void release() {
        std::scoped_lock lock(mutex);
        open = true;
        changed.notify_all();
    }
};
recording::Hmc2Record localRecord(int minute = 0) {
    recording::Hmc2Record r;
    r.header = {"BTC-USD", "deep", kMinuteMs, 100., 100, {}, 7};
    r.bucketStartMs = epoch + minute * kMinuteMs;
    r.observedMs = kMinuteMs;
    r.bidRowLo = r.askRowLo = 100;
    r.bidRowHi = r.askRowHi = 110;
    r.entries = {{101, false, recording::encodeSize(2), 0, uint32_t(kMinuteMs)}};
    return r;
}
class Fetcher : public testing::Test {
protected:
    int argc = 1;
    char name[16] = "chunk-fetcher";
    char *argv[2] = {name, nullptr};
    QCoreApplication app{argc, argv};
    ChunkStore store;
    FakeChunkTransport transport;
    int64_t now = 1000;
    std::unique_ptr<ChunkFetcher> fetcher;
    std::vector<ChunkKey> failures;
    void create(std::function<size_t(const ChunkKey &)> estimate = {}) {
        ChunkFetcher::Options options;
        options.nowMs = [this] { return now; };
        options.estimateBytes = std::move(estimate);
        fetcher = std::make_unique<ChunkFetcher>(store, transport, options);
        QObject::connect(fetcher.get(), &ChunkFetcher::chunkFailed, &app,
                         [this](ChunkKey k, const QString &, const QString &) { failures.push_back(k); },
                         Qt::QueuedConnection);
    }
    void SetUp() override { create(); }
    void online(ChunkAvailability a = available()) {
        transport.goOnline(); transport.push(std::move(a)); drain();
    }
    void reconnect() { transport.goOffline(); drain(); online(); }
    void answer(const FakeChunkTransport::Request &request, int minutes = 30) {
        // Copy because processing replies may append the next batch.
        const auto copy = request;
        for (size_t i = 0; i < copy.starts.size(); ++i) transport.reply(copy.id, body(copy.key(i), minutes));
        drain();
    }
};

TEST_F(Fetcher, GlobalRamTwoChartsShareOneRequestAndDecodedBodyPerUniqueKey) {
    online();
    std::vector<ChunkKey> keys{key(), key(1), key(2)};
    store.setMaxBytes(4 * sparseBytes(body(key())->columns));
    fetcher->want(1, keys); drain();
    fetcher->want(2, keys); drain();
    ASSERT_EQ(transport.requests.size(), 1u);
    const auto request = transport.requests.front();
    ASSERT_EQ(request.starts.size(), keys.size());
    std::vector<ChunkFramePtr> frames;
    for (size_t i = 0; i < keys.size(); ++i) {
        frames.push_back(body(request.key(i)));
        transport.hold(request.id, frames.back());
    }
    while (!transport.held.empty()) transport.releaseHeld();
    drain();
    for (const auto &frame : frames) {
        ASSERT_TRUE(store.peek(frame->key));
        EXPECT_EQ(store.peek(frame->key)->columns.get(), &frame->columns) << "no per-chart copy";
    }
    fetcher->want(1, keys); fetcher->want(2, keys); drain();
    EXPECT_EQ(transport.requests.size(), 1u) << "cached current bodies are deduplicated too";
    EXPECT_EQ(store.stats().loads, keys.size());
    EXPECT_EQ(fetcher->stats().bodies, keys.size());
    EXPECT_LE(store.stats().bytes, store.stats().maxBytes);
    EXPECT_EQ(fetcher->stats().inFlightChunks, 0u);
}
TEST_F(Fetcher, BatchesBySeriesAndCapsChunksAndEstimatedBytes) {
    online();
    std::vector<ChunkKey> keys;
    for (int i = 0; i < 10; ++i) keys.push_back(key(i));
    fetcher->want(1, keys); drain();
    ASSERT_EQ(transport.requests.size(), 1u);
    EXPECT_EQ(transport.requests[0].starts.size(), 4u);
    EXPECT_LE(transport.requests[0].starts.size(), protocol::chunkwire::kMaxStarts);
    EXPECT_EQ(fetcher->stats().inFlightChunks, 4u);
    EXPECT_EQ(fetcher->stats().inFlightBytes, 16ull << 20);
    answer(transport.requests[0]);
    ASSERT_EQ(transport.requests.size(), 2u);
    EXPECT_EQ(transport.requests[1].starts.size(), 4u);
    answer(transport.requests[1]);
    ASSERT_EQ(transport.requests.size(), 3u);
    EXPECT_EQ(transport.requests[2].starts.size(), 2u);
    answer(transport.requests[2]);
    EXPECT_EQ(fetcher->stats().inFlightBytes, 0u);

    fetcher.reset(); create([](const ChunkKey &) { return 9ull << 20; });
    online();
    fetcher->want(1, {key(20), key(21), key(22, "hmc2.near")}); drain();
    EXPECT_EQ(fetcher->stats().inFlightChunks, 1u) << "byte cap binds before the four-chunk cap";
    EXPECT_EQ(fetcher->stats().inFlightBytes, 9ull << 20);
    EXPECT_EQ(transport.requests.back().starts.size(), 1u);
    const auto count = transport.requests.size();
    answer(transport.requests.back());
    ASSERT_EQ(transport.requests.size(), count + 1);
    EXPECT_EQ(transport.requests.back().key(0), key(21));
    EXPECT_EQ(fetcher->stats().inFlightBytes, 9ull << 20);
}
TEST_F(Fetcher, SeparatesSymbolsSourcesAndLevelsAndHonorsPriority) {
    online();
    const auto minuteNear = key(0, "hmc2.near"), hourDeep = key(0, "hmc2.deep", kHourMs);
    fetcher->want(1, {key(1), minuteNear}, 1);
    fetcher->want(2, {hourDeep}, 20);
    drain();
    ASSERT_EQ(transport.requests.size(), 3u);
    EXPECT_EQ(transport.requests[0].key(0), hourDeep);
    EXPECT_EQ(transport.requests[1].key(0), key(1));
    EXPECT_EQ(transport.requests[2].key(0), minuteNear);
    auto eth = key(); eth.symbol = "ETH-USD";
    fetcher->want(2, {eth}); drain();
    EXPECT_EQ(transport.requests.size(), 3u) << "no fresh availability for ETH";
    auto a = available(); a.symbol = "ETH-USD"; transport.push(a); drain();
    ASSERT_EQ(transport.requests.size(), 4u);
    EXPECT_EQ(transport.requests.back().key(0), eth);
}
TEST_F(Fetcher, BatchingDoesNotPromotePrefetchAboveAnotherSourcesVisibleKeys) {
    online();
    fetcher->want(1, {key()}, 20);
    fetcher->want(1, {key(1), key(2), key(3)}, 0);
    fetcher->want(2, {key(0, "hmc2.near"), key(1, "hmc2.near"), key(2, "hmc2.near")}, 10);
    drain();
    ASSERT_EQ(transport.requests.size(), 2u);
    EXPECT_EQ(transport.requests[0].starts, std::vector<int64_t>{epoch});
    EXPECT_EQ(transport.requests[1].source, "hmc2.near");
    EXPECT_EQ(transport.requests[1].starts.size(), 3u);
}
TEST_F(Fetcher, BusyRetriesWithBoundedExponentialBackoff) {
    online(); fetcher->want(1, {key()}); drain();
    for (const int delay : {100, 200, 400, 800, 1600, 3200, 5000, 5000}) {
        const auto count = transport.requests.size();
        transport.error(transport.requests.back().id, key(), "busy"); drain();
        EXPECT_EQ(fetcher->stats().inFlightChunks, 0u);
        now += delay - 1; fetcher->pump(); drain();
        EXPECT_EQ(transport.requests.size(), count);
        ++now; fetcher->pump(); drain();
        ASSERT_EQ(transport.requests.size(), count + 1);
    }
    answer(transport.requests.back());
    EXPECT_TRUE(failures.empty());
    EXPECT_EQ(fetcher->stats().retries, 8u);
}
TEST_F(Fetcher, SupersededIsIgnoredAndReleasesItsSlot) {
    online(); fetcher->want(1, {key()}); drain();
    transport.error(transport.requests.back().id, key(), "superseded"); drain();
    now += 60'000; fetcher->pump(); drain();
    EXPECT_EQ(transport.requests.size(), 1u);
    EXPECT_TRUE(failures.empty());
    EXPECT_EQ(fetcher->stats().inFlightChunks, 0u);
    EXPECT_EQ(fetcher->stats().inFlightBytes, 0u);
}
TEST_F(Fetcher, KeylessMalformedFailsOnlyKeysOfThatRequest) {
    online(); fetcher->want(1, {key(), key(1), key(0, "hmc2.near")}); drain();
    ASSERT_EQ(transport.requests.size(), 2u);
    const auto first = transport.requests.front();
    transport.error(first.id, {}, "malformed"); drain();
    ASSERT_EQ(failures.size(), 2u);
    EXPECT_EQ(failures[0], first.key(0)); EXPECT_EQ(failures[1], first.key(1));
    EXPECT_EQ(fetcher->stats().inFlightChunks, 1u);
    answer(transport.requests.back());
    EXPECT_EQ(fetcher->stats().inFlightChunks, 0u);
}
TEST_F(Fetcher, MidFlightDisconnectWaitsForConnectAndFreshAvailabilityThenResends) {
    online(); fetcher->want(1, {key(), key(1)}); drain();
    const auto lost = transport.requests.front();
    transport.hold(lost.id, body(key()));
    transport.goOffline(); drain();
    EXPECT_EQ(fetcher->stats().inFlightChunks, 0u);
    transport.push(available()); drain();
    EXPECT_EQ(transport.requests.size(), 1u) << "availability while disconnected is stale";
    transport.goOnline(); drain();
    EXPECT_EQ(transport.requests.size(), 1u) << "connected alone must not resend";
    transport.releaseHeld(); drain();
    EXPECT_FALSE(store.contains(key())) << "late reply from the lost request must not store";
    transport.push(available()); drain();
    ASSERT_EQ(transport.requests.size(), 2u);
    EXPECT_EQ(transport.requests.back().starts, lost.starts);
    EXPECT_NE(transport.requests.back().id, lost.id);
    answer(transport.requests.back());
    EXPECT_EQ(store.stats().loads, 2u);
    EXPECT_EQ(fetcher->stats().inFlightChunks, 0u);
}
TEST_F(Fetcher, NotModifiedAfterReconnectKeepsGenerationAndDoesNotRevise) {
    online(); fetcher->want(1, {key()}); drain();
    const auto frame = body(key()); transport.reply(transport.requests.back().id, frame); drain();
    const auto before = store.peek(key());
    ASSERT_TRUE(before);
    int revised = 0;
    QObject::connect(fetcher.get(), &ChunkFetcher::chunkRevised, &app,
                     [&](ChunkKey, quint64) { ++revised; }, Qt::QueuedConnection);
    reconnect();
    ASSERT_EQ(transport.requests.size(), 2u);
    ASSERT_EQ(transport.requests.back().hashes.size(), 1u);
    EXPECT_EQ(transport.requests.back().hashes[0], frame->contentHash);
    transport.reply(transport.requests.back().id, unchanged(frame)); drain();
    ASSERT_TRUE(store.peek(key()));
    EXPECT_EQ(store.peek(key())->generation, before->generation);
    EXPECT_EQ(store.peek(key())->columns, before->columns);
    EXPECT_EQ(revised, 0);
    EXPECT_EQ(store.stats().loads, 1u);
    EXPECT_EQ(fetcher->stats().notModified, 1u);
}
TEST_F(Fetcher, ChangedOpenChunkRevisesOnceAndBothChartsHearIt) {
    online(); fetcher->want(1, {key()}); fetcher->want(2, {key()}); drain();
    answer(transport.requests.back());
    const auto before = store.peek(key())->generation;
    std::vector<quint64> chart1, chart2;
    QObject c1, c2;
    QObject::connect(fetcher.get(), &ChunkFetcher::chunkRevised, &c1,
                     [&](ChunkKey k, quint64 g) { if (k == key()) chart1.push_back(g); }, Qt::QueuedConnection);
    QObject::connect(fetcher.get(), &ChunkFetcher::chunkRevised, &c2,
                     [&](ChunkKey k, quint64 g) { if (k == key()) chart2.push_back(g); }, Qt::QueuedConnection);
    reconnect();
    const auto revised = body(key(), 45);
    transport.reply(transport.requests.back().id, revised); drain();
    const auto after = store.peek(key())->generation;
    EXPECT_NE(after, before);
    EXPECT_EQ(chart1, std::vector<quint64>{after});
    EXPECT_EQ(chart2, chart1);
    EXPECT_EQ(store.revisionCount(), 1u);
    transport.reply(transport.requests.back().id, revised); drain();
    EXPECT_EQ(chart1.size(), 1u) << "duplicate completion cannot revise twice";
}
TEST_F(Fetcher, CommittedThroughAdvanceRefreshesOnlyWantedOpenChunksOfThatLevel) {
    online();
    fetcher->want(1, {key(), key(1), key(2), key(0, "hmc2.deep", kHourMs)}); drain();
    const auto requests = transport.requests;
    for (const auto &r : requests) for (size_t i = 0; i < r.starts.size(); ++i)
        transport.reply(r.id, body(r.key(i), r.key(i) == key(2) ? 60 : r.levelMs == kHourMs ? 60 : 30));
    drain();
    fetcher->release(1, {key(1)});
    transport.push(available(epoch + 31 * kMinuteMs)); drain();
    ASSERT_EQ(transport.requests.size(), requests.size() + 1);
    const auto refresh = transport.requests.back();
    EXPECT_EQ(refresh.starts, std::vector<int64_t>{epoch});
    EXPECT_EQ(refresh.levelMs, kMinuteMs);
    transport.reply(refresh.id, body(key(), 31)); drain();
    transport.push(available(epoch + 31 * kMinuteMs)); drain();
    EXPECT_EQ(transport.requests.size(), requests.size() + 1) << "same cutoff is not an advance";
}
TEST_F(Fetcher, AdvanceDuringFlightSchedulesOneFollowupForOlderBody) {
    online(); fetcher->want(1, {key()}); drain();
    transport.push(available(epoch + 40 * kMinuteMs)); drain();
    EXPECT_EQ(transport.requests.size(), 1u) << "no duplicate in-flight key";
    answer(transport.requests.front(), 30);
    ASSERT_EQ(transport.requests.size(), 2u);
    answer(transport.requests.back(), 40);
    EXPECT_EQ(transport.requests.size(), 2u);
}
TEST_F(Fetcher, SealGivesOneNewGenerationAndIsNotRevalidatedOnReconnect) {
    online(); fetcher->want(1, {key()}); drain(); answer(transport.requests.back());
    const auto open = store.peek(key())->generation;
    transport.push(available(epoch + kHourMs)); drain();
    answer(transport.requests.back(), 60);
    const auto sealed = store.peek(key());
    ASSERT_TRUE(sealed->sealed);
    EXPECT_NE(sealed->generation, open);
    EXPECT_EQ(store.revisionCount(), 1u);
    reconnect();
    EXPECT_EQ(transport.requests.size(), 2u);
    EXPECT_EQ(store.peek(key())->generation, sealed->generation);
}
TEST_F(Fetcher, WireVersionMismatchClearsAndRejectsOutstandingReplies) {
    online(); fetcher->want(1, {key()}); drain(); answer(transport.requests.back());
    fetcher->want(1, {key(1)}); drain();
    const auto pending = transport.requests.back();
    auto bad = available(); ++bad.chunkWireVersion; transport.push(bad); drain();
    EXPECT_EQ(store.stats().entries, 0u);
    EXPECT_EQ(store.generationOf(key()), 0u);
    EXPECT_EQ(fetcher->stats().inFlightChunks, 0u);
    transport.reply(pending.id, body(key(1))); drain();
    EXPECT_EQ(store.stats().entries, 0u);
    const auto count = transport.requests.size(); fetcher->pump(); drain();
    EXPECT_EQ(transport.requests.size(), count);
}
TEST_F(Fetcher, HostChangeClearsSealedChunksAndRequiresFreshAvailability) {
    online(); fetcher->want(1, {key()}); drain(); answer(transport.requests.back(), 60);
    fetcher->hostChanged(); drain();
    EXPECT_FALSE(store.contains(key()));
    EXPECT_EQ(transport.requests.size(), 1u);
    transport.push(available()); drain();
    ASSERT_EQ(transport.requests.size(), 2u);
    EXPECT_FALSE(transport.requests.back().hashes.front());
}
TEST_F(Fetcher, ReleaseDropsPendingWorkButAnotherChartKeepsItsInterest) {
    online();
    std::vector<ChunkKey> keys; for (int i = 0; i < 6; ++i) keys.push_back(key(i));
    fetcher->want(1, keys); fetcher->want(2, {key(5)}); drain();
    fetcher->release(1); drain();
    answer(transport.requests.front());
    ASSERT_EQ(transport.requests.size(), 2u);
    EXPECT_EQ(transport.requests.back().starts, std::vector<int64_t>{key(5).startMs});
}
TEST_F(Fetcher, NotModifiedRetainsItsBodyAcrossStoreEviction) {
    online(); fetcher->want(1, {key()}); drain();
    const auto frame = body(key()); transport.reply(transport.requests.back().id, frame); drain();
    const auto generation = store.generationOf(key());
    reconnect();
    put(store, body(key(1), 60)); store.setMaxBytes(1);
    ASSERT_FALSE(store.contains(key()));
    store.setMaxBytes(512ull << 20);
    transport.reply(transport.requests.back().id, unchanged(frame)); drain();
    ASSERT_TRUE(store.contains(key()));
    EXPECT_EQ(store.peek(key())->columns.get(), &frame->columns);
    EXPECT_EQ(store.generationOf(key()), generation);
    EXPECT_TRUE(failures.empty());
}

TEST_F(Fetcher, AdapterForwardsPublicClientSignalsAndDropsPreviousHostEvents) {
    fetcher.reset();
    SentinelStreamClient client("127.0.0.1", "1"), replacement("127.0.0.1", "2");
    protocol::SentinelStreamClientTransport adapter(client);
    ChunkFetcher remote(store, adapter);
    std::vector<ChunkKey> failed;
    QObject::connect(&remote, &ChunkFetcher::chunkFailed, &app,
                     [&](ChunkKey k, QString, QString) { failed.push_back(k); }, Qt::QueuedConnection);
    client.connected(); client.heatmapAvailabilityReceived(available()); drain();
    remote.want(1, {key(), key(1)}); drain();
    client.heatmapChunkReceived(1, body(key()));
    client.heatmapChunkFailed({1, {}, "malformed", "broken second frame"}); drain();
    EXPECT_TRUE(store.contains(key()));
    EXPECT_EQ(failed, std::vector<ChunkKey>{key(1)});
    client.errorOccurred("heatmap chunk wire version mismatch"); drain();
    EXPECT_FALSE(store.contains(key()));
    client.heatmapAvailabilityReceived(available()); drain();
    // Events already queued from the old host must also be refused on replacement.
    client.heatmapChunkReceived(2, body(key()));
    adapter.setClient(replacement); drain();
    EXPECT_FALSE(store.contains(key()));
    replacement.connected(); replacement.heatmapAvailabilityReceived(available()); drain();
    replacement.heatmapChunkReceived(1, body(key())); drain();
    EXPECT_TRUE(store.contains(key())) << "new client's wire id 1 is remapped to a fresh transport id";
}

TEST_F(Fetcher, LocalTransportReadsSyntheticHmc2WithoutWriterLockAndReusesHash) {
    fetcher.reset();
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto root = dir.path().toStdString();
    // Keep the actual exclusive writer alive throughout the asynchronous read.
    recording::Hmc2Store writer(root);
    recording::Hmc2Record record;
    record.header = {"BTC-USD", "deep", kMinuteMs, 100., 100, {}, 7};
    record.bucketStartMs = epoch;
    record.observedMs = kMinuteMs;
    record.bidRowLo = record.askRowLo = 100;
    record.bidRowHi = record.askRowHi = 110;
    record.entries = {{101, false, recording::encodeSize(2), 0, uint32_t(kMinuteMs)},
                      {109, true, recording::encodeSize(3), 0, uint32_t(kMinuteMs)}};
    writer.append(record);
    LocalChunkTransport local(root);
    ChunkFetcher reader(store, local);
    QEventLoop loop;
    QTimer watchdog;
    watchdog.setSingleShot(true);
    QObject::connect(&watchdog, &QTimer::timeout, &loop, &QEventLoop::quit);
    std::vector<quint64> generations;
    std::vector<QString> errors;
    QObject::connect(&reader, &ChunkFetcher::chunkStored, &loop, [&](ChunkKey, quint64 generation) {
        generations.push_back(generation); loop.quit();
    }, Qt::QueuedConnection);
    QObject::connect(&reader, &ChunkFetcher::chunkFailed, &loop, [&](ChunkKey, QString code, QString) {
        errors.push_back(code); loop.quit();
    }, Qt::QueuedConnection);
    reader.want(1, {key()});
    local.start({"BTC-USD"});
    watchdog.start(5000); loop.exec(); watchdog.stop();
    ASSERT_TRUE(errors.empty()); ASSERT_EQ(generations.size(), 1u);
    const auto loaded = store.peek(key()); ASSERT_TRUE(loaded);
    EXPECT_FALSE(loaded->sealed);
    EXPECT_EQ(loaded->committedThroughMs, epoch + kMinuteMs);
    ASSERT_EQ(loaded->columns->columns.size(), 1u);
    EXPECT_EQ(loaded->columns->columns.front().native.front().entries.size(), 2u);
    const auto availability = reader.availability("BTC-USD"); ASSERT_TRUE(availability);
    ASSERT_EQ(availability->sources.size(), 1u);
    EXPECT_EQ(availability->sources.front().levels.front().oldestMs, epoch);
    EXPECT_EQ(availability->sources.front().levels.front().committedThroughMs, epoch + kMinuteMs);

    local.disconnected(); local.start({"BTC-USD"});
    watchdog.start(5000); loop.exec(); watchdog.stop();
    ASSERT_EQ(generations.size(), 2u);
    EXPECT_EQ(generations[1], generations[0]);
    EXPECT_EQ(reader.stats().notModified, 1u);
    EXPECT_EQ(store.stats().loads, 1u);
    // Appending through the same live writer becomes a single open revision.
    record.bucketStartMs += kMinuteMs; writer.append(record);
    local.refreshAvailability("BTC-USD");
    watchdog.start(5000); loop.exec(); watchdog.stop();
    ASSERT_EQ(generations.size(), 3u);
    EXPECT_NE(generations[2], generations[1]);
    EXPECT_EQ(store.peek(key())->columns->columns.size(), 2u);
    EXPECT_EQ(store.revisionCount(), 1u);
}

TEST_F(Fetcher, AdapterSecondConnectedReleasesLostSlotsAndRequiresFreshAvailability) {
    fetcher.reset();
    SentinelStreamClient client("127.0.0.1", "1");
    protocol::SentinelStreamClientTransport adapter(client);
    ChunkFetcher remote(store, adapter);
    client.connected(); client.heatmapAvailabilityReceived(available()); drain();
    remote.want(1, {key(), key(1), key(2), key(3)}); drain();
    ASSERT_EQ(remote.stats().inFlightChunks, 4u);
    // disconnectFromServer() has no disconnected signal; replay its public
    // observable sequence: another connected, then (later) availability.
    client.connected(); drain();
    EXPECT_EQ(remote.stats().inFlightChunks, 0u);
    EXPECT_FALSE(remote.availability("BTC-USD"));
    EXPECT_EQ(remote.stats().requests, 1u);
    client.heatmapChunkReceived(1, body(key())); drain();
    EXPECT_FALSE(store.contains(key()));
    client.heatmapAvailabilityReceived(available()); drain();
    ASSERT_EQ(remote.stats().requests, 2u);
    EXPECT_EQ(remote.stats().inFlightChunks, 4u);
    for (int i = 0; i < 4; ++i) client.heatmapChunkReceived(2, body(key(i)));
    drain();
    EXPECT_EQ(remote.stats().inFlightChunks, 0u);
    EXPECT_EQ(store.stats().loads, 4u);
}
TEST_F(Fetcher, LostRepliesExpireAfterThirtySecondsAndRetryWithBackoff) {
    online(); fetcher->want(1, {key(), key(1), key(2), key(3)}); drain();
    const auto lost = transport.requests.front();
    now += 29'999; fetcher->pump(); drain();
    EXPECT_EQ(fetcher->stats().inFlightChunks, 4u);
    EXPECT_EQ(transport.requests.size(), 1u);
    ++now; fetcher->pump(); drain();
    EXPECT_EQ(fetcher->stats().inFlightChunks, 0u);
    EXPECT_EQ(fetcher->stats().inFlightBytes, 0u);
    EXPECT_EQ(transport.forgotten, std::vector<quint64>{lost.id});
    EXPECT_TRUE(failures.empty());
    // Late replies to the abandoned request do not satisfy its replacement.
    transport.reply(lost.id, body(key())); drain();
    EXPECT_FALSE(store.contains(key()));
    now += 99; fetcher->pump(); drain();
    EXPECT_EQ(transport.requests.size(), 1u);
    ++now; fetcher->pump(); drain();
    ASSERT_EQ(transport.requests.size(), 2u);
    EXPECT_EQ(transport.requests.back().starts, lost.starts);
    answer(transport.requests.back());
    EXPECT_EQ(fetcher->stats().inFlightChunks, 0u);
    EXPECT_EQ(store.stats().loads, 4u);
}
TEST_F(Fetcher, VisibleKeyBlockedByBytesRunsBeforeFurtherPrefetch) {
    fetcher.reset();
    create([](const ChunkKey &k) { return k == key(1) ? 12ull << 20 : 8ull << 20; });
    online();
    fetcher->want(1, {key()}, 0); drain(); // existing 8 MiB prefetch
    fetcher->want(1, {key(1)}, 20);       // visible needs 12 MiB
    fetcher->want(1, {key(2)}, 0);       // another 8 MiB prefetch would fit
    drain();
    ASSERT_EQ(transport.requests.size(), 1u) << "leave spare bytes for the blocked visible key";
    answer(transport.requests.front());
    ASSERT_EQ(transport.requests.size(), 2u);
    EXPECT_EQ(transport.requests.back().key(0), key(1));
    EXPECT_EQ(fetcher->stats().inFlightBytes, 12ull << 20);
    answer(transport.requests.back());
    ASSERT_EQ(transport.requests.size(), 3u);
    EXPECT_EQ(transport.requests.back().key(0), key(2));
}
TEST_F(Fetcher, RequestLevelSentinelErrorRetiresAllRemainingKeysOfThatRequest) {
    online(); fetcher->want(1, {key(), key(1), key(0, "hmc2.near")}); drain();
    const auto request = transport.requests.front();
    auto sentinel = key(); sentinel.startMs = 0;
    transport.error(request.id, sentinel, "invalid_request"); drain();
    EXPECT_EQ(failures, (std::vector<ChunkKey>{key(), key(1)}));
    EXPECT_EQ(fetcher->stats().inFlightChunks, 1u);
    EXPECT_EQ(transport.forgotten, std::vector<quint64>{request.id});
    answer(transport.requests.back());
    EXPECT_EQ(fetcher->stats().inFlightChunks, 0u);
}
TEST_F(Fetcher, UnreadableRequestIdRetriesKeysNotShownBad) {
    online(); fetcher->want(1, {key(), key(1)}); drain();
    transport.error(0, key(), "malformed"); drain();
    EXPECT_EQ(failures, std::vector<ChunkKey>{key()});
    EXPECT_EQ(fetcher->stats().inFlightChunks, 0u);
    now += 100; fetcher->pump(); drain();
    ASSERT_EQ(transport.requests.size(), 2u);
    EXPECT_EQ(transport.requests.back().starts, std::vector<int64_t>{key(1).startMs});
    transport.error(0, {}, "malformed"); drain();
    EXPECT_EQ(failures.size(), 1u);
    now += 200; fetcher->pump(); drain();
    ASSERT_EQ(transport.requests.size(), 3u);
    answer(transport.requests.back());
    EXPECT_TRUE(store.contains(key(1)));
}
TEST_F(Fetcher, RejectedStoreInsertionRetriesInsteadOfLosingTheWantedKey) {
    online(); fetcher->want(1, {key()}); drain();
    const auto request = transport.requests.front();
    // Another producer installs a later revision, then it is evicted before
    // this request returns. The older reply cannot be returned as current.
    put(store, body(key(), 45));
    store.setMaxBytes(1);
    ASSERT_FALSE(store.contains(key()));
    transport.reply(request.id, body(key(), 30)); drain();
    EXPECT_EQ(fetcher->stats().inFlightChunks, 0u);
    EXPECT_TRUE(failures.empty());
    store.setMaxBytes(512ull << 20);
    now += 100; fetcher->pump(); drain();
    ASSERT_EQ(transport.requests.size(), 2u);
    answer(transport.requests.back(), 45);
    EXPECT_TRUE(store.contains(key()));
}
TEST_F(Fetcher, DisconnectAndClearResetBackoffToItsBaseDelay) {
    online(); fetcher->want(1, {key()}); drain();
    transport.error(transport.requests.back().id, key(), "busy"); drain();
    now += 100; fetcher->pump(); drain();
    transport.error(transport.requests.back().id, key(), "busy"); drain();
    reconnect();
    auto count = transport.requests.size();
    transport.error(transport.requests.back().id, key(), "busy"); drain();
    now += 100; fetcher->pump(); drain();
    EXPECT_EQ(transport.requests.size(), count + 1);
    transport.error(transport.requests.back().id, key(), "busy"); drain();
    fetcher->hostChanged(); transport.push(available()); drain();
    count = transport.requests.size();
    transport.error(transport.requests.back().id, key(), "busy"); drain();
    now += 100; fetcher->pump(); drain();
    EXPECT_EQ(transport.requests.size(), count + 1);
}
TEST_F(Fetcher, QueuedCallsAndNotificationsCrossARealDataThread) {
    fetcher.reset();
    QThread dataThread;
    auto *host = new QObject;
    auto *fake = new FakeChunkTransport;
    fake->setParent(host);
    auto *remote = new ChunkFetcher(store, *fake, host);
    host->moveToThread(&dataThread);
    QObject::connect(&dataThread, &QThread::finished, host, &QObject::deleteLater);
    QEventLoop loop;
    bool received = false;
    QThread *admissionThread = nullptr;
    struct Join { QThread &thread; ~Join() { thread.quit(); thread.wait(); } } join{dataThread};
    QObject::connect(remote, &ChunkFetcher::chunkStored, &loop, [&](ChunkKey k, quint64 generation) {
        EXPECT_EQ(QThread::currentThread(), app.thread());
        EXPECT_EQ(k, key());
        EXPECT_EQ(generation, store.generationOf(key()));
        received = true; loop.quit();
    }, Qt::QueuedConnection);
    dataThread.start();
    QMetaObject::invokeMethod(host, [&, host, fake, remote] {
        admissionThread = QThread::currentThread();
        fake->goOnline(); fake->push(available()); remote->want(1, {key()});
        QMetaObject::invokeMethod(host, [fake] {
            if (!fake->requests.empty()) fake->reply(fake->requests.front().id, body(key()));
        }, Qt::QueuedConnection);
    }, Qt::QueuedConnection);
    ASSERT_TRUE(await(loop, [&] { return received; }));
    EXPECT_EQ(admissionThread, &dataThread);
    EXPECT_NE(admissionThread, app.thread());
    EXPECT_TRUE(store.contains(key()));
}
TEST_F(Fetcher, LocalAvailabilityFailureDoesNotFailAnOutstandingRequest) {
    fetcher.reset();
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    recording::Hmc2Store writer(dir.path().toStdString()); writer.append(localRecord());
    WorkerGate gate;
    int scans = 0;
    LocalChunkTransport::TestHooks hooks;
    hooks.beforeAvailability = [&](recording::Hmc2Reader &, std::stop_token stop) {
        if (++scans == 2) { gate.wait(stop); throw std::runtime_error("scripted availability failure"); }
    };
    LocalChunkTransport local(dir.path().toStdString(), hooks);
    ChunkFetcher::Options options;
    options.nowMs = [] { return int64_t{1000}; }; // retries cannot race the scripted reply
    ChunkFetcher reader(store, local, options);
    QEventLoop loop;
    int availabilityCount = 0, requestErrors = 0, replies = 0;
    QObject::connect(&reader, &ChunkFetcher::availabilityChanged, &loop, [&](ChunkAvailability) {
        ++availabilityCount; loop.quit();
    }, Qt::QueuedConnection);
    QObject::connect(&reader, &ChunkFetcher::chunkFailed, &loop, [&](ChunkKey, QString, QString) {
        ++requestErrors;
    }, Qt::QueuedConnection);
    QObject::connect(&local, &ChunkTransport::received, &loop, [&](quint64, ChunkFramePtr) {
        ++replies; loop.quit();
    }, Qt::QueuedConnection);
    local.start({"BTC-USD"});
    ASSERT_TRUE(await(loop, [&] { return availabilityCount == 1; }));
    local.refreshAvailability("BTC-USD");
    gate.entered.get_future().wait(); // availability scan holds the worker
    reader.want(1, {key()}); drain(); // request is admitted behind the scan
    EXPECT_EQ(reader.stats().inFlightChunks, 1u);
    gate.release();
    ASSERT_TRUE(await(loop, [&] { return replies == 1; }));
    drain();
    EXPECT_EQ(requestErrors, 0);
    EXPECT_TRUE(store.contains(key()));
    EXPECT_EQ(reader.stats().inFlightChunks, 0u);
}
TEST_F(Fetcher, LocalRequestsReuseAvailabilityAndUnchangedRefreshDoesNotPublish) {
    fetcher.reset();
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    recording::Hmc2Store writer(dir.path().toStdString()); writer.append(localRecord());
    std::atomic<int> scans{0};
    LocalChunkTransport::TestHooks hooks;
    hooks.beforeAvailability = [&](recording::Hmc2Reader &, std::stop_token) { ++scans; };
    LocalChunkTransport local(dir.path().toStdString(), hooks);
    ChunkFetcher reader(store, local);
    QEventLoop loop;
    int publications = 0, replies = 0;
    QObject::connect(&reader, &ChunkFetcher::availabilityChanged, &loop, [&](ChunkAvailability) {
        ++publications; loop.quit();
    }, Qt::QueuedConnection);
    QObject::connect(&local, &ChunkTransport::received, &loop, [&](quint64, ChunkFramePtr) {
        ++replies; loop.quit();
    }, Qt::QueuedConnection);
    local.start({"BTC-USD"}); ASSERT_TRUE(await(loop, [&] { return publications == 1; }));
    reader.want(1, {key()}); drain();
    ASSERT_TRUE(await(loop, [&] { return replies == 1; })); drain();
    EXPECT_EQ(scans, 1);
    // Worker FIFO: the following reply is a barrier after this refresh.
    local.refreshAvailability("BTC-USD");
    local.request("BTC-USD", "hmc2.deep", kMinuteMs, {epoch}, {});
    ASSERT_TRUE(await(loop, [&] { return replies == 2; })); drain();
    EXPECT_EQ(scans, 2);
    EXPECT_EQ(publications, 1);
}
TEST_F(Fetcher, LocalDestructorCancelsAnActiveReaderScan) {
    fetcher.reset();
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    recording::Hmc2Store writer(dir.path().toStdString()); writer.append(localRecord());
    WorkerGate gate;
    std::atomic<bool> cancelled{false};
    LocalChunkTransport::TestHooks hooks;
    hooks.beforeBuild = [&](recording::Hmc2Reader &reader, std::stop_token stop) {
        reader.beforeCandidateForTest([&, stop] { gate.wait(stop); cancelled = stop.stop_requested(); });
    };
    auto local = std::make_unique<LocalChunkTransport>(dir.path().toStdString(), hooks);
    QEventLoop loop;
    bool available = false;
    QObject::connect(local.get(), &ChunkTransport::availability, &loop, [&](ChunkAvailability) {
        available = true; loop.quit();
    }, Qt::QueuedConnection);
    local->start({"BTC-USD"}); ASSERT_TRUE(await(loop, [&] { return available; }));
    local->request("BTC-USD", "hmc2.deep", kMinuteMs, {epoch}, {});
    gate.entered.get_future().wait();
    local.reset(); // requests stop, wakes the latch, and joins the cancelled read
    EXPECT_TRUE(cancelled);
}
TEST_F(Fetcher, LocalRequestsUseTheAdvertisedCutoffUntilAvailabilityRefreshes) {
    fetcher.reset();
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    recording::Hmc2Store writer(dir.path().toStdString()); writer.append(localRecord());
    LocalChunkTransport local(dir.path().toStdString());
    ChunkFetcher reader(store, local);
    QEventLoop loop;
    bool ready = false, received = false;
    QObject::connect(&reader, &ChunkFetcher::availabilityChanged, &loop, [&](ChunkAvailability) {
        ready = true; loop.quit();
    }, Qt::QueuedConnection);
    QObject::connect(&reader, &ChunkFetcher::chunkStored, &loop, [&](ChunkKey, quint64) {
        received = true; loop.quit();
    }, Qt::QueuedConnection);
    local.start({"BTC-USD"}); ASSERT_TRUE(await(loop, [&] { return ready; }));
    writer.append(localRecord(1)); // no availability refresh yet
    reader.want(1, {key()}); drain();
    ASSERT_TRUE(await(loop, [&] { return received; }));
    const auto cached = store.peek(key()); ASSERT_TRUE(cached);
    EXPECT_EQ(cached->committedThroughMs, epoch + kMinuteMs);
    EXPECT_EQ(cached->columns->columns.size(), 1u);
}
TEST_F(Fetcher, RecordingChunkBuildHonorsCancellationDuringScan) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    recording::Hmc2Store writer(dir.path().toStdString()); writer.append(localRecord());
    recording::Hmc2Reader reader(dir.path().toStdString());
    std::stop_source stop;
    recording::ReadControl control;
    control.stop = stop.get_token();
    reader.beforeCandidateForTest([&] { stop.request_stop(); });
    const recording::BookRecorder::Watermarks marks{epoch + kMinuteMs, 0};
    EXPECT_THROW(recording::buildChunk(reader, key(), marks, control), std::runtime_error);
    EXPECT_EQ(control.status, recording::ReadStatus::Cancelled);
}
} // namespace
