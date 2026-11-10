#define SENTINEL_FLOW_HISTORY_TEST
#include "CandleWireFixture.hpp"
#include "roller/fixtures/FlowJournal.hpp"
#include <gtest/gtest.h>
#include <QTemporaryDir>
#include <future>
#include <thread>

struct HeatmapChunkWireTest {
    static std::shared_ptr<Session> start(SentinelStreamServer& server, ServerDataModel& model,
                                          std::function<void(std::string, bool)> sink) {
        server.m_historyWorkers = std::make_unique<net::thread_pool>(2);
        server.m_running = true;
        auto session = std::make_shared<Session>(tcp::socket(net::make_strand(server.m_ioc)),
                                                 server.m_sslCtx, model, &server);
        session->flowHistoryWriteForTest_ = std::move(sink);
        server.m_thread = std::thread([&server, guard = net::make_work_guard(server.m_ioc)] { server.m_ioc.run(); });
        return session;
    }
    static void request(std::shared_ptr<Session> session, nlohmann::json request) {
        net::post(session->ws_.get_executor(), [session, request = std::move(request)] {
            session->handleTradeHistory(request);
        });
    }
    static std::thread::id ioThread(const std::shared_ptr<Session>& session) {
        auto promise = std::make_shared<std::promise<std::thread::id>>();
        auto future = promise->get_future();
        net::post(session->ws_.get_executor(), [promise] { promise->set_value(std::this_thread::get_id()); });
        return future.get();
    }
};
struct TradeOverlayModelTest {
    static void fill(ServerDataModel& model, int64_t ms) {
        std::lock_guard lock(model.m_footprintTradeMutex);
        auto& tape = model.m_recentFootprintTrades["BTC-USD"];
        for (size_t i = 0; i <= protocol::SentinelProtocol::kTradeHistoryMaxTrades; ++i)
            tape.push_back({ms + int64_t(i / 100), 100000, 0.1, AggressorSide::Buy,
                            std::to_string(i)});
    }
};

namespace {
using namespace flow_fixture;
using Page = protocol::tradewire::Page;
using namespace std::chrono_literals;
struct TradeHistory : testing::Test {
    int argc = 1;
    char name[16] = "trade-history";
    char* argv[2] = {name, nullptr};
    QCoreApplication app{argc, argv};
    QTemporaryDir dir;
    ServerConfig config;
    std::unique_ptr<ServerDataModel> model;
    std::unique_ptr<Authenticator> auth;
    std::unique_ptr<SentinelStreamServer> server;
    std::unique_ptr<recording::FlowStore> store;
    std::shared_ptr<Session> session;
    std::mutex mutex;
    std::condition_variable cv;
    std::map<uint64_t, Page> replies;
    std::thread::id journalThread;
    void SetUp() override {
        config.recording.enabled = false;
        config.rollerShadow.enabled = false;
        config.rollerShadow.products = {"BTC-USD"};
        config.chart.timeframesMs = {100, 1000, 60'000, 3'600'000};
        model = std::make_unique<ServerDataModel>(config);
        auth = std::make_unique<Authenticator>((dir.path() + "/no-credentials").toStdString());
        server = std::make_unique<SentinelStreamServer>(*model, *auth, config, 0);
        const auto root = std::filesystem::path(dir.path().toStdString());
        std::filesystem::create_directory(root / "journal");
        std::filesystem::create_directory(root / "hmc");
        recording::FlowStore::Options options;
        options.root = root / "flow"; options.journalRoot = root / "journal";
        options.hmc2Root = root / "hmc"; options.symbols = {"BTC-USD"};
        options.hourThrough = [](const std::string&) { return Epoch + Hour; };
        options.afterRecordForTest = [this](std::stop_token stop) {
            journalThread = std::this_thread::get_id();
        };
        store = std::make_unique<recording::FlowStore>(std::move(options));
        model->attachFlowStore(store.get());
        session = HeatmapChunkWireTest::start(*server, *model, [this](std::string bytes, bool binary) {
            if (!binary) return;
            auto page = protocol::tradewire::decode({reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()});
            { std::lock_guard lock(mutex); replies[page.req] = std::move(page); }
            cv.notify_all();
        });
    }
    void TearDown() override {
        server->stop();
        session.reset();
        model->attachFlowStore(nullptr);
        store.reset();
    }
    void live(int64_t rel, std::string id) {
        model->onTrade({std::chrono::system_clock::time_point(std::chrono::milliseconds(Epoch + rel)),
                        "BTC-USD", std::move(id), AggressorSide::Buy, 100000, 0.1});
    }
    Page request(uint64_t req, int64_t from, int64_t to, const std::string& symbol = "BTC-USD") {
        HeatmapChunkWireTest::request(session, {{"req", req}, {"symbol", symbol},
            {"start_ms", from}, {"end_ms", to}});
        std::unique_lock lock(mutex);
        EXPECT_TRUE(cv.wait_for(lock, 10s, [&] { return replies.contains(req); }));
        if (!replies.contains(req)) return {};
        return std::move(replies.at(req));
    }
    void journal() {
        Journal writer(std::filesystem::path(dir.path().toStdString()) / "journal");
        writer.snapshot(0);
        writer.trades(1000, {trade(1000, "a", 100000)});
        writer.trades(2000, {trade(2000, "b", 100001)});
        writer.trades(3000, {trade(3000, "c", 100002)});
        writer.heartbeat(70'000);
        writer.close();
    }
};
}

TEST_F(TradeHistory, LiveJournalAndStraddleAreOrderedWithoutSeamDuplicate) {
    journal();
    live(3000, "c"); live(4000, "d");
    const auto livePage = request(1, Epoch + 3000, Epoch + 5000);
    ASSERT_EQ(livePage.status, "complete");
    ASSERT_EQ(livePage.rows.size(), 2u);
    const auto old = request(2, Epoch, Epoch + 3000);
    ASSERT_EQ(old.status, "complete");
    ASSERT_EQ(old.rows.size(), 2u);
    EXPECT_EQ(old.rows.front().id, "a");
    const auto both = request(3, Epoch, Epoch + 5000);
    ASSERT_EQ(both.status, "complete");
    ASSERT_EQ(both.rows.size(), 4u);
    EXPECT_EQ(both.rows[0].id, "a"); EXPECT_EQ(both.rows[1].id, "b");
    EXPECT_EQ(both.rows[2].id, "c"); EXPECT_EQ(both.rows[3].id, "d");
    EXPECT_NE(journalThread, std::this_thread::get_id());
    EXPECT_NE(journalThread, HeatmapChunkWireTest::ioThread(session));
    HeatmapChunkWireTest::request(session, {{"req", uint64_t(4)}, {"symbol", "BTC-USD"},
        {"start_ms", Epoch}, {"end_ms", Epoch + 5000}});
    live(4500, "live-during-request");
    EXPECT_EQ(model->tradeHistorySnapshot("BTC-USD", Epoch, Epoch + 5000, 10).rows.size(), 3u);
    std::unique_lock lock(mutex);
    EXPECT_TRUE(cv.wait_for(lock, 10s, [&] { return replies.contains(4); }));
}

TEST_F(TradeHistory, InvalidWindowsAndUnknownSymbolAreRefused) {
    EXPECT_EQ(request(1, Epoch, Epoch + 2 * Hour).status, "refused");
    EXPECT_EQ(request(2, Epoch, Epoch + 1000, "UNKNOWN").status, "refused");
    EXPECT_EQ(request(3, -1, Epoch).status, "refused");
}

TEST_F(TradeHistory, TradeCountAboveCapIsRefused) {
    TradeOverlayModelTest::fill(*model, Epoch + 1000);
    const auto page = request(1, Epoch, Epoch + 10'000);
    EXPECT_EQ(page.status, "refused");
    EXPECT_NE(page.reason.find("250000"), std::string::npos);
}

TEST_F(TradeHistory, MissingJournalReportsOnlyLiveSuffixCoverage) {
    live(3000, "live");
    const auto result = request(1, Epoch, Epoch + 5000);
    ASSERT_EQ(result.status, "partial");
    EXPECT_EQ(result.coveredFromMs, Epoch + 3000);
    EXPECT_EQ(result.coveredToMs, Epoch + 5000);
    ASSERT_EQ(result.rows.size(), 1u);
    EXPECT_EQ(result.rows.front().id, "live");
}

TEST(TradeHistoryWire, ValidAndMalformedFrames) {
    Page page; page.req = 7; page.symbol = "BTC-USD"; page.status = "complete";
    page.startMs = page.coveredFromMs = 1000;
    page.endMs = page.coveredToMs = 2000;
    page.rows.push_back({"a", 1500, 100, 0.1, AggressorSide::Buy, 1});
    auto bytes = protocol::tradewire::encode(page);
    auto decode = [](const std::string& value) {
        return protocol::tradewire::decode({reinterpret_cast<const uint8_t*>(value.data()), value.size()});
    };
    EXPECT_EQ(decode(bytes).rows.front().id, "a");
    auto truncated = bytes; truncated.pop_back();
    EXPECT_THROW(decode(truncated), std::exception);
    auto oversized = bytes; oversized.resize(protocol::SentinelProtocol::kTradeHistoryMaxFrameBytes + 1);
    EXPECT_THROW(decode(oversized), std::exception);
    auto badCount = bytes;
    const auto countAt = badCount.size() - 31 - 1 - 4;
    badCount[countAt] = 2;
    EXPECT_THROW(decode(badCount), std::exception);
}
