#define SENTINEL_FLOW_HISTORY_TEST
#include "CandleWireFixture.hpp"
#include "FlowLiveTestAccess.hpp"
#include "roller/fixtures/FlowJournal.hpp"
#include "roller/Grid.hpp"
#include <gtest/gtest.h>
#include <QCryptographicHash>
#include <QFile>
#include <QScopeGuard>
#include <future>
#include <iostream>
#include <thread>

struct HeatmapChunkWireTest {
    static std::shared_ptr<Session> start(SentinelStreamServer& server, ServerDataModel& model,
                                                std::function<void(std::string, bool)> write) {
        server.m_historyWorkers = std::make_unique<net::thread_pool>(2);
        server.m_running = true;
        auto session = std::make_shared<Session>(tcp::socket(net::make_strand(server.m_ioc)),
                                                server.m_sslCtx, model, &server);
        session->flowHistoryWriteForTest_ = std::move(write);
        server.m_thread = std::thread([&server, guard = net::make_work_guard(server.m_ioc)] {
            server.m_ioc.run();
        });
        return session;
    }
    static void request(const std::shared_ptr<Session>& session, Json request) {
        net::post(session->ws_.get_executor(), [session, request = std::move(request)] {
            session->handleFlowHistory(request);
        });
    }
    static bool submit(SentinelStreamServer& server, std::function<void()> task) {
        return server.submitHistoryTask(std::move(task));
    }
    static std::shared_ptr<Session> attach(SentinelStreamServer& server, ServerDataModel& model,
                                          std::function<void(std::string, bool)> write) {
        auto session = std::make_shared<Session>(tcp::socket(net::make_strand(server.m_ioc)),
                                                 server.m_sslCtx, model, &server);
        session->flowHistoryWriteForTest_ = std::move(write);
        return session;
    }
    static void close(const std::shared_ptr<Session>& session) { session->stop(); }
};
namespace {
using namespace flow_fixture;
namespace flow = sentinel::flow;
namespace fs = std::filesystem;
std::atomic<int> lagWarningCount{0};
void captureLagWarnings(QtMsgType type, const QMessageLogContext&, const QString& text) {
    if (type == QtWarningMsg && text.contains("Flow store lag exceeds live retention symbol=BTC-USD"))
        ++lagWarningCount;
}
struct Decoded {
    Json header;
    std::vector<flow::FlowBar> bars;
};
Decoded decode(const std::string& payload) {
    if (!payload.starts_with("FHE1")) return {Json::parse(payload), {}};
    size_t offset = 4;
    auto read32 = [&] {
        if (offset + 4 > payload.size()) throw std::runtime_error("short frame");
        uint32_t value = 0;
        for (unsigned i = 0; i < 4; ++i) value |= uint32_t(uint8_t(payload[offset++])) << (8 * i);
        return value;
    };
    const auto length = read32();
    if (length > payload.size() - offset) throw std::runtime_error("short header");
    Decoded result{Json::parse(payload.substr(offset, length)), {}};
    offset += length;
    for (const auto& info : result.header.at("bars")) {
        const auto bytes = read32();
        if (bytes > payload.size() - offset) throw std::runtime_error("short bar");
        result.bars.push_back(flow::decodeFlowBar({reinterpret_cast<const uint8_t*>(payload.data() + offset), bytes}));
        EXPECT_EQ(result.bars.back().startMs, info.at("bucket_start_ms"));
        offset += bytes;
    }
    EXPECT_EQ(offset, payload.size());
    return result;
}
// Uses the production session handler, worker pool and queued Qt snapshot. The
// frame sink replaces only socket writes, so this fixture can run without bind permission.
struct FlowFixture {
    QTemporaryDir dir;
    ServerConfig config;
    std::unique_ptr<ServerDataModel> model;
    std::unique_ptr<Authenticator> auth;
    std::unique_ptr<SentinelStreamServer> server;
    FlowFixture() {
        config.recording.enabled = false;
        config.rollerShadow.enabled = false;
        config.rollerShadow.products = {"BTC-USD"};
        config.chart.timeframesMs = {100, 1000, 60000, 300000, 3600000, 14'400'000, 86400000};
        model = std::make_unique<ServerDataModel>(config);
        auth = std::make_unique<Authenticator>((dir.path() + "/no-credentials").toStdString());
        server = std::make_unique<SentinelStreamServer>(*model, *auth, config, 0);
    }
    ~FlowFixture() { server->stop(); }
};
class FlowHistory : public testing::Test {
protected:
    int argc = 1;
    char name[16] = "flow-history";
    char* argv[2] = {name, nullptr};
    QCoreApplication app{argc, argv};
    std::unique_ptr<FlowFixture> fixture;
    std::unique_ptr<recording::FlowStore> store;
    std::shared_ptr<Session> session;
    std::mutex repliesMutex;
    std::map<uint64_t, Decoded> replies;
    fs::path journal, hmc, root;
    int64_t through = Epoch + Hour;
    std::atomic<int> replayRecords{0};
    std::atomic<int> pauseAfterRecords{1};
    std::atomic<int> catalogConstructions{0}, wakeCount{0};
    std::atomic_bool pauseReplay{false}, replayBlocked{false}, replayExited{false};
    std::atomic_bool failRebuild{false};
    std::vector<Json> published;
    uint64_t nextReq = 1;
    Json metadata = {{"product_id", "BTC-USD"}, {"quote_increment", "0.01"}, {"base_increment", "0.00000001"}};
    void SetUp() override {
        fixture = std::make_unique<FlowFixture>();
        journal = fs::path(fixture->dir.path().toStdString()) / "journal";
        hmc = journal.parent_path() / "hmc";
        root = journal.parent_path() / "flow";
        fs::create_directory(journal); fs::create_directory(hmc);
        recording::FlowStore::Options options;
        options.root = root; options.journalRoot = journal; options.hmc2Root = hmc;
        options.symbols = {"BTC-USD"};
        options.nowMs = [] { return Epoch + 2 * Hour; };
        options.hourThrough = [this](const std::string&) { return through; };
        options.afterRecordForTest = [this](std::stop_token stop) {
            const auto count = replayRecords.fetch_add(1) + 1;
            if (!pauseReplay.load() || count < pauseAfterRecords.load()) return;
            replayBlocked.store(true);
            while (!stop.stop_requested()) std::this_thread::sleep_for(1ms);
            replayExited.store(true);
        };
        options.afterCatalogConstructForTest = [this] { catalogConstructions.fetch_add(1); };
        options.afterWakeForTest = [this] { wakeCount.fetch_add(1); };
        options.beforeCommitForTest = [this](const fs::path&) {
            if (failRebuild.load()) throw std::runtime_error("synthetic rebuild interruption");
        };
        store = std::make_unique<recording::FlowStore>(std::move(options));
        fixture->model->attachFlowStore(store.get());
        FlowLiveTestAccess::stopTimer(*fixture->model);
        fixture->model->onFlowGrid("BTC-USD", Epoch, sentinel::roller::deriveGrid(metadata, 100000), metadata);
    }
    void TearDown() override {
        if (!fixture) return;
        fixture->server->stop();
        session.reset();
        fixture->model->attachFlowStore(nullptr);
        store.reset(); fixture.reset();
    }
    void live(int64_t rel, const std::string& id, double price = 100000) {
        fixture->model->onTrade({std::chrono::system_clock::time_point(std::chrono::milliseconds(Epoch + rel)),
                                "BTC-USD", id, AggressorSide::Buy, price, 1});
    }
    void tick(int64_t rel) {
        fixture->model->onFeedWatermark("BTC-USD", Epoch + rel + ServerDataModel::kFlowCloseDelayMs);
        FlowLiveTestAccess::tick(*fixture->model);
    }
    void seed(bool late = false) {
        Journal writer(journal);
        writer.snapshot(0);
        for (int i = 0; i < 60; ++i) {
            auto print = trade(i * Minute, std::to_string(i), 100000 + i % 3, "SELL");
            print["size"] = "1";
            writer.trades(i * Minute + 100, {print});
            live(i * Minute, std::to_string(i), 100000 + i % 3);
            tick(i * Minute);
        }
        if (late) {
            auto print = trade(Hour - Minute + 1000, "late", 100003, "SELL");
            print["size"] = "1";
            writer.trades(Hour + 500, {print});
        }
        writer.heartbeat(Hour + Minute + 1);
        writer.close();
        ASSERT_EQ(store->runPass().built, 1u);
        auto subscription = std::make_shared<FlowSubscription>();
        subscription->symbol = "BTC-USD"; subscription->timeframeMs = 300000;
        subscription->publish = [this](std::string frame) { published.push_back(Json::parse(frame)); };
        fixture->model->subscribeFlow(subscription);
        for (int i = 0; i < 5; ++i) {
            live(Hour + i * Minute, "live" + std::to_string(i), 100003 + i);
            tick(Hour + i * Minute);
            if (late && i == 0) {
                live(Hour - Minute + 1000, "late", 100003);
                tick(Hour);
            }
        }
        tick(Hour + 5 * Minute);
    }
    void connect() {
        session = HeatmapChunkWireTest::start(*fixture->server, *fixture->model,
            [this](std::string payload, bool binary) {
                auto result = decode(payload);
                EXPECT_EQ(binary, result.header["type"] == "flow_history_chunk");
                const auto id = result.header.at("req").get<uint64_t>();
                std::lock_guard lock(repliesMutex);
                replies[id] = std::move(result);
            });
    }
    bool hasReply(uint64_t id) {
        std::lock_guard lock(repliesMutex);
        return replies.contains(id);
    }
    Decoded takeReply(uint64_t id) {
        std::lock_guard lock(repliesMutex);
        auto result = std::move(replies.at(id));
        replies.erase(id);
        return result;
    }
    Decoded request(int64_t tf, int64_t end, int64_t limit) {
        const auto id = nextReq++;
        Json q = {{"type", "flow_history_request"}, {"req", id}, {"symbol", "BTC-USD"},
                  {"timeframe_ms", tf}, {"end_time_ms", end}, {"limit", limit}};
        HeatmapChunkWireTest::request(session, std::move(q));
        if (!spinUntil([&] { return hasReply(id); }, 5s))
            throw std::runtime_error("no flow history reply");
        return takeReply(id);
    }
};
TEST(FlowHistoryMerge, StoreOwnershipBeatsLargerLiveRevision) {
    using protocol::FlowBarSource;
    using protocol::FlowBarVersion;
    const FlowBarVersion live{FlowBarSource::Live, 2, false};
    const FlowBarVersion store{FlowBarSource::Store, 1, true};
    EXPECT_TRUE(protocol::flowBarReplaces(live, store));
    EXPECT_FALSE(protocol::flowBarReplaces(store, live));
    EXPECT_TRUE(protocol::flowBarReplaces(store, {FlowBarSource::Store, 2, true}));
    EXPECT_TRUE(protocol::flowBarReplaces(live, {FlowBarSource::Live, 2, true}));
    EXPECT_FALSE(protocol::flowBarReplaces({FlowBarSource::Live, 2, true}, live));
    EXPECT_TRUE(protocol::dropLiveFlowBar(Epoch, Hour, Epoch + Hour));
    EXPECT_FALSE(protocol::dropLiveFlowBar(Epoch, 4 * Hour, Epoch + Hour));
    EXPECT_FALSE(protocol::dropLiveFlowBar(Epoch + Hour, Hour, Epoch + Hour));
}
TEST_F(FlowHistory, DetachesStoreAfterHistoryWorkersDrain) {
    connect();
    auto* heldStore = fixture->model->flowHistorySnapshot("BTC-USD", Minute, Epoch, Epoch + Minute).store;
    ASSERT_EQ(heldStore, store.get());
    std::promise<void> entered, release;
    auto released = release.get_future().share();
    std::atomic_bool exited{false};
    ASSERT_TRUE(HeatmapChunkWireTest::submit(*fixture->server, [=, &entered, &exited] {
        heldStore->sealedThroughMs("BTC-USD");
        entered.set_value();
        released.wait();
        heldStore->sealedThroughMs("BTC-USD");
        exited.store(true);
    }));
    ASSERT_EQ(entered.get_future().wait_for(2s), std::future_status::ready);
    std::jthread unblock([&] { std::this_thread::sleep_for(100ms); release.set_value(); });
    fixture->server->stop();
    EXPECT_TRUE(exited.load());
    fixture->model->attachFlowStore(nullptr);
    EXPECT_EQ(fixture->model->flowHistorySnapshot("BTC-USD", Minute, Epoch, Epoch + Minute).store, nullptr);
    store->stop();
}
TEST_F(FlowHistory, EmptyStoreWithoutLiveTradeDoesNotWarnAboutLag) {
    ASSERT_EQ(store->sealedThroughMs("BTC-USD"), 0);
    lagWarningCount = 0;
    const auto old = qInstallMessageHandler(captureLagWarnings);
    const auto restore = qScopeGuard([&] { qInstallMessageHandler(old); });
    tick(8 * Hour + Minute);
    EXPECT_EQ(lagWarningCount.load(), 0);
}

TEST_F(FlowHistory, RealSevenHourFlowLagWarnsOncePerMinute) {
    ASSERT_EQ(store->sealedThroughMs("BTC-USD"), 0);
    live(Hour, "first");
    lagWarningCount = 0;
    const auto old = qInstallMessageHandler(captureLagWarnings);
    const auto restore = qScopeGuard([&] { qInstallMessageHandler(old); });
    tick(8 * Hour + Minute);
    EXPECT_EQ(lagWarningCount.load(), 1);
    tick(8 * Hour + Minute + 59'000);
    EXPECT_EQ(lagWarningCount.load(), 1);
    tick(8 * Hour + Minute + 60'000);
    EXPECT_EQ(lagWarningCount.load(), 2);
}

TEST_F(FlowHistory, StoreAndLiveAreOrderedAndPagingCrossesSeamWithoutJournalReads) {
    seed();
    // Moving only a synthetic fixture proves serving does not open the journal.
    fs::rename(journal, journal.string() + "-offline");
    connect();
    auto page = request(Minute, Epoch + Hour + 4 * Minute, 8);
    ASSERT_EQ(page.header["status"], "complete");
    ASSERT_EQ(page.bars.size(), 8u);
    ASSERT_EQ(page.header["ranges"].size(), 8u);
    EXPECT_EQ(page.header["sealed_through_ms"], Epoch + Hour);
    for (size_t i = 0; i < page.header["bars"].size(); ++i)
        EXPECT_EQ(page.header["bars"][i]["source"], i < 3 ? "store" : "live");
    for (size_t i = 1; i < page.bars.size(); ++i)
        EXPECT_EQ(page.bars[i].startMs - page.bars[i - 1].startMs, Minute);
    EXPECT_EQ(page.bars.front().startMs, Epoch + Hour - 3 * Minute);
    for (const auto& r : page.header["ranges"]) EXPECT_EQ(r["status"], "complete");
    auto older = request(Minute, page.header["oldest_ms"].get<int64_t>() - Minute, 8);
    ASSERT_EQ(older.bars.size(), 8u);
    EXPECT_EQ(older.bars.back().startMs + Minute, page.bars.front().startMs);
}
TEST_F(FlowHistory, SealedStoreRevisionOneOwnsLiveRevisionTwoBucket) {
    Journal writer(journal);
    writer.snapshot(0);
    auto print = [](int64_t rel, const std::string& id) {
        auto value = trade(rel, id, 100000, "SELL");
        value["size"] = "1";
        return value;
    };
    writer.trades(59 * Minute + 100, {print(59 * Minute, "first")});
    writer.trades(Hour + 500, {print(59 * Minute + 1000, "late1"),
                                print(59 * Minute + 2000, "late2")});
    writer.heartbeat(Hour + Minute + 1);
    writer.close();
    live(59 * Minute, "first");
    tick(Hour);
    live(59 * Minute + 1000, "late1");
    live(59 * Minute + 2000, "late2");
    tick(Hour);
    const auto& liveHour = FlowLiveTestAccess::hour(*fixture->model, Epoch, Epoch);
    ASSERT_EQ(liveHour.revision, 2u);
    ASSERT_EQ(store->runPass().built, 1u);
    const auto snapshot = fixture->model->flowHistorySnapshot(
        "BTC-USD", Minute, Epoch + 59 * Minute, Epoch + Hour);
    ASSERT_EQ(snapshot.data.bars.size(), 1u);
    ASSERT_EQ(snapshot.data.revisions[0], 2u);
    connect();
    const auto page = request(Minute, Epoch + 59 * Minute, 1);
    ASSERT_EQ(page.bars.size(), 1u);
    ASSERT_EQ(page.header["bars"][0]["source"], "store");
    ASSERT_EQ(page.header["bars"][0]["revision"], 1u);
    EXPECT_EQ(flow::encodeFlowBar(page.bars[0]), flow::encodeFlowBar(snapshot.data.bars[0]));
    EXPECT_TRUE(protocol::flowBarReplaces(
        {protocol::FlowBarSource::Live, snapshot.data.revisions[0], true},
        {protocol::FlowBarSource::Store, 1, true}));
    const auto hourPage = request(Hour, Epoch, 1);
    ASSERT_EQ(hourPage.bars.size(), 1u);
    EXPECT_EQ(hourPage.header["bars"][0]["source"], "store");
    EXPECT_EQ(hourPage.header["bars"][0]["revision"], 1u);
    EXPECT_EQ(flow::encodeFlowBar(hourPage.bars[0]), flow::encodeFlowBar(liveHour.bar));
    EXPECT_TRUE(protocol::flowBarReplaces(
        {protocol::FlowBarSource::Live, liveHour.revision, true},
        {protocol::FlowBarSource::Store, 1, true}));
    EXPECT_TRUE(protocol::dropLiveFlowBar(Epoch, Hour, Epoch + Hour));
}
TEST_F(FlowHistory, SealedSecondsInsideRetentionUseRamWithoutReplay) {
    seed();
    connect();
    const auto before = replayRecords.load();
    const auto page = request(1000, Epoch + Hour - Minute, 2);
    ASSERT_EQ(page.bars.size(), 1u);
    EXPECT_EQ(page.header["bars"][0]["source"], "store");
    EXPECT_EQ(page.header["bars"][0]["revision"], 0);
    EXPECT_EQ(replayRecords.load(), before);
}
TEST_F(FlowHistory, IncompleteRetainedSecondKeepsLiveSource) {
    seed();
    fixture->model->onTrade({std::chrono::system_clock::time_point(
        std::chrono::milliseconds(Epoch + Hour - Minute)), "BTC-USD", "bad-size",
        AggressorSide::Buy, 100000, 0});
    connect();
    const auto before = replayRecords.load();
    const auto page = request(1000, Epoch + Hour - Minute, 1);
    ASSERT_EQ(page.bars.size(), 1u);
    EXPECT_EQ(page.header["bars"][0]["source"], "live");
    EXPECT_EQ(page.header["bars"][0]["complete"], false);
    EXPECT_EQ(replayRecords.load(), before);
}
TEST_F(FlowHistory, FiveMinuteHistoryEqualsComposeAndLiveFlb1) {
    seed();
    auto snapshot = fixture->model->flowHistorySnapshot("BTC-USD", 300000, Epoch + Hour, Epoch + Hour + 300000);
    ASSERT_EQ(snapshot.data.bars.size(), 5u);
    const auto expected = flow::encodeFlowBar(flow::composeFlowBars(snapshot.data.bars, 300000));
    ASSERT_FALSE(published.empty());
    EXPECT_EQ(published.back()["source"], "live");
    auto raw = QByteArray::fromBase64(QByteArray::fromStdString(published.back()["bar"].get<std::string>()));
    EXPECT_EQ(expected, flow::Bytes(raw.begin(), raw.end()));
    fs::rename(journal, journal.string() + "-offline");
    connect();
    auto page = request(300000, Epoch + Hour, 2);
    ASSERT_EQ(page.bars.size(), 2u);
    EXPECT_EQ(page.header["sealed_through_ms"], Epoch + Hour);
    EXPECT_EQ(page.header["bars"][0]["source"], "store");
    EXPECT_EQ(page.header["bars"][1]["source"], "live");
    EXPECT_EQ(flow::encodeFlowBar(page.bars.back()), expected);
    EXPECT_EQ(flow::encodeFlowBar(flow::decodeFlowBar(expected)), expected);
    auto minutes = store->window("BTC-USD", Epoch + Hour - 300000, Epoch + Hour);
    EXPECT_EQ(flow::encodeFlowBar(page.bars.front()), flow::encodeFlowBar(flow::composeFlowBars(minutes.bars, 300000)));
}
TEST_F(FlowHistory, SecondsBeyondRetentionReplayJournalAndCapsAreExplicit) {
    seed();
    connect();
    auto page = request(1000, Epoch + 2 * Minute, 121);
    ASSERT_EQ(page.bars.size(), 3u);
    EXPECT_EQ(page.header["status"], "complete");
    EXPECT_EQ(page.bars.front().startMs, Epoch);
    EXPECT_EQ(page.bars.front().count, 1u);
    EXPECT_EQ(page.header["bars"][0]["source"], "store");
    auto tooLarge = request(1000, Epoch + 2 * Hour, 7200);
    EXPECT_EQ(tooLarge.header["status"], "refused");
    EXPECT_NE(tooLarge.header["error"].get<std::string>().find("3600000 ms (1 h)"), std::string::npos);
    EXPECT_EQ(request(100, Epoch, 10).header["status"], "refused");
    EXPECT_THROW(store->buildWindow("BTC-USD", Epoch, Epoch + 2 * Hour), std::invalid_argument);
}
TEST_F(FlowHistory, MissingSealedHoursAreBuildingAndEmptySealedHoursAreComplete) {
    Journal writer(journal);
    writer.snapshot(0); writer.heartbeat(Hour + Minute + 1); writer.close();
    ASSERT_EQ(store->runPass().built, 1u);
    through = Epoch + 2 * Hour;
    connect();
    auto missing = request(Minute, Epoch + Hour, 2);
    ASSERT_TRUE(missing.bars.empty());
    EXPECT_EQ(missing.header["status"], "building");
    EXPECT_EQ(missing.header["ranges"][0]["status"], "complete");
    EXPECT_EQ(missing.header["ranges"][1]["status"], "building");
}
TEST_F(FlowHistory, PoolSaturationReturnsCorrelatedError) {
    connect();
    auto release = std::make_shared<std::promise<void>>();
    auto ready = release->get_future().share();
    struct Release { std::shared_ptr<std::promise<void>> p; ~Release() { p->set_value(); } } guard{release};
    for (int i = 0; i < 8; ++i)
        ASSERT_TRUE(HeatmapChunkWireTest::submit(*fixture->server, [ready] { ready.wait(); }));
    const auto reply = request(Minute, Epoch, 1);
    EXPECT_EQ(reply.header["type"], "error");
    EXPECT_EQ(reply.header["context"], "flow_history_request");
    EXPECT_EQ(reply.header["message"], "history worker queue is full");
    EXPECT_FALSE(reply.header.contains("status"));
}
TEST_F(FlowHistory, RestartCoverageStaysPartialAndUnbuiltPrefixStaysBuilding) {
    live(Hour + 30'000, "restart");
    tick(Hour + 45'000);
    connect();
    auto reply = request(Minute, Epoch + Hour, 2);
    ASSERT_EQ(reply.bars.size(), 1u);
    EXPECT_EQ(reply.header["ranges"][0]["status"], "building");
    EXPECT_EQ(reply.header["ranges"][1]["status"], "partial");
    EXPECT_FALSE(reply.header["bars"][0]["complete"].get<bool>());
}
TEST_F(FlowHistory, OneJobIncludesQueuedSnapshotAndOversizedPagesAreRefused) {
    connect();
    Json q = {{"type", "flow_history_request"}, {"req", 900u}, {"symbol", "BTC-USD"},
              {"timeframe_ms", Minute}, {"end_time_ms", Epoch}, {"limit", 1}};
    HeatmapChunkWireTest::request(session, q);
    q["req"] = 901u;
    HeatmapChunkWireTest::request(session, q);
    // Do not pump Qt: the first request must be busy while its snapshot is queued.
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!hasReply(901) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    ASSERT_TRUE(hasReply(901));
    const auto error = takeReply(901);
    EXPECT_EQ(error.header["type"], "error");
    EXPECT_EQ(error.header["message"], "one flow history job is already in flight");
    ASSERT_TRUE(spinUntil([&] { return hasReply(900); }));
    EXPECT_EQ(takeReply(900).header["type"], "flow_history_chunk");
    EXPECT_EQ(request(Minute, Epoch, 513).header["status"], "refused");
}
TEST_F(FlowHistory, RetainedSecondsDoNotOpenJournal) {
    seed();
    fs::rename(journal, journal.string() + "-offline");
    connect();
    const auto reply = request(1000, Epoch + Hour + 2000, 3);
    ASSERT_EQ(reply.bars.size(), 1u);
    EXPECT_EQ(reply.header["status"], "complete");
    EXPECT_TRUE(reply.header["bars"][0]["complete"].get<bool>());
}
TEST_F(FlowHistory, PartiallySealedFourHourBarKeepsLiveRevisionAndAcceptsUpdates) {
    auto sub = std::make_shared<FlowSubscription>();
    sub->symbol = "BTC-USD"; sub->timeframeMs = 4 * Hour;
    std::vector<Json> frames;
    sub->publish = [&](std::string payload) { frames.push_back(Json::parse(payload)); };
    fixture->model->subscribeFlow(sub);
    seed(true);
    const auto snapshot = fixture->model->flowHistorySnapshot(
        "BTC-USD", 4 * Hour, Epoch, Epoch + 4 * Hour);
    ASSERT_TRUE(snapshot.liveRevisions.contains(Epoch));
    ASSERT_EQ(snapshot.liveRevisions.at(Epoch), 1u);
    ASSERT_FALSE(frames.empty());
    const auto liveBar = frames.back();
    EXPECT_EQ(liveBar["revision"], 1u);
    EXPECT_EQ(liveBar["source"], "live");
    fs::rename(journal, journal.string() + "-offline");
    connect();
    const auto page = request(4 * Hour, Epoch, 1);
    ASSERT_EQ(page.bars.size(), 1u);
    EXPECT_EQ(page.header["sealed_through_ms"], Epoch + Hour);
    EXPECT_EQ(page.header["bars"][0]["source"], "live");
    EXPECT_FALSE(page.header["bars"][0]["complete"].get<bool>());
    EXPECT_EQ(page.header["bars"][0]["revision"], 1u);
    EXPECT_FALSE(protocol::dropLiveFlowBar(Epoch, 4 * Hour, Epoch + Hour));
    EXPECT_TRUE(protocol::flowBarReplaces(
        {protocol::FlowBarSource::Live, 1, false},
        {protocol::FlowBarSource::Live, 1, true}));
    const auto raw = QByteArray::fromBase64(QByteArray::fromStdString(liveBar["bar"].get<std::string>()));
    EXPECT_EQ(flow::encodeFlowBar(page.bars[0]), flow::Bytes(raw.begin(), raw.end()));
    sub->active.store(false);
}
TEST_F(FlowHistory, PartiallySealedBarWithoutLiveCopyUsesRevisionZero) {
    seed();
    const auto snapshot = fixture->model->flowHistorySnapshot(
        "BTC-USD", 4 * Hour, Epoch, Epoch + 4 * Hour);
    ASSERT_FALSE(snapshot.liveRevisions.contains(Epoch));
    fs::rename(journal, journal.string() + "-offline");
    connect();
    const auto page = request(4 * Hour, Epoch, 1);
    ASSERT_EQ(page.bars.size(), 1u);
    EXPECT_EQ(page.header["bars"][0]["source"], "live");
    EXPECT_FALSE(page.header["bars"][0]["complete"].get<bool>());
    EXPECT_EQ(page.header["bars"][0]["revision"], 0u);
}
TEST_F(FlowHistory, DayCompositeAcrossStoreAndLiveMatchesLivePublication) {
    seed();
    auto sub = std::make_shared<FlowSubscription>();
    sub->symbol = "BTC-USD"; sub->timeframeMs = 86'400'000;
    Json daily;
    sub->publish = [&](std::string payload) { daily = Json::parse(payload); };
    fixture->model->subscribeFlow(sub);
    tick(Hour + 5 * Minute);
    ASSERT_FALSE(daily.empty());
    const auto raw = QByteArray::fromBase64(QByteArray::fromStdString(daily["bar"].get<std::string>()));
    fs::rename(journal, journal.string() + "-offline");
    connect();
    const auto reply = request(86'400'000, Epoch, 1);
    ASSERT_EQ(reply.bars.size(), 1u);
    EXPECT_EQ(reply.header["status"], "complete");
    EXPECT_EQ(reply.header["sealed_through_ms"], Epoch + Hour);
    EXPECT_EQ(reply.header["bars"][0]["source"], "live");
    EXPECT_FALSE(reply.header["bars"][0]["complete"].get<bool>());
    EXPECT_EQ(flow::encodeFlowBar(reply.bars[0]), flow::Bytes(raw.begin(), raw.end()));
    sub->active.store(false);
}
TEST_F(FlowHistory, WindowBeforeFirstJournalDayIsUnrecordedWithoutInventedBars) {
    Journal writer(journal);
    writer.snapshot(0); writer.heartbeat(Minute); writer.close();
    const auto reply = store->buildWindow("BTC-USD", Epoch - 3 * Minute, Epoch - 2 * Minute);
    EXPECT_TRUE(reply.bars.empty());
    ASSERT_EQ(reply.ranges.size(), 1u);
    EXPECT_EQ(reply.ranges.front().status, "unrecorded");
}
TEST_F(FlowHistory, ShortJournalIsPartialAndNeverPresentedAsComplete) {
    Journal writer(journal);
    writer.snapshot(0); writer.trades(1000, {trade(800, "1", 100000)}); writer.close();
    connect();
    const auto reply = request(1000, Epoch + 120'000, 121);
    ASSERT_EQ(reply.bars.size(), 1u);
    EXPECT_EQ(reply.header["status"], "partial");
    EXPECT_FALSE(reply.header["bars"][0]["complete"].get<bool>());
}
TEST_F(FlowHistory, BuiltThroughSeamRetainsLaggingHourWithoutDuplicateStoreMinutes) {
    seed();
    through = Epoch + 2 * Hour; // recorder advanced, but the store has not built hour one
    tick(2 * Hour + Minute);
    EXPECT_EQ(store->sealedThroughMs("BTC-USD"), Epoch + Hour);
    connect();
    const auto hour = request(Hour, Epoch + Hour, 1);
    ASSERT_EQ(hour.bars.size(), 1u);
    EXPECT_EQ(hour.header["sealed_through_ms"], Epoch + Hour);
    EXPECT_EQ(hour.header["bars"][0]["source"], "live");
    EXPECT_EQ(hour.header["ranges"][0]["status"], "complete");
    const auto seam = request(Minute, Epoch + Hour + Minute, 4);
    ASSERT_EQ(seam.bars.size(), 4u);
    for (size_t i = 0; i < seam.bars.size(); ++i)
        EXPECT_EQ(seam.header["bars"][i]["source"], i < 2 ? "store" : "live");
    for (size_t i = 1; i < seam.bars.size(); ++i)
        EXPECT_EQ(seam.bars[i].startMs - seam.bars[i - 1].startMs, Minute);
}
TEST_F(FlowHistory, RestartLoadsBuiltWatermarkBeforeNextHourCommits) {
    Journal writer(journal);
    writer.snapshot(0); writer.heartbeat(3 * Hour + Minute); writer.close();
    through = Epoch + 2 * Hour;
    ASSERT_EQ(store->runPass().built, 2u);
    fixture->model->attachFlowStore(nullptr);
    store.reset();
    through = Epoch + 3 * Hour;
    std::promise<void> entered, release;
    auto released = release.get_future().share();
    recording::FlowStore::Options options;
    options.root = root; options.journalRoot = journal; options.hmc2Root = hmc;
    options.symbols = {"BTC-USD"};
    options.hourThrough = [this](const std::string&) { return through; };
    options.beforeCommitForTest = [&](const fs::path&) { entered.set_value(); released.wait(); };
    store = std::make_unique<recording::FlowStore>(std::move(options));
    fixture->model->attachFlowStore(store.get());
    auto ready = entered.get_future();
    auto pass = std::async(std::launch::async, [&] { return store->runPass(); });
    const auto status = ready.wait_for(5s);
    if (status != std::future_status::ready) {
        release.set_value();
        FAIL() << "new hour did not reach commit";
    }
    EXPECT_EQ(store->sealedThroughMs("BTC-USD"), Epoch + 2 * Hour);
    release.set_value();
    EXPECT_EQ(pass.get().built, 1u);
    EXPECT_EQ(store->sealedThroughMs("BTC-USD"), Epoch + 3 * Hour);
}
TEST_F(FlowHistory, OldFailedHourDoesNotPinRecentBuiltWatermark) {
    Journal writer(journal);
    writer.snapshot(0);
    writer.trades(24 * Hour + 1000, {trade(24 * Hour + 800, "recent", 100000)});
    writer.heartbeat(25 * Hour + Minute); writer.close();
    through = Epoch + 25 * Hour;
    ASSERT_EQ(store->runPass().built, 25u);
    const auto path = store->filePath("BTC-USD", Epoch);
    QFile file(QString::fromStdString(path.string()));
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    const auto dayBytes = file.readAll(); file.close();
    QFile sidecar(QString::fromStdString(path.string() + ".state"));
    ASSERT_TRUE(sidecar.open(QIODevice::ReadOnly));
    auto state = sidecar.readAll(); sidecar.close();
    ASSERT_GT(state.size(), 32);
    ASSERT_NE(uint8_t(state[5]) & 1u, 0u);
    state[5] = char(uint8_t(state[5]) & ~1u); // the old, empty hour stays unbuilt
    QCryptographicHash digest(QCryptographicHash::Sha256);
    digest.addData(QByteArrayView(state.constData(), state.size() - 32));
    digest.addData(dayBytes);
    state.replace(state.size() - 32, 32, digest.result());
    ASSERT_TRUE(sidecar.open(QIODevice::WriteOnly | QIODevice::Truncate));
    ASSERT_EQ(sidecar.write(state), state.size()); sidecar.close();
    fs::last_write_time(path.string() + ".state",
                        fs::last_write_time(path.string() + ".state") + std::chrono::seconds(2));
    failRebuild.store(true);
    EXPECT_EQ(store->runPass().failed, 1u);
    EXPECT_EQ(store->runPass().failed, 1u);
    EXPECT_EQ(store->sealedThroughMs("BTC-USD"), through);
    const auto old = store->window("BTC-USD", Epoch, Epoch + Hour);
    ASSERT_EQ(old.ranges.size(), 1u);
    EXPECT_EQ(old.ranges[0].status, "building");
    const auto recent = store->window("BTC-USD", Epoch + 19 * Hour, through);
    ASSERT_EQ(recent.ranges.size(), 6u);
    for (const auto& range : recent.ranges) EXPECT_EQ(range.status, "complete");
    ASSERT_EQ(recent.bars.size(), 1u);
    connect();
    const auto page = request(Hour, Epoch, 1);
    EXPECT_EQ(page.header["status"], "building");
    EXPECT_EQ(page.header["sealed_through_ms"], through);
}
TEST_F(FlowHistory, InvalidatedHourRollsBackBuiltThroughBeforeRebuildCompletes) {
    seed();
    ASSERT_EQ(store->sealedThroughMs("BTC-USD"), Epoch + Hour);
    auto file = fs::recursive_directory_iterator(journal);
    while (file->path().extension() != ".rawl2") ++file;
    fs::last_write_time(file->path(), fs::last_write_time(file->path()) + std::chrono::seconds(2));
    failRebuild.store(true);
    EXPECT_EQ(store->runPass().failed, 1u);
    EXPECT_EQ(store->sealedThroughMs("BTC-USD"), Epoch);
}
TEST_F(FlowHistory, LiveMinuteFallbackStopsAfterSixExtraClosedHours) {
    seed();
    tick(8 * Hour + Minute);
    const auto snapshot = fixture->model->flowHistorySnapshot(
        "BTC-USD", Minute, Epoch + Hour, Epoch + 2 * Hour);
    EXPECT_EQ(snapshot.retainedFromMs, Epoch + 2 * Hour);
    EXPECT_TRUE(snapshot.data.bars.empty());
    EXPECT_TRUE(snapshot.data.ranges.empty());
    connect();
    const auto page = request(Hour, Epoch + Hour, 1);
    EXPECT_EQ(page.header["status"], "building");
    EXPECT_EQ(page.header["sealed_through_ms"], Epoch + Hour);
}
TEST_F(FlowHistory, MissingProvisionalConstituentClearsCachedLiveRevision) {
    seed();
    auto snapshot = fixture->model->flowHistorySnapshot("BTC-USD", 4 * Hour, Epoch, Epoch + 4 * Hour);
    snapshot.liveRevisions[Epoch] = 9;
    snapshot.observedThroughMs = Epoch + 3 * Hour;
    const flow_history::Request q{1, "BTC-USD", 4 * Hour, Epoch, Epoch + 4 * Hour};
    const auto page = flow_history::buildPage(q, snapshot, Epoch + Hour);
    ASSERT_EQ(page.bars.size(), 1u);
    EXPECT_EQ(page.metadata["ranges"][0]["status"], "building");
    EXPECT_EQ(page.metadata["bars"][0]["source"], "live");
    EXPECT_EQ(page.metadata["bars"][0]["revision"], 0);
    EXPECT_EQ(page.metadata["bars"][0]["complete"], false);
}
TEST_F(FlowHistory, IncompleteFinalStoreBarKeepsStoreSourceAndRevision) {
    seed();
    auto snapshot = fixture->model->flowHistorySnapshot("BTC-USD", 4 * Hour, Epoch, Epoch + 4 * Hour);
    const flow_history::Request q{1, "BTC-USD", 4 * Hour, Epoch, Epoch + 4 * Hour};
    const auto page = flow_history::buildPage(q, snapshot, Epoch + 4 * Hour);
    ASSERT_EQ(page.bars.size(), 1u);
    EXPECT_EQ(page.metadata["ranges"][0]["status"], "building");
    EXPECT_EQ(page.metadata["bars"][0]["source"], "store");
    EXPECT_EQ(page.metadata["bars"][0]["complete"], false);
    EXPECT_EQ(page.metadata["bars"][0]["revision"], 1u);
    EXPECT_TRUE(protocol::flowBarReplaces(
        {protocol::FlowBarSource::Store, 1, false}, {protocol::FlowBarSource::Store, 1, true}));
}
TEST_F(FlowHistory, HourThroughAdvanceWakesStoreOnce) {
    const auto before = wakeCount.load();
    fixture->model->onFlowHourThrough("BTC-USD", Epoch + Hour);
    fixture->model->onFlowHourThrough("BTC-USD", Epoch + Hour);
    EXPECT_EQ(wakeCount.load(), before + 1);
    fixture->model->onFlowHourThrough("BTC-USD", Epoch + 2 * Hour);
    EXPECT_EQ(wakeCount.load(), before + 2);
}
TEST_F(FlowHistory, ReplayUsesCachedCatalogAndRejectsSecondClient) {
    seed();
    connect();
    const auto constructions = catalogConstructions.load();
    pauseReplay.store(true);
    HeatmapChunkWireTest::request(session, Json{{"type", "flow_history_request"}, {"req", uint64_t(700)},
        {"symbol", "BTC-USD"}, {"timeframe_ms", 1000}, {"end_time_ms", Epoch + 2 * Minute}, {"limit", 121}});
    ASSERT_TRUE(spinUntil([&] { return replayBlocked.load(); }, 5s));
    auto other = HeatmapChunkWireTest::attach(*fixture->server, *fixture->model,
        [this](std::string payload, bool) {
            auto result = decode(payload);
            std::lock_guard lock(repliesMutex);
            replies[result.header.at("req").get<uint64_t>()] = std::move(result);
        });
    HeatmapChunkWireTest::request(other, Json{{"type", "flow_history_request"}, {"req", uint64_t(701)},
        {"symbol", "BTC-USD"}, {"timeframe_ms", 1000}, {"end_time_ms", Epoch + 2 * Minute}, {"limit", 121}});
    ASSERT_TRUE(spinUntil([&] { return hasReply(701); }, 5s));
    EXPECT_NE(takeReply(701).header["message"].get<std::string>().find("busy"), std::string::npos);
    HeatmapChunkWireTest::close(session);
    ASSERT_TRUE(spinUntil([&] { return replayExited.load(); }, 2s));
    EXPECT_EQ(catalogConstructions.load(), constructions);
    HeatmapChunkWireTest::close(other);
}
TEST_F(FlowHistory, ServerStopCancelsLargeSyntheticReplayWithinTwoSeconds) {
    Journal writer(journal);
    writer.snapshot(0);
    for (int frame = 0; frame < 60; ++frame) {
        std::vector<Json> prints;
        for (int i = 0; i < 100; ++i)
            prints.push_back(trade(frame * 1000 + i, std::to_string(frame * 100 + i), 100000));
        writer.trades(frame * 1000 + 100, std::move(prints));
    }
    writer.heartbeat(2 * Minute + 1000);
    writer.close();
    connect();
    pauseAfterRecords.store(50);
    pauseReplay.store(true);
    HeatmapChunkWireTest::request(session, Json{{"type", "flow_history_request"}, {"req", uint64_t(702)},
        {"symbol", "BTC-USD"}, {"timeframe_ms", 1000}, {"end_time_ms", Epoch + Minute}, {"limit", 61}});
    ASSERT_TRUE(spinUntil([&] { return replayBlocked.load(); }, 5s));
    const auto start = std::chrono::steady_clock::now();
    fixture->server->stop();
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count();
    EXPECT_TRUE(replayExited.load());
    EXPECT_LT(elapsed, 2'000'000);
    std::cout << "flow replay stop latency us=" << elapsed << '\n';
}
} // namespace

TEST_F(FlowHistory, AbsentDaysReachWireEvenWithZeroStoreWatermark) {
    through = 0;
    ASSERT_EQ(store->runPass().failed, 0u);
    connect();
    for (const auto tf : {1000LL, 60'000LL, 86'400'000LL}) {
        const auto reply = request(tf, Epoch - 48 * Hour, 2);
        EXPECT_EQ(reply.header["status"], "unrecorded");
        EXPECT_EQ(reply.header["sealed_through_ms"], 0);
        EXPECT_TRUE(reply.bars.empty());
        ASSERT_EQ(reply.header["ranges"].size(), 2u);
        for (const auto& range : reply.header["ranges"]) EXPECT_EQ(range["status"], "unrecorded");
    }
}
TEST_F(FlowHistory, BeforeJournalDayIsTerminalWhileRecordedAndAbsentPagesArePartial) {
    seed();
    connect();
    for (const auto tf : {1000LL, 60'000LL}) {
        const auto reply = request(tf, Epoch - Hour, 2);
        EXPECT_EQ(reply.header["status"], "unrecorded");
        EXPECT_TRUE(reply.bars.empty());
    }
    const auto mixed = request(Hour, Epoch, 3);
    EXPECT_EQ(mixed.header["status"], "partial");
    EXPECT_EQ(mixed.header["ranges"][0]["status"], "unrecorded");
    EXPECT_EQ(mixed.header["ranges"][1]["status"], "partial"); // midnight receive tail remains unknown
}
TEST(FlowHistoryMerge, UnrecordedMixedWithRecordedConstituentIsPartial) {
    flow_history::Request q;
    q.symbol = "BTC-USD"; q.tf = 300'000; q.from = 0; q.to = q.tf;
    ServerDataModel::FlowHistorySnapshot snapshot;
    snapshot.data.ranges = {{0, 60'000, "unrecorded", {}}, {60'000, q.to, "complete", {}}};
    auto page = flow_history::buildPage(q, snapshot, 0);
    EXPECT_EQ(page.metadata["status"], "partial");
    EXPECT_EQ(page.metadata["ranges"][0]["status"], "partial");
    EXPECT_TRUE(page.bars.empty());
}

TEST_F(FlowHistory, ClosedAbsentPrefixInsideRamRetentionDoesNotHideLiveCoverage) {
    through = 0;
    store->runPass();
    fixture->model->onFlowGrid("BTC-USD", Epoch - 24 * Hour,
                              sentinel::roller::deriveGrid(metadata, 100000), metadata);
    live(-2 * Hour, "previous-day-live");
    tick(-2 * Hour + Minute);
    connect();
    const auto result = request(Minute, Epoch - 2 * Hour, 2);
    ASSERT_EQ(result.header["ranges"].size(), 2u);
    EXPECT_EQ(result.header["ranges"][0]["status"], "unrecorded");
    EXPECT_EQ(result.header["ranges"][1]["status"], "complete");
    EXPECT_EQ(result.header["status"], "partial");
    ASSERT_EQ(result.bars.size(), 1u);
}
