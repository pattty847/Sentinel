#include <gtest/gtest.h>
#include "marketdata/MarketDataCoreEngine.hpp"
#include "fixtures/coinbase_messages.hpp"
#include "fixtures/fake_ws_transport.hpp"
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
} // namespace
