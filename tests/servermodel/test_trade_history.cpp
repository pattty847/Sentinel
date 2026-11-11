#define SENTINEL_FLOW_HISTORY_TEST
#include "CandleWireFixture.hpp"
#include "roller/fixtures/FlowJournal.hpp"
#include <gtest/gtest.h>
#include <QTemporaryDir>
#include <future>
#include <limits>
#include <thread>
#include "protocol/SentinelStreamClient.hpp"

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
    static std::shared_ptr<Session> additional(SentinelStreamServer& server, ServerDataModel& model,
                                                std::function<void(std::string, bool)> sink) {
        auto session = std::make_shared<Session>(tcp::socket(net::make_strand(server.m_ioc)),
                                                 server.m_sslCtx, model, &server);
        session->flowHistoryWriteForTest_ = std::move(sink);
        return session;
    }
    static void stop(const std::shared_ptr<Session>& session) { session->stop(); }
    static bool submit(SentinelStreamServer& server, std::function<void()> task) {
        return server.submitHistoryTask(std::move(task));
    }
    static bool busy(const SentinelStreamServer& server) { return server.m_tradeHistoryBusy.load(); }
    static std::thread::id ioThread(const std::shared_ptr<Session>& session) {
        auto promise = std::make_shared<std::promise<std::thread::id>>();
        auto future = promise->get_future();
        net::post(session->ws_.get_executor(), [promise] { promise->set_value(std::this_thread::get_id()); });
        return future.get();
    }
};
struct TradeOverlayModelTest {
    static int64_t retentionMs(const ServerDataModel& model) { return model.m_tradeRetentionMs; }
    static void fill(ServerDataModel& model, int64_t ms) {
        std::lock_guard lock(model.m_tradeMutex);
        auto& tape = model.m_recentTrades["BTC-USD"];
        for (size_t i = 0; i <= protocol::SentinelProtocol::kTradeHistoryMaxTrades; ++i)
            tape.push_back({ms + int64_t(i / 100), 100000, 0.1, AggressorSide::Buy,
                            std::to_string(i)});
    }
    static void invalid(ServerDataModel& model, int64_t ms) {
        std::lock_guard lock(model.m_tradeMutex);
        auto& tape = model.m_recentTrades["BTC-USD"];
        tape.insert(tape.begin() + 1, {ms, 100000, 0.0, AggressorSide::Buy, "invalid"});
        tape.insert(tape.begin() + 2, {ms, std::numeric_limits<double>::quiet_NaN(), 0.1,
                                      AggressorSide::Buy, "nan"});
        tape.insert(tape.begin() + 3, {ms, 100000, 0.1, AggressorSide::Buy, std::string(129, 'x')});
    }
};

namespace {
using namespace flow_fixture;
using Page = protocol::tradewire::Page;
using namespace std::chrono_literals;
struct TestModel : ServerDataModel {
    using ServerDataModel::ServerDataModel;
    int64_t exchangeNowMs() const override { return Epoch + 120'000; }
};
struct TradeHistory : testing::Test {
    int argc = 1;
    char name[16] = "trade-history";
    char* argv[2] = {name, nullptr};
    QCoreApplication app{argc, argv};
    QTemporaryDir dir;
    ServerConfig config;
    std::unique_ptr<TestModel> model;
    std::unique_ptr<Authenticator> auth;
    std::unique_ptr<SentinelStreamServer> server;
    std::unique_ptr<recording::FlowStore> store;
    std::shared_ptr<Session> session;
    std::mutex mutex;
    std::condition_variable cv;
    std::map<uint64_t, Page> replies;
    std::thread::id journalThread;
    std::function<void(std::stop_token)> journalHook;
    void SetUp() override {
        config.recording.enabled = false;
        config.rollerShadow.enabled = false;
        config.rollerShadow.products = {"BTC-USD"};
        config.chart.timeframesMs = {100, 1000, 60'000, 3'600'000};
        model = std::make_unique<TestModel>(config);
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
            if (journalHook) journalHook(stop);
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
        EXPECT_TRUE(cv.wait_for(lock, 60s, [&] { return replies.contains(req); }));
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

TEST_F(TradeHistory, ReconnectGapUsesJournalAndSameMillisecondRows) {
    Journal writer(std::filesystem::path(dir.path().toStdString()) / "journal");
    writer.snapshot(0);
    writer.trades(1000, {trade(1000, "before", 100000)});
    writer.trades(2000, {trade(2000, "gap", 100001)});
    writer.trades(3000, {trade(3000, "resume", 100002), trade(3000, "same-ms", 100003)});
    writer.heartbeat(70'000); writer.close();
    model->onMarketDataConnectionChanged("BTC-USD", true);
    live(1000, "before");
    model->onMarketDataConnectionChanged("BTC-USD", false);
    model->onMarketDataConnectionChanged("BTC-USD", true);
    live(3000, "resume");
    const auto page = request(1, Epoch, Epoch + 4000);
    ASSERT_EQ(page.status, "complete");
    ASSERT_EQ(page.rows.size(), 4u);
    EXPECT_EQ(page.rows[0].id, "before"); EXPECT_EQ(page.rows[1].id, "gap");
    EXPECT_EQ(page.rows[2].id, "resume"); EXPECT_EQ(page.rows[3].id, "same-ms");
}

TEST_F(TradeHistory, BookResyncMovesLiveHistoryFloor) {
    journal();
    model->onMarketDataConnectionChanged("BTC-USD", true);
    live(1000, "a");
    model->onLiveOrderBookInvalidated(QStringLiteral("BTC-USD"), QStringLiteral("test resync"));
    live(3000, "c");
    const auto page = request(1, Epoch, Epoch + 4000);
    ASSERT_EQ(page.status, "complete");
    ASSERT_EQ(page.rows.size(), 3u);
    EXPECT_EQ(page.rows[1].id, "b");
}

TEST_F(TradeHistory, ServerWideBusyLeavesOtherHistoryWorkerAvailable) {
    journal();
    auto entered = std::make_shared<std::promise<void>>();
    auto enteredFuture = entered->get_future();
    auto release = std::make_shared<std::promise<void>>();
    auto releaseFuture = release->get_future().share();
    auto once = std::make_shared<std::atomic_bool>(false);
    journalHook = [entered, releaseFuture, once](std::stop_token stop) {
        if (!once->exchange(true)) {
            entered->set_value();
            while (releaseFuture.wait_for(1ms) != std::future_status::ready && !stop.stop_requested()) {}
        }
    };
    auto other = HeatmapChunkWireTest::additional(*server, *model, [this](std::string bytes, bool binary) {
        if (!binary) return;
        auto page = protocol::tradewire::decode({reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()});
        { std::lock_guard lock(mutex); replies[page.req] = std::move(page); }
        cv.notify_all();
    });
    HeatmapChunkWireTest::request(session, {{"req", uint64_t(1)}, {"symbol", "BTC-USD"},
        {"start_ms", Epoch}, {"end_ms", Epoch + 4000}});
    ASSERT_EQ(enteredFuture.wait_for(10s), std::future_status::ready);
    HeatmapChunkWireTest::request(other, {{"req", uint64_t(2)}, {"symbol", "BTC-USD"},
        {"start_ms", Epoch}, {"end_ms", Epoch + 4000}});
    std::promise<void> otherWorker;
    ASSERT_TRUE(HeatmapChunkWireTest::submit(*server, [&] { otherWorker.set_value(); }));
    EXPECT_EQ(otherWorker.get_future().wait_for(10s), std::future_status::ready);
    {
        std::unique_lock lock(mutex);
        ASSERT_TRUE(cv.wait_for(lock, 10s, [&] { return replies.contains(2); }));
        EXPECT_EQ(replies.at(2).status, "refused");
        EXPECT_EQ(replies.at(2).reason, "busy");
    }
    release->set_value();
    {
        std::unique_lock lock(mutex);
        ASSERT_TRUE(cv.wait_for(lock, 10s, [&] { return replies.contains(1); }));
    }
    HeatmapChunkWireTest::request(other, {{"req", uint64_t(3)}, {"symbol", "BTC-USD"},
        {"start_ms", Epoch}, {"end_ms", Epoch + 4000}});
    std::unique_lock lock(mutex);
    ASSERT_TRUE(cv.wait_for(lock, 10s, [&] { return replies.contains(3); }));
    EXPECT_EQ(replies.at(3).status, "complete");
}

TEST_F(TradeHistory, DisconnectCancelsJournalWithoutReplyAndReleasesFlight) {
    journal();
    auto entered = std::make_shared<std::promise<void>>();
    auto enteredFuture = entered->get_future();
    auto once = std::make_shared<std::atomic_bool>(false);
    journalHook = [entered, once](std::stop_token stop) {
        if (!once->exchange(true)) {
            entered->set_value();
            while (!stop.stop_requested()) std::this_thread::sleep_for(1ms);
        }
    };
    HeatmapChunkWireTest::request(session, {{"req", uint64_t(1)}, {"symbol", "BTC-USD"},
        {"start_ms", Epoch}, {"end_ms", Epoch + 4000}});
    ASSERT_EQ(enteredFuture.wait_for(10s), std::future_status::ready);
    HeatmapChunkWireTest::stop(session);
    auto other = HeatmapChunkWireTest::additional(*server, *model, [this](std::string bytes, bool binary) {
        if (!binary) return;
        auto page = protocol::tradewire::decode({reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()});
        { std::lock_guard lock(mutex); replies[page.req] = std::move(page); }
        cv.notify_all();
    });
    // A worker fence establishes that cancellation released the global flight.
    std::promise<void> drained;
    ASSERT_TRUE(HeatmapChunkWireTest::submit(*server, [&] { drained.set_value(); }));
    ASSERT_EQ(drained.get_future().wait_for(10s), std::future_status::ready);
    for (int i = 0; i < 1000 && HeatmapChunkWireTest::busy(*server); ++i)
        std::this_thread::sleep_for(1ms);
    HeatmapChunkWireTest::request(other, {{"req", uint64_t(2)}, {"symbol", "BTC-USD"},
        {"start_ms", Epoch}, {"end_ms", Epoch + 4000}});
    std::unique_lock lock(mutex);
    ASSERT_TRUE(cv.wait_for(lock, 10s, [&] { return replies.contains(2); }));
    EXPECT_EQ(replies.at(2).status, "complete");
    EXPECT_FALSE(replies.contains(1));
}

TEST_F(TradeHistory, JournalAboveTradeCapIsRefusedEarly) {
    Journal writer(std::filesystem::path(dir.path().toStdString()) / "journal");
    writer.snapshot(0);
    for (int batch = 0; batch < 251; ++batch) {
        std::vector<nlohmann::json> prints;
        prints.reserve(1000);
        for (int row = 0; row < 1000; ++row)
            prints.push_back(trade(1000 + batch, std::to_string(batch * 1000 + row), 100000));
        writer.trades(1000 + batch, std::move(prints));
    }
    writer.heartbeat(70'000); writer.close();
    const auto page = request(1, Epoch, Epoch + 60'000);
    EXPECT_EQ(page.status, "refused");
    EXPECT_NE(page.reason.find("250000"), std::string::npos);
}

TEST_F(TradeHistory, LateLiveRowsAreSortedAndInvalidOnesAreSkipped) {
    live(2000, "later"); live(1000, "earlier");
    TradeOverlayModelTest::invalid(*model, 1500 + Epoch);
    const auto page = request(1, Epoch + 1000, Epoch + 3000);
    ASSERT_EQ(page.status, "partial");
    ASSERT_EQ(page.rows.size(), 2u);
    EXPECT_EQ(page.rows[0].id, "earlier");
    EXPECT_EQ(page.rows[1].id, "later");
}

TEST_F(TradeHistory, StaleLateTradeCannotLowerRetentionFloor) {
    const auto newest = TradeOverlayModelTest::retentionMs(*model) + 100'000;
    live(newest, "newest"); live(1000, "stale");
    const auto snapshot = model->tradeHistorySnapshot("BTC-USD", Epoch, Epoch + newest + 1000, 10);
    EXPECT_EQ(snapshot.retainedFromMs, Epoch + newest);
    ASSERT_EQ(snapshot.rows.size(), 1u);
    EXPECT_EQ(snapshot.rows.front().id, "newest");
}

TEST_F(TradeHistory, FutureEndCoverageIsClampedToExchangeNow) {
    live(1000, "a");
    const auto page = request(1, Epoch, Epoch + 180'000);
    EXPECT_EQ(page.status, "partial");
    EXPECT_EQ(page.coveredToMs, Epoch + 120'000);
}

TEST_F(TradeHistory, JournalSourceChangeReportsPartial) {
    journal();
    auto once = std::make_shared<std::atomic_bool>(false);
    journalHook = [this, once](std::stop_token) {
        if (once->exchange(true)) return;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(
                 std::filesystem::path(dir.path().toStdString()) / "journal")) {
            if (!entry.is_regular_file() || entry.path().extension() != ".rawl2") continue;
            const auto old = std::filesystem::last_write_time(entry.path());
            std::filesystem::last_write_time(entry.path(), old + 1s);
            break;
        }
    };
    const auto page = request(1, Epoch, Epoch + 4000);
    EXPECT_EQ(page.status, "partial");
    EXPECT_TRUE(page.rows.empty());
    EXPECT_EQ(page.coveredToMs, page.coveredFromMs);
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
    page.rows.push_back({"earlier", 1400, 100, 0.1, AggressorSide::Buy, 1});
    EXPECT_THROW(decode(protocol::tradewire::encode(page)), std::exception);
}

struct TradeHistoryWireClientTest {
    static void decode(SentinelStreamClient& client, const std::string& bytes) {
        client.decodeBinaryMessage({bytes.begin(), bytes.end()}, client.m_connectionEpoch.load());
    }
    static void request(SentinelStreamClient& client, uint64_t req) { client.m_tradeHistoryRequest.store(req); }
};

TEST(TradeHistoryWire, ClientAcceptsValidAndRejectsMalformedFrames) {
    int argc = 1;
    char name[16] = "trade-client";
    char* argv[2] = {name, nullptr};
    QCoreApplication app{argc, argv};
    SentinelStreamClient client{"127.0.0.1", "1"};
    TradeHistoryWireClientTest::request(client, 7);
    int accepted = 0, rejected = 0;
    QObject::connect(&client, &SentinelStreamClient::tradeHistoryReceived, &client,
                     [&](auto page) { if (page->req == 7 && page->rows.size() == 1) ++accepted; });
    QObject::connect(&client, &SentinelStreamClient::tradeHistoryFailed, &client,
                     [&](quint64 req, const QString&) { if (req == 7) ++rejected; });
    Page page; page.req = 7; page.symbol = "BTC-USD"; page.status = "complete";
    page.startMs = page.coveredFromMs = 1000;
    page.endMs = page.coveredToMs = 2000;
    page.rows.push_back({"a", 1500, 100, 0.1, AggressorSide::Buy, 1});
    const auto valid = protocol::tradewire::encode(page);
    TradeHistoryWireClientTest::decode(client, valid);
    auto truncated = valid; truncated.pop_back();
    TradeHistoryWireClientTest::decode(client, truncated);
    auto count = valid;
    count[count.size() - 31 - 1 - 4] = 2;
    TradeHistoryWireClientTest::decode(client, count);
    auto oversized = valid; oversized.resize(protocol::SentinelProtocol::kTradeHistoryMaxFrameBytes + 1);
    TradeHistoryWireClientTest::decode(client, oversized);
    EXPECT_EQ(accepted, 1);
    EXPECT_EQ(rejected, 3);
}
