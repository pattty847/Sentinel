#include "capture/CaptureApp.hpp"
#include "marketdata/MarketDataFeeds.hpp"
#include "marketdata/rest/CoinbaseRestClient.hpp"
#include "marketdata/fixtures/coinbase_messages.hpp"
#include "marketdata/fixtures/fake_ws_transport.hpp"
#include "SentinelLogging.hpp"
#include "SentinelLogSink.hpp"
#include <QCoreApplication>
#include <algorithm>
#include <iostream>
#include <string_view>
#include <vector>

using namespace std::chrono_literals;
int main(int argc, char** argv) {
    // Tests never bind the production 127.0.0.1:8091 unless they ask for a port.
    std::vector<char*> args(argv, argv + argc);
    static char metricsFlag[] = "--metrics-port", metricsOff[] = "0";
    if (std::none_of(args.begin(), args.end(), [](const char* a) { return std::string_view(a) == "--metrics-port"; })) {
        args.push_back(metricsFlag); args.push_back(metricsOff);
    }
    args.push_back(nullptr);
    argc = int(args.size()) - 1; argv = args.data();
    sentinel::logging::installLogSink("sentinel-capture", argc, argv);
    QCoreApplication app(argc, argv);
    sentinel::capture::ApplicationDependencies dependencies;
    auto count = std::make_shared<size_t>(0);
    dependencies.fetchMetadata = [count](auto&, const std::string& symbol) {
        ++*count;
        return ProductMetadataResult{true, {}, "0.01", "0.00000001",
            {{"product_id", symbol}, {"quote_increment", "0.01"}, {"base_increment", "0.00000001"}},
            "/api/v3/brokerage/market/products/" + symbol};
    };
    const bool uneven = qEnvironmentVariableIsSet("SENTINEL_FIXTURE_UNEVEN");
    dependencies.makeFeeds = [count, uneven](auto& auth, const auto& config) {
        MarketDataFeeds::Options options;
        options.jitter = [] { return 0ms; };
        options.reconnect.initialDelay = 100ms;
        auto ready = std::make_shared<size_t>(0);
        options.transportFactory = [count, ready, uneven](const std::string& product, auto& io, auto&) {
            std::cout << "FIXTURE_ENGINE\n" << std::flush;
            auto state = std::make_shared<fixtures::WsScenario>();
            state->onAttempt = [](auto& transport, int attempt) {
                if (attempt == 2) transport.fail(); else transport.up();
            };
            state->onFrame = [](size_t sends, int attempt) {
                if (sends < size_t(attempt > 2 ? attempt - 1 : attempt) * 3) std::cout << "FIXTURE_EARLY_FRAME\n" << std::flush;
            };
            state->onSend = [product, count, ready, uneven, weak = std::weak_ptr(state)](auto& transport, size_t sends) {
                if (sends % 3) return;
                if (uneven && product == "ETH-USD" && sends == 3) {
                    transport.down(); // established but empty connection must be audited
                    return;
                }
                transport.frame(fixtures::coinbaseSubscriptionAck({product}).dump());
                auto snapshot = fixtures::coinbaseL2Snapshot(product, {{100, 1}}, {{101, 2}});
                snapshot["sequence_num"] = 1;
                transport.frame(" \n" + snapshot.dump(2) + "\t");
                auto trade = nlohmann::json::parse(R"({"channel":"market_trades","sequence_num":2,
                    "timestamp":"2026-09-29T00:00:00Z","events":[{"type":"update","trades":[{
                    "product_id":"BTC-USD","price":"100.01","size":"0.00000001","side":"BUY",
                    "trade_id":"1","time":"2026-09-29T00:00:00Z"}]}]})");
                trade["events"][0]["trades"][0]["product_id"] = product;
                trade["events"][0]["trades"][0]["trade_id"] = std::to_string(uneven && product == "BTC-USD" && sends == 6 ? 4 : sends / 3);
                transport.frame(trade.dump());
                const size_t connections = uneven ? (product == "ETH-USD" ? 3 : 2) : 2;
                if (sends / 3 < connections) transport.down();
                else {
                    transport.heartbeats(500ms, 3);
                    if (auto s = weak.lock()) {
                        std::lock_guard lock(s->mutex);
                        std::cout << "FIXTURE_SENDS " << nlohmann::json(s->sends).dump() << '\n';
                    }
                    if (++*ready == *count) std::cout << "FIXTURE_READY\n" << std::flush;
                }
            };
            return std::make_unique<fixtures::FakeWsTransport>(io, state);
        };
        return std::make_unique<MarketDataFeeds>(auth, config, std::move(options));
    };
    try {
        const int result = sentinel::capture::runApplication(app, dependencies);
        sentinel::logging::flushLogSink(); return result;
    } catch (const std::exception& e) { sLog_Error("Capture fixture failed: " << e.what()); return 1; }
}
