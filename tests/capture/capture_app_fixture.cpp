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
    auto products = std::make_shared<std::vector<std::string>>();
    dependencies.fetchMetadata = [products](auto&, const std::string& symbol) {
        products->push_back(symbol);
        return ProductMetadataResult{true, {}, "0.01", "0.00000001",
            {{"product_id", symbol}, {"quote_increment", "0.01"}, {"base_increment", "0.00000001"}},
            "/api/v3/brokerage/market/products/" + symbol};
    };
    dependencies.makeEngine = [products](auto& auth, const auto& config) {
        std::cout << "FIXTURE_ENGINE\n" << std::flush;
        auto state = std::make_shared<fixtures::WsScenario>();
        state->onAttempt = [products, weak = std::weak_ptr(state)](auto& transport, int attempt) {
            transport.up();
            transport.frame(fixtures::coinbaseSubscriptionAck(*products).dump());
            uint64_t sequence = 1;
            for (const auto& product : *products) {
                auto snapshot = fixtures::coinbaseL2Snapshot(product, {{100, 1}}, {{101, 2}});
                snapshot["sequence_num"] = sequence++;
                transport.frame(" \n" + snapshot.dump(2) + "\t");
            }
            auto trade = nlohmann::json::parse(R"({"channel":"market_trades","sequence_num":2,
                "timestamp":"2026-09-29T00:00:00Z","events":[{"type":"update","trades":[{
                "product_id":"BTC-USD","price":"100.01","size":"0.00000001","side":"BUY",
                "trade_id":"1","time":"2026-09-29T00:00:00Z"}]}]})");
            trade["events"][0]["trades"][0]["product_id"] = products->front();
            trade["sequence_num"] = sequence++;
            transport.frame(trade.dump());
            transport.heartbeats(10ms, sequence);
            if (attempt == 1) transport.later(30ms, [&transport] { transport.down(); });
            else transport.later(20ms, [weak] {
                if (const auto state = weak.lock()) {
                    std::lock_guard lock(state->mutex);
                    std::cout << "FIXTURE_SENDS " << nlohmann::json(state->sends).dump() << "\nFIXTURE_READY\n" << std::flush;
                }
            });
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
