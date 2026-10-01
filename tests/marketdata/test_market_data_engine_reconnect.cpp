#include <gtest/gtest.h>
#include "marketdata/MarketDataCoreEngine.hpp"
#include "fixtures/coinbase_messages.hpp"
#include "fixtures/fake_ws_transport.hpp"
#include "servermodel/BookRecorder.hpp"
#include "servermodel/Hmc2Store.hpp"
#include <QTemporaryDir>
#include <future>

namespace {
using namespace std::chrono_literals;
using Engine = MarketDataCoreEngine;
using Kind = Engine::IngestKind;
struct StopEngine { Engine* engine; ~StopEngine() { engine->stop(); } };
struct EngineReconnect : testing::Test {
    Authenticator auth{"/nonexistent-sentinel-test-credentials"};
    std::shared_ptr<fixtures::WsScenario> scenario = std::make_shared<fixtures::WsScenario>();
    std::unique_ptr<Engine> engine;
    Engine::ReconnectPolicy policy{20ms, 200ms, 5ms, 2s, 60ms};
    void create() {
        ServerMdcConfig config;
        config.sslCaBundle = SENTINEL_TEST_CA;
        engine = std::make_unique<Engine>(auth, config, [state = scenario](auto& io, auto&) {
            return std::make_unique<fixtures::FakeWsTransport>(io, state);
        }, policy);
        engine->subscribeToSymbols({"BTC-USD"});
    }
    void TearDown() override { if (engine) engine->stop(); }
};
TEST_F(EngineReconnect, InitialFailuresRetryWithCappedBackoffDuplicateDownDedupAndSuccessReset) {
    scenario->duplicateDowns = 4;
    scenario->onAttempt = [](auto& transport, int attempt) {
        if (attempt <= 6) transport.fail();
        else {
            transport.up();
            if (attempt == 7) transport.later(25ms, [&transport] { transport.down(); });
        }
    };
    create(); engine->start();
    ASSERT_TRUE(scenario->wait([](auto& s) { return s.ups == 2 && s.sends.size() == 6; }));
    engine->stop();
    ASSERT_EQ(scenario->attempts.size(), 8);
    ASSERT_EQ(scenario->downs.size(), 7);
    const std::vector expected{20ms, 40ms, 80ms, 160ms, 200ms, 200ms, 20ms};
    for (size_t i = 0; i < expected.size(); ++i) {
        const auto actual = scenario->attempts[i + 1] - scenario->downs[i].second;
        EXPECT_GE(actual, expected[i] - 1ms) << i;
        EXPECT_LT(actual, expected[i] + 100ms) << i;
    }
    EXPECT_EQ(scenario->closes, 0); // never close a failed transport again before connect
    for (size_t i = 0; i < scenario->sends.size(); ++i) {
        const auto request = nlohmann::json::parse(scenario->sends[i]);
        EXPECT_EQ(request["type"], "subscribe");
        EXPECT_EQ(request["channel"], (std::vector{"level2", "market_trades", "heartbeats"}[i % 3]));
    }
}
TEST_F(EngineReconnect, WatchdogRearmsAcrossFailedRetriesAndASecondStaleConnection) {
    policy.heartbeatStale = 30ms;
    scenario->duplicateDowns = 3;
    scenario->onAttempt = [](auto& transport, int attempt) {
        if (attempt == 2 || attempt == 3) transport.fail();
        else {
            transport.up();
            if (attempt >= 5) transport.heartbeats(5ms);
        }
    };
    create();
    std::vector<std::string> resyncs;
    StopEngine stop{engine.get()};
    engine->onIngest([&](const auto& event) {
        if (event.kind == Kind::ResyncRequested) resyncs.emplace_back(event.reason);
    });
    engine->start();
    ASSERT_TRUE(scenario->wait([](auto& s) { return s.frames >= 10; }));
    engine->stop();
    EXPECT_EQ(scenario->attempts.size(), 5);
    EXPECT_EQ(scenario->closes, 2);
    EXPECT_EQ(resyncs, (std::vector<std::string>{"stale heartbeat", "stale heartbeat"}));
}
TEST_F(EngineReconnect, HealthyConnectionKeepsOneWatchdogAndDoesNotReconnect) {
    policy.heartbeatStale = 30ms;
    scenario->onAttempt = [](auto& transport, int) { transport.up(); transport.heartbeats(5ms); };
    create(); engine->start();
    ASSERT_TRUE(scenario->wait([](auto& s) { return s.frames >= 30; }));
    engine->stop();
    EXPECT_EQ(scenario->attempts.size(), 1); EXPECT_EQ(scenario->closes, 0);
}
TEST_F(EngineReconnect, RawHookPrecedesParsingAndUpPrecedesFramesAcrossResync) {
    auto snapshot = fixtures::coinbaseL2Snapshot("BTC-USD", {{100, 1}}, {{101, 2}}).dump(2);
    snapshot = " \n" + snapshot + "\t";
    const std::string malformed = "  { definitely not JSON }\n";
    scenario->onAttempt = [snapshot, malformed](auto& transport, int attempt) {
        transport.up();
        transport.frame(snapshot);
        if (attempt == 1) {
            transport.frame(malformed);
            auto gap = fixtures::coinbaseL2Update("BTC-USD", {{"bid", 100, 2}});
            gap["sequence_num"] = 2;
            transport.frame(gap.dump());
        }
    };
    create();
    struct Event { std::string kind, bytes; int connection; };
    std::vector<Event> observed;
    int connection = 0;
    StopEngine stop{engine.get()};
    engine->onIngest([&](const auto& event) {
        if (event.kind == Kind::TransportUp) observed.push_back({"up", {}, ++connection});
        if (event.kind == Kind::Frame) observed.push_back({"raw", std::string(event.payload), connection});
        if (event.kind == Kind::BookInvalidated) observed.push_back({"invalid-hook", std::string(event.reason), connection});
    });
    engine->onLiveOrderBookInitialized([&](auto&&...) { observed.push_back({"parsed", {}, connection}); });
    engine->onLiveOrderBookInvalidated([&](const auto&, const auto& reason) { observed.push_back({"invalid-callback", reason, connection}); });
    engine->start();
    ASSERT_TRUE(scenario->wait([](auto& s) { return s.ups == 2 && s.frames == 4; }));
    engine->stop();
    ASSERT_GE(observed.size(), 6);
    EXPECT_EQ(observed[0].kind, "up"); EXPECT_EQ(observed[0].connection, 1);
    EXPECT_EQ(observed[1].kind, "raw"); EXPECT_EQ(observed[1].bytes, snapshot);
    EXPECT_EQ(observed[2].kind, "parsed"); EXPECT_EQ(observed[3].bytes, malformed);
    for (size_t i = 0; i < observed.size(); ++i) {
        if (observed[i].kind == "up") {
            ASSERT_LT(i + 2, observed.size());
            EXPECT_EQ(observed[i + 1].kind, "raw"); EXPECT_EQ(observed[i + 2].kind, "parsed");
            EXPECT_EQ(observed[i + 1].connection, observed[i].connection);
        }
        if (observed[i].kind == "invalid-hook") {
            ASSERT_LT(i + 1, observed.size());
            EXPECT_EQ(observed[i + 1].kind, "invalid-callback");
            EXPECT_EQ(observed[i].bytes, observed[i + 1].bytes);
        }
    }
    EXPECT_EQ(connection, 2);
}
TEST_F(EngineReconnect, StopCancelsPendingRetry) {
    policy.initialDelay = 100ms; policy.staleHeartbeatDelay = 100ms;
    scenario->onAttempt = [](auto& transport, int) { transport.fail(); };
    create(); engine->start();
    ASSERT_TRUE(scenario->wait([](auto& s) { return !s.downs.empty(); }));
    engine->stop();
    EXPECT_FALSE(scenario->wait([](auto& s) { return s.attempts.size() > 1; }, 150ms));
    EXPECT_EQ(scenario->attempts.size(), 1);
}
TEST_F(EngineReconnect, WatchdogDoesNotCloseAgainWhileWaitingForTransportDown) {
    policy.heartbeatStale = 20ms;
    scenario->closeDelay = 120ms;
    scenario->onAttempt = [](auto& transport, int attempt) {
        transport.up();
        if (attempt > 1) transport.heartbeats(5ms);
    };
    create(); engine->start();
    ASSERT_TRUE(scenario->wait([](auto& s) { return s.frames >= 10; }));
    engine->stop();
    EXPECT_EQ(scenario->attempts.size(), 2); EXPECT_EQ(scenario->closes, 1);
}
// The recorder drops a book on its own (here: queue level overflow). Its request
// must reach the engine, which reconnects; the fresh snapshot resumes recording.
TEST_F(EngineReconnect, RecorderSelfInvalidationResnapshotsAndRecordingResumes) {
    constexpr int64_t kT0 = 1'767'225'600'000; // 2026-01-01T00:00:00Z
    scenario->onAttempt = [](auto& transport, int attempt) {
        transport.up();
        if (attempt == 1) {
            transport.frame(fixtures::coinbaseL2Snapshot("BTC-USD", {{99, 2}}, {{101, 4}}, "2026-01-01T00:00:00Z").dump());
            transport.frame(fixtures::coinbaseL2Update("BTC-USD", {{"bid", 99, 7}, {"offer", 101, 8}, {"bid", 98, 1}},
                                                       "2026-01-01T00:00:10Z").dump());
        } else {
            transport.frame(fixtures::coinbaseL2Snapshot("BTC-USD", {{99, 2}}, {{101, 4}}, "2026-01-01T00:00:30Z").dump());
            transport.frame(fixtures::coinbaseL2Update("BTC-USD", {{"bid", 99, 3}}, "2026-01-01T00:01:10Z").dump());
        }
    };
    create();
    QTemporaryDir dir;
    recording::RecorderConfig config{dir.path().toStdString(), 100, {}, {{"near", 100, 0.5, 2, false}}, 0, 2};
    config.onSelfInvalidated = [this](const std::string& symbol, const std::string&) { engine->requestResnapshot(symbol); };
    recording::BookRecorder recorder(std::move(config), [] { return kT0; });
    engine->onLiveOrderBookInitialized([&](const std::string& product, const auto& bids, const auto& asks, int64_t envelopeMs) {
        std::vector<recording::Level> levels;
        for (const auto& l : bids) levels.push_back({true, l.price, l.size});
        for (const auto& l : asks) levels.push_back({false, l.price, l.size});
        recorder.onSnapshot(product, envelopeMs, std::move(levels));
    });
    engine->onLiveOrderBookLevelUpdates([&](const std::string& product, const auto& updates, int64_t exchangeMs) {
        std::vector<recording::Level> levels;
        for (const auto& u : updates) levels.push_back({u.isBid, u.price, u.quantity});
        recorder.onUpdates(product, exchangeMs, std::move(levels));
    });
    engine->onLiveOrderBookInvalidated([&](const std::string& product, const std::string& reason) {
        recorder.onInvalid(product, kT0, reason);
    });
    std::vector<std::string> resyncs;
    engine->onIngest([&](const auto& event) {
        if (event.kind == Kind::ResyncRequested) resyncs.emplace_back(event.reason);
    });
    engine->start();
    ASSERT_TRUE(scenario->wait([](auto& s) { return s.ups == 2 && s.frames == 4; }));
    engine->stop();
    recorder.drainForTest();
    EXPECT_EQ(scenario->attempts.size(), 2);
    EXPECT_EQ(resyncs, (std::vector<std::string>{"resnapshot BTC-USD"}));
    const auto rows = recording::Hmc2Store::readRange(dir.path().toStdString(), "BTC-USD", "near", 60000, kT0,
                                                      kT0 + 3'600'000);
    ASSERT_EQ(rows.size(), 1);
    EXPECT_EQ(rows[0].bucketStartMs, kT0);
    EXPECT_EQ(rows[0].observedMs, 40000); // 10 s before the overflow + 30 s after the fresh snapshot
    EXPECT_NE(rows[0].flags & recording::kResynced, 0u);
}
} // namespace
