#include <gtest/gtest.h>
#include "marketdata/MarketDataFeeds.hpp"
#include "fixtures/coinbase_messages.hpp"
#include "fixtures/fake_ws_transport.hpp"
#include "servermodel/BookRecorder.hpp"
#include "servermodel/Hmc2Store.hpp"
#include "servermodel/TradeOverlayPublisher.hpp"
#include <QTemporaryDir>
#include <QtEndian>

namespace {
using namespace std::chrono_literals;
using Engine = MarketDataCoreEngine;
using Kind = Engine::IngestKind;
using fixtures::coinbaseL2Snapshot;
using fixtures::coinbaseL2Update;
struct FeedsTest : testing::Test {
    Authenticator auth{"/nonexistent-sentinel-test-credentials"};
    int64_t now = 1'000'000;
    MarketDataFeeds::Options options;
    std::unique_ptr<MarketDataFeeds> feeds;
    std::map<std::string, std::shared_ptr<fixtures::WsScenario>> scenarios;
    std::map<std::string, fixtures::FakeWsTransport*> transports;
    std::map<std::string, bool> valid;
    std::map<std::string, int> snapshots, updates, invalidations, statuses, errors;
    std::map<std::string, std::vector<int64_t>> attempts;
    std::vector<Trade> receivedTrades;
    std::vector<std::pair<std::string, bool>> statusEvents;
        void SetUp() override {
        options.manualPump = true;
        options.clock = [this] { return now; };
        options.jitter = [] { return 0ms; };
        options.limiter = std::make_shared<FeedConnectLimiter>();
        options.reconnect = {100ms, 800ms, 10ms, 20s, 100ms};
        options.transportFactory = [this](const std::string& product, auto& io, auto&) {
            auto& scenario = scenarios[product];
            if (!scenario) scenario = std::make_shared<fixtures::WsScenario>();
            if (!scenario->onAttempt) scenario->onAttempt = [this, product](auto& t, int) {
                attempts[product].push_back(now); t.up();
            };
            auto transport = std::make_unique<fixtures::FakeWsTransport>(io, scenario);
            transports[product] = transport.get();
            return transport;
        };
    }
    void create(std::vector<std::string> products = {"BTC-USD", "ETH-USD"}) {
        ServerMdcConfig config; config.sslCaBundle = SENTINEL_TEST_CA;
        feeds = std::make_unique<MarketDataFeeds>(auth, config, options);
        feeds->onLiveOrderBookInitialized([this](const auto& p, const auto&, const auto&, auto) { valid[p] = true; ++snapshots[p]; });
        feeds->onLiveOrderBookLevelUpdates([this](const auto& p, const auto&, auto) { ++updates[p]; });
        feeds->onLiveOrderBookInvalidated([this](const auto& p, const auto&) { valid[p] = false; ++invalidations[p]; });
        feeds->onConnectionStatus([this](const auto& p, bool up) { ++statuses[p]; statusEvents.emplace_back(p, up); });
        feeds->onError([this](const auto& p, const auto&) { ++errors[p]; });
        feeds->onTrade([this](const Trade& trade) { receivedTrades.push_back(trade); });
        for (const auto& p : products) EXPECT_EQ(feeds->add(p), MarketDataFeeds::AddResult::Added);
        feeds->start(); feeds->poll();
    }
    void advance(int64_t us) { now += us; feeds->poll(); }
    void advanceKeepingPeer(int64_t us) {
        while (us > 0) {
            update("BTC-USD");
            const auto step = std::min<int64_t>(us, 100'000);
            advance(step); us -= step;
        }
    }
    void send(const std::string& p, nlohmann::json frame) {
        // Most tests isolate the event under test from sequence checking.
        frame.erase("sequence_num"); transports.at(p)->frame(frame.dump()); feeds->poll();
    }
    void snapshot(const std::string& p) { send(p, coinbaseL2Snapshot(p, {{99, 2}}, {{101, 4}})); }
    void update(const std::string& p) { send(p, coinbaseL2Update(p, {{"bid", 99, 3}})); }
    void twoBooks() {
        create(); snapshot("BTC-USD"); advanceKeepingPeer(1'000'000); snapshot("BTC-USD"); snapshot("ETH-USD");
        ASSERT_TRUE(valid["BTC-USD"]); ASSERT_TRUE(valid["ETH-USD"]);
    }
    void peerUntouched() {
        EXPECT_TRUE(valid["BTC-USD"]);
        EXPECT_EQ(invalidations["BTC-USD"], 0);
        EXPECT_EQ(statuses["BTC-USD"], 1);
        EXPECT_EQ(scenarios["BTC-USD"]->closes, 0);
        EXPECT_EQ(scenarios["BTC-USD"]->attempts.size(), 1u);
        EXPECT_EQ(scenarios["BTC-USD"]->sends.size(), 3u);
        const auto before = updates["BTC-USD"]; update("BTC-USD");
        EXPECT_EQ(updates["BTC-USD"], before + 1);
    }
    void TearDown() override { if (feeds) feeds->stop(); }
};

TEST_F(FeedsTest, CoinbaseMakerSidesBecomeAggressorSidesBeforeDelivery) {
    create({"BTC-USD"});
    for (const auto& [side, size] : {std::pair{"BUY", 3}, std::pair{"SELL", 1}}) {
        auto frame = fixtures::coinbaseTrade("BTC-USD", 115, size, side);
        frame["events"] = nlohmann::json::array({{{"trades", frame["trades"]}}});
        frame.erase("trades");
        send("BTC-USD", frame);
    }
    ASSERT_EQ(receivedTrades.size(), 2u);
    EXPECT_EQ(receivedTrades[0].side, AggressorSide::Sell);
    EXPECT_EQ(receivedTrades[1].side, AggressorSide::Buy);
}

TEST_F(FeedsTest, CoinbaseTapeProducesAggressorSignedFootprintDelta) {
    create({"BTC-USD"});
    auto makerBuy = fixtures::coinbaseTrade("BTC-USD", 115, 3, "BUY");
    auto makerSell = fixtures::coinbaseTrade("BTC-USD", 115, 1, "SELL");
    for (auto* frame : {&makerBuy, &makerSell}) {
        (*frame)["events"] = nlohmann::json::array({{{"trades", (*frame)["trades"]}}});
        frame->erase("trades");
        (*frame)["events"][0]["trades"][0]["time"] = "2025-10-09T12:34:00.000Z";
    }
    send("BTC-USD", makerBuy);
    send("BTC-USD", makerSell);
    ASSERT_EQ(receivedTrades.size(), 2u);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        receivedTrades[0].timestamp.time_since_epoch()).count();
    std::vector<ServerDataModel::FootprintTradeSample> tape;
    for (const auto& trade : receivedTrades)
        tape.push_back({ms, trade.price, trade.size, trade.side});
    trade_overlay::Request request;
    request.symbol = "BTC-USD";
    request.grid = {16, 10, 2, 120};
    request.nowMs = ms + 1000;
    request.previousMs = ms;
    const auto result = trade_overlay::build(request, tape);
    ASSERT_TRUE(result.error.empty()) << result.error;
    ASSERT_FALSE(result.messages.empty());
    const auto column = nlohmann::json::parse(result.messages[0]);
    ASSERT_EQ(column.at("type"), "footprint_slice");
    const auto bytes = QByteArray::fromBase64(
        QByteArray::fromStdString(column.at("delta_levels_q16").get<std::string>()));
    ASSERT_GE(bytes.size(), 6);
    const int code = qFromLittleEndian<uint16_t>(reinterpret_cast<const uchar*>(bytes.constData()) + 4);
    EXPECT_NEAR((code - 32768) * column.at("quant_scale").get<double>(), -2.0, 0.001);
}

TEST_F(FeedsTest, SequenceGapInvalidatesAndReconnectsOnlyItsProduct) {
    twoBooks();
    auto heartbeat = nlohmann::json{{"channel", "heartbeats"}, {"sequence_num", 10}};
    transports["ETH-USD"]->frame(heartbeat.dump()); feeds->poll();
    heartbeat["sequence_num"] = 12;
    transports["ETH-USD"]->frame(heartbeat.dump()); feeds->poll();
    EXPECT_FALSE(valid["ETH-USD"]); EXPECT_EQ(scenarios["ETH-USD"]->closes, 1);
    advance(1'000'000); EXPECT_EQ(scenarios["ETH-USD"]->attempts.size(), 2u);
    peerUntouched();
}
TEST_F(FeedsTest, MalformedMessagesAndProviderErrorsAreIsolated) {
    for (const auto& bytes : {std::string("{invalid json"), std::string(R"({"type":"error","message":"bad product"})"),
         std::string(R"({"channel":"l2_data","events":[{"type":"update","product_id":"ETH-USD","updates":[{"side":"bid","price_level":"NaN","new_quantity":"1"}]}]})")}) {
        if (!feeds) twoBooks(); else snapshot("ETH-USD");
        auto closes = scenarios["ETH-USD"]->closes;
        transports["ETH-USD"]->frame(bytes); feeds->poll();
        EXPECT_FALSE(valid["ETH-USD"]); EXPECT_EQ(scenarios["ETH-USD"]->closes, closes + 1);
        advance(1'000'000); peerUntouched();
    }
}
TEST_F(FeedsTest, TransportDownIsIsolatedAndUpdatesWaitForSnapshot) {
    twoBooks(); transports["ETH-USD"]->down(); feeds->poll();
    EXPECT_FALSE(valid["ETH-USD"]); advance(1'000'000);
    update("ETH-USD"); EXPECT_EQ(updates["ETH-USD"], 0);
    snapshot("ETH-USD"); update("ETH-USD"); EXPECT_EQ(updates["ETH-USD"], 1);
    peerUntouched();
}
TEST_F(FeedsTest, RemoveClosesOnlyItsSocketAndNeverSendsUnsubscribe) {
    twoBooks(); auto eth = scenarios["ETH-USD"];
    EXPECT_TRUE(feeds->remove("ETH-USD")); feeds->poll();
    EXPECT_EQ(eth->closes, 1); EXPECT_EQ(eth->sends.size(), 3u);
    EXPECT_FALSE(feeds->remove("ETH-USD"));
    advance(1'000'000); EXPECT_EQ(eth->attempts.size(), 1u);
    peerUntouched(); EXPECT_EQ(feeds->stats().size(), 1u);
}
TEST_F(FeedsTest, HeartbeatSilenceReconnectsOnlySilentProduct) {
    twoBooks(); advance(19'000'000); update("BTC-USD"); advance(1'000'000);
    EXPECT_FALSE(valid["ETH-USD"]); EXPECT_EQ(scenarios["ETH-USD"]->closes, 1);
    peerUntouched(); advance(1'000'000); EXPECT_EQ(scenarios["ETH-USD"]->attempts.size(), 2u);
}
TEST_F(FeedsTest, Level2SilenceReconnectsWithDoublingRetryResetOnlyBySnapshotThenUpdate) {
    options.reconnect.level2Stale = 1s; options.reconnect.level2RetryMaximum = 4s;
    twoBooks();
    for (const int64_t interval : {1'000'000, 2'000'000, 4'000'000}) {
        update("BTC-USD"); send("ETH-USD", {{"channel", "heartbeats"}});
        auto closes = scenarios["ETH-USD"]->closes;
        advanceKeepingPeer(interval - 10'000); update("BTC-USD");
        EXPECT_EQ(scenarios["ETH-USD"]->closes, closes);
        advance(10'000); EXPECT_EQ(scenarios["ETH-USD"]->closes, closes + 1);
        EXPECT_FALSE(valid["ETH-USD"]); peerUntouched();
        advanceKeepingPeer(1'000'000); snapshot("ETH-USD"); // snapshot alone must not reset retry
    }
    update("ETH-USD"); update("BTC-USD");
    advance(990'000); update("BTC-USD"); advance(10'000);
    EXPECT_EQ(scenarios["ETH-USD"]->closes, 4); peerUntouched();
}
TEST_F(FeedsTest, ResnapshotCooldownIsPerProduct) {
    twoBooks(); feeds->requestResnapshot("ETH-USD"); feeds->poll(); advance(1'000'000); snapshot("ETH-USD");
    feeds->requestResnapshot("ETH-USD"); feeds->poll();
    EXPECT_EQ(scenarios["ETH-USD"]->closes, 1); peerUntouched();
    feeds->requestResnapshot("BTC-USD"); feeds->poll();
    EXPECT_EQ(scenarios["BTC-USD"]->closes, 1);
}
TEST_F(FeedsTest, CapRefusesWithoutCreatingTransportAndPinnedProductsAreExempt) {
    options.maxConnections = 1; create({});
    EXPECT_EQ(feeds->add("BTC-USD", true), MarketDataFeeds::AddResult::Added);
    EXPECT_EQ(feeds->add("ETH-USD"), MarketDataFeeds::AddResult::Added);
    EXPECT_EQ(feeds->add("SOL-USD"), MarketDataFeeds::AddResult::CapacityExceeded);
    EXPECT_FALSE(scenarios.contains("SOL-USD"));
    EXPECT_EQ(feeds->add("ETH-USD"), MarketDataFeeds::AddResult::AlreadyPresent);
    EXPECT_FALSE(feeds->remove("BTC-USD"));
    EXPECT_TRUE(feeds->remove("ETH-USD"));
    EXPECT_EQ(feeds->add("SOL-USD"), MarketDataFeeds::AddResult::Added);
}
TEST_F(FeedsTest, JitterAndProcessBucketBoundSimultaneousInitialAndReconnectAttempts) {
    int draws = 0;
    options.jitter = [&] { ++draws; return 1000ms; };
    std::vector<std::string> products;
    for (int i = 0; i < 8; ++i) products.push_back("P" + std::to_string(i));
    create(products);
    advance(999'999); EXPECT_TRUE(attempts.empty()); advance(1);
    for (int i = 1; i < 8; ++i) { advance(999'999); EXPECT_EQ(attempts.size(), size_t(i)); advance(1); }
    EXPECT_EQ(draws, 8);
    for (const auto& p : products) transports[p]->down(); feeds->poll();
    const auto base = now;
    advance(1'099'999);
    for (const auto& p : products) EXPECT_EQ(attempts[p].size(), 1u);
    advance(1);
    for (int i = 1; i < 8; ++i) advance(1'000'000);
    EXPECT_EQ(draws, 16);
    std::vector<int64_t> times;
    for (const auto& p : products) {
        ASSERT_EQ(attempts[p].size(), 2u);
        EXPECT_LE(attempts[p][1], base + 1'100'000 + 7 * 1'000'000);
        times.insert(times.end(), attempts[p].begin(), attempts[p].end());
    }
    std::sort(times.begin(), times.end());
    for (size_t i = 1; i < times.size(); ++i) EXPECT_GE(times[i] - times[i - 1], 1'000'000);
}
TEST_F(FeedsTest, PendingJitterIsCancelledByRemoveAndStatsExposeAgeAndReconnects) {
    options.jitter = [] { return 1000ms; }; create({"ETH-USD"});
    EXPECT_TRUE(feeds->remove("ETH-USD")); advance(2'000'000); EXPECT_TRUE(attempts.empty());
    feeds->add("BTC-USD"); feeds->poll(); advance(1'000'000); snapshot("BTC-USD"); advance(100'000);
    auto stats = feeds->stats(); ASSERT_EQ(stats.size(), 1u);
    EXPECT_TRUE(stats[0].up); EXPECT_EQ(stats[0].connection, 1u); EXPECT_EQ(stats[0].lastMessageAgeMs, 100);
    transports["BTC-USD"]->down(); feeds->poll(); advance(1'100'000);
    stats = feeds->stats(); EXPECT_EQ(stats[0].reconnects, 1u);
}
TEST_F(FeedsTest, IngestTapKeepsProductConnectionAndRawBytesBeforeDispatch) {
    options.jitter = [] { return 0ms; };
    ServerMdcConfig config; config.sslCaBundle = SENTINEL_TEST_CA;
    feeds = std::make_unique<MarketDataFeeds>(auth, config, options);
    std::vector<std::string> events;
    std::string raw;
    feeds->onIngest([&](const auto& observation) {
        EXPECT_EQ(observation.product, "BTC-USD");
        EXPECT_GE(observation.connection, 1u);
        if (observation.kind == Kind::TransportUp) events.push_back("up");
        if (observation.kind == Kind::Frame) { events.push_back("raw"); raw = observation.payload; }
    });
    feeds->onLiveOrderBookInitialized([&](auto&&...) { events.push_back("snapshot"); });
    struct Stop { MarketDataFeeds& feeds; ~Stop() { feeds.stop(); } } stop{*feeds};
    feeds->add("BTC-USD"); feeds->start(); feeds->poll();
    const auto bytes = " \n" + coinbaseL2Snapshot("BTC-USD", {{99, 2}}, {{101, 4}}).dump(2) + "\t";
    transports["BTC-USD"]->frame(bytes); feeds->poll();
    EXPECT_EQ(raw, bytes);
    EXPECT_EQ(events, (std::vector<std::string>{"up", "raw", "snapshot"}));
}

TEST_F(FeedsTest, FailedAttemptsBackOffDeduplicateDownAndResetAfterSnapshot) {
    options.reconnect.initialDelay = 1s;
    options.reconnect.maximumDelay = 4s;
    options.reconnect.staleHeartbeatDelay = 1s;
    auto state = scenarios["BTC-USD"] = std::make_shared<fixtures::WsScenario>();
    state->duplicateDowns = 4;
    state->onAttempt = [this](auto& t, int attempt) {
        attempts["BTC-USD"].push_back(now);
        if (attempt < 5) t.fail(); else t.up();
    };
    create({"BTC-USD"});
    for (const int64_t delay : {1'000'000, 2'000'000, 4'000'000, 4'000'000}) {
        const auto count = attempts["BTC-USD"].size();
        advance(delay - 1); EXPECT_EQ(attempts["BTC-USD"].size(), count);
        advance(1); EXPECT_EQ(attempts["BTC-USD"].size(), count + 1);
    }
    snapshot("BTC-USD");
    transports["BTC-USD"]->down(); feeds->poll();
    advance(999'999); EXPECT_EQ(attempts["BTC-USD"].size(), 5u);
    advance(1); EXPECT_EQ(attempts["BTC-USD"].size(), 6u);
    EXPECT_EQ(state->closes, 0);
}
TEST_F(FeedsTest, FailingProductCannotStarveQueuedPeersAndRemovedTicketDoesNotBlock) {
    auto state = scenarios["A"] = std::make_shared<fixtures::WsScenario>();
    state->onAttempt = [this](auto& t, int) { attempts["A"].push_back(now); t.fail(); };
    create({"A", "B", "C", "D"});
    EXPECT_TRUE(feeds->remove("B"));
    advance(1'000'000); EXPECT_EQ(attempts["C"].size(), 1u);
    advance(1'000'000); EXPECT_EQ(attempts["D"].size(), 1u);
    advance(1'000'000); EXPECT_EQ(attempts["A"].size(), 2u);
}
TEST_F(FeedsTest, MalformedBatchCannotAcceptLaterSnapshotOnClosingSocket) {
    twoBooks();
    auto batch = coinbaseL2Snapshot("ETH-USD", {{99, 2}}, {{101, 4}});
    auto malformed = batch["events"][0];
    malformed["updates"][0]["price_level"] = "NaN";
    batch["events"].insert(batch["events"].begin(), malformed);
    send("ETH-USD", batch);
    EXPECT_FALSE(valid["ETH-USD"]); EXPECT_EQ(snapshots["ETH-USD"], 1);
    peerUntouched();
}

struct RecorderFeed {
    static constexpr int64_t kT0 = 1'767'225'600'000; // 2026-01-01T00:00:00Z
    QTemporaryDir dir;
    std::atomic<int64_t> local{kT0};
    std::mutex mutex;
    std::vector<std::pair<std::string, std::string>> requests;
    std::unique_ptr<recording::BookRecorder> recorder;
    RecorderFeed(MarketDataFeeds& engine, size_t maxQueuedLevels) {
        recording::RecorderConfig config{dir.path().toStdString(), 100, {}, {{"near", 100, 0.5, 2, false}}, 0,
                                         maxQueuedLevels};
        config.onSelfInvalidated = [this, &engine](const std::string& symbol, const std::string& reason) {
            { std::lock_guard lock(mutex); requests.emplace_back(symbol, reason); }
            engine.requestResnapshot(symbol);
        };
        recorder = std::make_unique<recording::BookRecorder>(std::move(config), [this] { return local.load(); });
        engine.onLiveOrderBookInitialized([this](const std::string& product, const auto& bids, const auto& asks, int64_t ms) {
            std::vector<recording::Level> levels;
            for (const auto& l : bids) levels.push_back({true, l.price, l.size});
            for (const auto& l : asks) levels.push_back({false, l.price, l.size});
            recorder->onSnapshot(product, ms, std::move(levels));
        });
        engine.onLiveOrderBookLevelUpdates([this](const std::string& product, const auto& updates, int64_t ms) {
            std::vector<recording::Level> levels;
            for (const auto& u : updates) levels.push_back({u.isBid, u.price, u.quantity});
            recorder->onUpdates(product, ms, std::move(levels));
        });
        engine.onLiveOrderBookInvalidated([this](const std::string& product, const std::string& reason) {
            recorder->onInvalid(product, local.load(), reason);
        });
    }
    std::vector<recording::Hmc2Record> rows(const std::string& symbol) {
        recorder->drainForTest();
        return recording::Hmc2Store::readRange(dir.path().toStdString(), symbol, "near", 60000, kT0, kT0 + 3'600'000);
    }
    size_t requestCount() { std::lock_guard lock(mutex); return requests.size(); }
};
std::string frame(nlohmann::json message, int sequence) {
    message["sequence_num"] = sequence;
    return message.dump();
}
// Queue-level overflow depends on what is still queued, so the trigger alone
// exceeds the cap (7 > 6) while every other burst in a test fits under it.
const std::vector<std::tuple<std::string, double, double>> kOverflow{
    {"bid", 98, 1}, {"bid", 97, 1}, {"bid", 96, 1}, {"bid", 95, 1}, {"offer", 102, 1}, {"offer", 103, 1}, {"offer", 104, 1}};
double twap(const recording::Hmc2Record& r, int64_t row, bool ask) {
    for (const auto& e : r.entries)
        if (e.row == row && e.isAsk == ask) return recording::decodeSize(e.twapCode);
    return -1;
}

TEST_F(FeedsTest, RecorderSelfInvalidationResnapshotsAndRecordingResumes) {
    auto state = scenarios["BTC-USD"] = std::make_shared<fixtures::WsScenario>();
    state->onAttempt = [](auto& transport, int attempt) {
        transport.up();
        if (attempt == 1) {
            transport.frame(frame(coinbaseL2Snapshot("BTC-USD", {{99, 2}}, {{101, 4}}, "2026-01-01T00:00:00Z"), 0));
            transport.frame(frame(coinbaseL2Update("BTC-USD", kOverflow, "2026-01-01T00:00:10Z"), 1));
        } else {
            transport.frame(frame(coinbaseL2Snapshot("BTC-USD", {{99, 2}}, {{101, 4}}, "2026-01-01T00:00:30Z"), 0));
            transport.frame(frame(coinbaseL2Update("BTC-USD", {{"bid", 99, 3}}, "2026-01-01T00:01:10Z"), 1));
            transport.frame(frame(coinbaseL2Update("BTC-USD", {{"offer", 101, 5}}, "2026-01-01T00:02:10Z"), 2));
        }
    };
    ServerMdcConfig config; config.sslCaBundle = SENTINEL_TEST_CA;
    feeds = std::make_unique<MarketDataFeeds>(auth, config, options);
    RecorderFeed feed(*feeds, 6);
    struct Stop { MarketDataFeeds& feeds; ~Stop() { feeds.stop(); } } stop{*feeds};
    feeds->add("BTC-USD"); feeds->start(); feeds->poll();
    feed.recorder->drainForTest(); feeds->poll(); advance(1'000'000);
    feeds->stop();
    const auto rows = feed.rows("BTC-USD");
    EXPECT_EQ(state->attempts.size(), 2u);
    EXPECT_EQ(feed.recorder->stats().queueDrops, 1);
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[0].observedMs, 40000);
    EXPECT_NE(rows[0].flags & recording::kResynced, 0u);
    EXPECT_EQ(rows[1].observedMs, 60000);
    EXPECT_NEAR(twap(rows[1], 99, false), (2.0 * 10 + 3.0 * 50) / 60, 0.01);
}
TEST_F(FeedsTest, RecorderInvalidationKeepsPeerFullyObserved) {
    ServerMdcConfig config; config.sslCaBundle = SENTINEL_TEST_CA;
    feeds = std::make_unique<MarketDataFeeds>(auth, config, options);
    RecorderFeed feed(*feeds, 100);
    struct Stop { MarketDataFeeds& feeds; ~Stop() { feeds.stop(); } } stop{*feeds};
    feeds->add("BTC-USD"); feeds->add("ETH-USD"); feeds->start(); feeds->poll(); advance(1'000'000);
    const auto push = [&](const std::string& product, nlohmann::json message, int sequence, int64_t ms) {
        feed.local = RecorderFeed::kT0 + ms;
        transports[product]->frame(frame(std::move(message), sequence)); feeds->poll();
        feed.recorder->drainForTest();
    };
    for (const std::string p : {"BTC-USD", "ETH-USD"})
        push(p, coinbaseL2Snapshot(p, {{99, 2}}, {{101, 4}}, "2026-01-01T00:00:00Z"), 0, 0);
    push("ETH-USD", coinbaseL2Update("ETH-USD", {{"bid", 99, 3}}, "2026-01-01T00:00:20Z"), 2, 20000);
    advance(1'000'000);
    push("ETH-USD", coinbaseL2Snapshot("ETH-USD", {{99, 2}}, {{101, 4}}, "2026-01-01T00:00:30Z"), 0, 30000);
    for (const std::string p : {"BTC-USD", "ETH-USD"}) {
        push(p, coinbaseL2Update(p, {{"bid", 99, 3}}, "2026-01-01T00:01:10Z"), 1, 70000);
        push(p, coinbaseL2Update(p, {{"offer", 101, 5}}, "2026-01-01T00:02:10Z"), 2, 130000);
    }
    feeds->stop();
    const auto btc = feed.rows("BTC-USD"), eth = feed.rows("ETH-USD");
    ASSERT_EQ(btc.size(), 2u); ASSERT_EQ(eth.size(), 2u);
    EXPECT_EQ(btc[0].observedMs, 60000); EXPECT_EQ(btc[1].observedMs, 60000);
    EXPECT_EQ(eth[0].observedMs, 50000); EXPECT_EQ(eth[1].observedMs, 60000);
    EXPECT_EQ(scenarios["BTC-USD"]->attempts.size(), 1u);
    EXPECT_EQ(scenarios["ETH-USD"]->attempts.size(), 2u);
}
} // namespace

TEST_F(FeedsTest, FailedAttemptDoesNotConsumeConnectionIdOrCountAsReconnect) {
    auto state = scenarios["BTC-USD"] = std::make_shared<fixtures::WsScenario>();
    state->onAttempt = [](auto& t, int attempt) { if (attempt == 2) t.fail(); else t.up(); };
    create({"BTC-USD"}); snapshot("BTC-USD");
    transports["BTC-USD"]->down(); feeds->poll(); advance(1'000'000);
    auto stats = feeds->stats().at(0);
    EXPECT_FALSE(stats.up); EXPECT_EQ(stats.connection, 1u); EXPECT_EQ(stats.reconnects, 0u);
    advance(1'000'000);
    stats = feeds->stats().at(0);
    EXPECT_TRUE(stats.up); EXPECT_EQ(stats.connection, 2u); EXPECT_EQ(stats.reconnects, 1u);
}
TEST_F(FeedsTest, ProviderErrorOnEveryUpRetainsExponentialBackoffUntilSnapshot) {
    options.reconnect.initialDelay = 1s; options.reconnect.maximumDelay = 30s;
    options.reconnect.staleHeartbeatDelay = 1s;
    auto state = scenarios["BTC-USD"] = std::make_shared<fixtures::WsScenario>();
    state->onAttempt = [this](auto& t, int) {
        attempts["BTC-USD"].push_back(now); t.up();
        t.frame(R"({"type":"error","message":"persistent provider error"})");
    };
    create({"BTC-USD"});
    for (const int64_t seconds : {1, 2, 4, 8, 16, 30, 30}) {
        const auto count = attempts["BTC-USD"].size();
        advance(seconds * 1'000'000 - 1); EXPECT_EQ(attempts["BTC-USD"].size(), count);
        advance(1); EXPECT_EQ(attempts["BTC-USD"].size(), count + 1);
    }
}
TEST_F(FeedsTest, SubscriptionBatchesStayPacedWhenDelayedHandshakesCompleteTogether) {
    for (const auto* p : {"A", "B", "C"}) {
        auto state = scenarios[p] = std::make_shared<fixtures::WsScenario>();
        state->onAttempt = [](auto&, int) {}; // handshake held
    }
    create({"A", "B", "C"}); advance(1'000'000); advance(1'000'000);
    for (const auto* p : {"A", "B", "C"}) transports[p]->up();
    feeds->poll();
    const auto sent = [&] { size_t n = 0; for (const auto& [_, s] : scenarios) n += s->sends.size(); return n; };
    EXPECT_EQ(sent(), 3u); advance(999'999); EXPECT_EQ(sent(), 3u);
    advance(1); EXPECT_EQ(sent(), 6u);
    advance(999'999); EXPECT_EQ(sent(), 6u); advance(1); EXPECT_EQ(sent(), 9u);
}
TEST_F(FeedsTest, LateCallbackSettersFailExplicitly) {
    create({"BTC-USD"});
    EXPECT_THROW(feeds->onTrade({}), std::logic_error);
    EXPECT_THROW(feeds->onLiveOrderBookLevelUpdates({}), std::logic_error);
    EXPECT_THROW(feeds->onLiveOrderBookInitialized({}), std::logic_error);
    EXPECT_THROW(feeds->onLiveOrderBookInvalidated({}), std::logic_error);
    EXPECT_THROW(feeds->onConnectionStatus({}), std::logic_error);
    EXPECT_THROW(feeds->onError({}), std::logic_error);
    EXPECT_THROW(feeds->onLatency({}), std::logic_error);
    EXPECT_THROW(feeds->onIngest({}), std::logic_error);
}

namespace { std::vector<QString>* downAlarmMessages = nullptr; }
TEST_F(FeedsTest, DownOverTwoMinutesLogsPerProductErrorOncePerMinute) {
    std::vector<QString> messages;
    downAlarmMessages = &messages;
    const auto previous = qInstallMessageHandler([](QtMsgType type, const QMessageLogContext&, const QString& message) {
        if (type == QtCriticalMsg && message.contains("Feed down:")) downAlarmMessages->push_back(message);
    });
    struct Restore { QtMessageHandler previous; ~Restore() { qInstallMessageHandler(previous); downAlarmMessages = nullptr; } } restore{previous};
    auto state = scenarios["BTC-USD"] = std::make_shared<fixtures::WsScenario>();
    state->onAttempt = [](auto& t, int) { t.fail(); };
    create({"BTC-USD"});
    advance(119'999'999); EXPECT_TRUE(messages.empty());
    advance(1); ASSERT_EQ(messages.size(), 1u);
    EXPECT_TRUE(messages[0].contains("product=BTC-USD")); EXPECT_TRUE(messages[0].contains("conn=0"));
    advance(59'999'999); EXPECT_EQ(messages.size(), 1u);
    advance(1); EXPECT_EQ(messages.size(), 2u);
    transports["BTC-USD"]->up(); feeds->poll(); snapshot("BTC-USD");
    advance(60'000'000); EXPECT_EQ(messages.size(), 2u);
}

// N1: a product whose subscribe batch waits in the process bucket and is then
// reconnected must leave the queue at once. Its stale ticket would otherwise
// hold every other product's subscribe until its close completes (up to the 3 s
// close timeout, against Coinbase's 5 s subscribe deadline).
TEST_F(FeedsTest, ReconnectWhileWaitingToSubscribeReleasesTheSubscribeQueue) {
    for (const auto* p : {"A", "B", "C"}) {
        auto state = scenarios[p] = std::make_shared<fixtures::WsScenario>();
        state->onAttempt = [](auto&, int) {}; // handshake held
    }
    scenarios["B"]->closeDelay = 3s; // B's close stays pending through the test (real timer)
    create({"A", "B", "C"}); advance(1'000'000); advance(1'000'000);
    for (const auto* p : {"A", "B", "C"}) transports[p]->up();
    feeds->poll();
    ASSERT_EQ(scenarios["A"]->sends.size(), 3u); // A took the token; B, then C, wait
    ASSERT_EQ(scenarios["B"]->sends.size(), 0u);
    feeds->requestResnapshot("B"); feeds->poll();
    ASSERT_EQ(scenarios["B"]->closes, 1);
    advance(1'000'000);
    EXPECT_EQ(scenarios["C"]->sends.size(), 3u) << "C must not wait behind B's closing socket";
    EXPECT_EQ(scenarios["B"]->sends.size(), 0u);
}

// N2: after remove + re-add, errors from the retired (still closing) socket must
// not reach the product's error callback or log as that product: a new engine
// owns the name.
namespace { std::vector<QString>* retiredMessages = nullptr; }
TEST_F(FeedsTest, RetiredSocketErrorsDoNotSpeakForTheLiveProduct) {
    std::vector<QString> messages;
    retiredMessages = &messages;
    const auto previous = qInstallMessageHandler([](QtMsgType, const QMessageLogContext&, const QString& message) {
        retiredMessages->push_back(message);
    });
    struct Restore { QtMessageHandler previous; ~Restore() { qInstallMessageHandler(previous); retiredMessages = nullptr; } } restore{previous};
    twoBooks();
    scenarios["ETH-USD"]->closeDelay = 3s; // the retired socket keeps closing (real timer)
    auto* retired = transports["ETH-USD"];
    EXPECT_TRUE(feeds->remove("ETH-USD")); feeds->poll();
    EXPECT_EQ(feeds->add("ETH-USD"), MarketDataFeeds::AddResult::Added);
    advance(1'000'000); snapshot("ETH-USD");
    ASSERT_NE(transports["ETH-USD"], retired);
    ASSERT_TRUE(valid["ETH-USD"]);
    messages.clear();
    retired->fail(); feeds->poll(); // e.g. "close timed out" on the old socket
    EXPECT_EQ(errors["ETH-USD"], 0);
    for (const auto& message : messages)
        EXPECT_FALSE(message.contains("product=ETH-USD") && message.contains("error=")) << message.toStdString();
    EXPECT_TRUE(std::any_of(messages.begin(), messages.end(), [](const QString& m) {
        return m.contains("Retired feed socket error ignored: retiredProduct=ETH-USD"); }));
    EXPECT_TRUE(valid["ETH-USD"]); // the live engine is untouched
    update("ETH-USD"); EXPECT_EQ(updates["ETH-USD"], 1);
}


TEST_F(FeedsTest, LifecycleEventsBracketDataAndPinnedRemovalEmitsNothing) {
    create({}); // setters remain mutable until the first successful add
    std::vector<std::string> events;
    feeds->onFeedLifecycle([&](const std::string& p, bool acquired) {
        events.push_back((acquired ? "acquire:" : "release:") + p);
    });
    feeds->onLiveOrderBookInitialized([&](const auto& p, const auto&, const auto&, auto) {
        events.push_back("snapshot:" + p);
    });
    feeds->onConnectionStatus([&](const auto& p, bool up) {
        events.push_back((up ? "up:" : "down:") + p);
    });
    struct Stop { MarketDataFeeds& feeds; ~Stop() { feeds.stop(); } } stop{*feeds};
    feeds->add("BTC-USD", true); feeds->poll();
    feeds->add("ETH-USD"); advance(1000000); snapshot("ETH-USD");
    EXPECT_LT(std::find(events.begin(), events.end(), "acquire:ETH-USD"),
              std::find(events.begin(), events.end(), "snapshot:ETH-USD"));
    events.clear();
    EXPECT_FALSE(feeds->remove("BTC-USD"));
    EXPECT_TRUE(events.empty());
    EXPECT_TRUE(feeds->remove("ETH-USD"));
    feeds->add("ETH-USD"); advance(1000000); snapshot("ETH-USD");
    ASSERT_GE(events.size(), 3u);
    EXPECT_EQ(events[0], "release:ETH-USD");
    EXPECT_EQ(events[1], "acquire:ETH-USD");
    EXPECT_EQ(std::count(events.begin(), events.end(), "down:ETH-USD"), 0);
    EXPECT_EQ(events.back(), "snapshot:ETH-USD");
}
