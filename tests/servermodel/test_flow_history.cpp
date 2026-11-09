#define SENTINEL_FLOW_HISTORY_TEST
#include "CandleWireFixture.hpp"
#include "FlowLiveTestAccess.hpp"
#include "roller/fixtures/FlowJournal.hpp"
#include "roller/Grid.hpp"
#include <gtest/gtest.h>

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
};
namespace {
using namespace flow_fixture;
namespace flow = sentinel::flow;
namespace fs = std::filesystem;
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
        config.chart.timeframesMs = {100, 1000, 60000, 300000, 3600000, 86400000};
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
        options.hourThrough = [this](const std::string&) { return through; };
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
    void seed() {
        Journal writer(journal);
        writer.snapshot(0);
        for (int i = 0; i < 60; ++i) {
            auto print = trade(i * Minute, std::to_string(i), 100000 + i % 3, "SELL");
            print["size"] = "1";
            writer.trades(i * Minute + 100, {print});
            live(i * Minute, std::to_string(i), 100000 + i % 3);
            tick(i * Minute);
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
TEST_F(FlowHistory, StoreAndLiveAreOrderedAndPagingCrossesSeamWithoutJournalReads) {
    seed();
    // Moving only a synthetic fixture proves serving does not open the journal.
    fs::rename(journal, journal.string() + "-offline");
    connect();
    auto page = request(Minute, Epoch + Hour + 4 * Minute, 8);
    ASSERT_EQ(page.header["status"], "complete");
    ASSERT_EQ(page.bars.size(), 8u);
    ASSERT_EQ(page.header["ranges"].size(), 8u);
    for (size_t i = 1; i < page.bars.size(); ++i)
        EXPECT_EQ(page.bars[i].startMs - page.bars[i - 1].startMs, Minute);
    EXPECT_EQ(page.bars.front().startMs, Epoch + Hour - 3 * Minute);
    for (const auto& r : page.header["ranges"]) EXPECT_EQ(r["status"], "complete");
    auto older = request(Minute, page.header["oldest_ms"].get<int64_t>() - Minute, 8);
    ASSERT_EQ(older.bars.size(), 8u);
    EXPECT_EQ(older.bars.back().startMs + Minute, page.bars.front().startMs);
}
TEST_F(FlowHistory, FiveMinuteHistoryEqualsComposeAndLiveFlb1) {
    seed();
    auto snapshot = fixture->model->flowHistorySnapshot("BTC-USD", 300000, Epoch + Hour, Epoch + Hour + 300000);
    ASSERT_EQ(snapshot.data.bars.size(), 5u);
    const auto expected = flow::encodeFlowBar(flow::composeFlowBars(snapshot.data.bars, 300000));
    ASSERT_FALSE(published.empty());
    auto raw = QByteArray::fromBase64(QByteArray::fromStdString(published.back()["bar"].get<std::string>()));
    EXPECT_EQ(expected, flow::Bytes(raw.begin(), raw.end()));
    fs::rename(journal, journal.string() + "-offline");
    connect();
    auto page = request(300000, Epoch + Hour, 2);
    ASSERT_EQ(page.bars.size(), 2u);
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
    EXPECT_EQ(flow::encodeFlowBar(reply.bars[0]), flow::Bytes(raw.begin(), raw.end()));
    sub->active.store(false);
}
TEST_F(FlowHistory, WindowBeforeFirstJournalIsPartialWithoutInventedBars) {
    Journal writer(journal);
    writer.snapshot(0); writer.heartbeat(Minute); writer.close();
    const auto reply = store->buildWindow("BTC-USD", Epoch - 2 * Minute, Epoch - Minute);
    EXPECT_TRUE(reply.bars.empty());
    ASSERT_EQ(reply.ranges.size(), 1u);
    EXPECT_EQ(reply.ranges.front().status, "partial");
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
} // namespace
