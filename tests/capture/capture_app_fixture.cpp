#include "capture/CaptureApp.hpp"
#include "marketdata/MarketDataCoreEngine.hpp"
#include "marketdata/rest/CoinbaseRestClient.hpp"
#include "marketdata/fixtures/coinbase_messages.hpp"
#include "marketdata/fixtures/fake_ws_transport.hpp"
#include "SentinelLogging.hpp"
#include "SentinelLogSink.hpp"
#include <QCoreApplication>
#include <iostream>

using namespace std::chrono_literals;
int main(int argc, char** argv) {
    sentinel::logging::installLogSink("sentinel-capture", argc, argv);
    QCoreApplication app(argc, argv);
    sentinel::capture::ApplicationDependencies dependencies;
    dependencies.fetchMetadata = [](auto&, const std::string& symbol) {
        return ProductMetadataResult{true, {}, "0.01", "0.00000001",
            {{"product_id", symbol}, {"quote_increment", "0.01"}, {"base_increment", "0.00000001"}},
            "/api/v3/brokerage/market/products/" + symbol};
    };
    dependencies.makeEngine = [](auto& auth, const auto& config) {
        auto state = std::make_shared<fixtures::WsScenario>();
        state->onAttempt = [](auto& transport, int attempt) {
            transport.up();
            transport.frame(fixtures::coinbaseSubscriptionAck({"BTC-USD"}).dump());
            auto snapshot = fixtures::coinbaseL2Snapshot("BTC-USD", {{100, 1}}, {{101, 2}});
            snapshot["sequence_num"] = 1;
            transport.frame(" \n" + snapshot.dump(2) + "\t");
            auto trade = nlohmann::json::parse(R"({"channel":"market_trades","sequence_num":2,
                "timestamp":"2026-09-29T00:00:00Z","events":[{"type":"update","trades":[{
                "product_id":"BTC-USD","price":"100.01","size":"0.00000001","side":"BUY",
                "trade_id":"1","time":"2026-09-29T00:00:00Z"}]}]})");
            transport.frame(trade.dump());
            transport.heartbeats(10ms, 3);
            if (attempt == 1) transport.later(30ms, [&transport] { transport.down(); });
            else std::cout << "FIXTURE_READY\n" << std::flush;
        };
        return std::make_unique<MarketDataCoreEngine>(auth, config, [state](auto& io, auto&) {
            return std::make_unique<fixtures::FakeWsTransport>(io, state);
        }, MarketDataCoreEngine::ReconnectPolicy{20ms, 200ms, 10ms, 2s, 60ms});
    };
    try {
        const int result = sentinel::capture::runApplication(app, dependencies);
        sentinel::logging::flushLogSink();
        return result;
    } catch (const std::exception& e) {
        sLog_Error("Capture fixture failed: " << e.what());
        return 1;
    }
}
