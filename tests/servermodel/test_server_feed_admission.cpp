// Actual Session admission/close/write paths, injected upstream transports, no sockets/services.
#include "protocol/SentinelStreamServer.cpp"
#include "protocol/SentinelStreamClient.hpp"
#include "marketdata/MarketDataFeeds.hpp"
#include "marketdata/auth/Authenticator.hpp"
#include "marketdata/fixtures/fake_ws_transport.hpp"
#include "ConfigLoader.hpp"
#include "mainwindow/SymbolSubscriptionManager.hpp"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QFile>
#include <QScopeGuard>
#include <gtest/gtest.h>
#include <fstream>

struct FeedClockModel : ServerDataModel {
    int64_t& clock;
    FeedClockModel(const ServerConfig& c, int64_t& now) : ServerDataModel(c), clock(now) {}
    int64_t exchangeNowMs() const override { return clock; }
};

struct ServerFeedAdmissionTest : testing::Test {
    int argc = 1;
    char name[20] = "feed-admission";
    char* argv[2] = {name, nullptr};
    QCoreApplication app{argc, argv};
    QTemporaryDir dir;
    Authenticator auth{"/nonexistent-sentinel-test-credentials"};
    ServerConfig config;
    std::unique_ptr<ServerDataModel> model;
    std::unique_ptr<SentinelStreamServer> server;
    std::unique_ptr<MarketDataFeeds> feeds;
    sentinel::metrics::MetricsRegistry metrics;
    net::io_context io;
    ssl::context tls{ssl::context::tls_server};
    std::vector<std::shared_ptr<Session>> sessions;
    std::map<std::string, std::shared_ptr<fixtures::WsScenario>> transports;
    int64_t now = 1'000'000;
    int64_t recordingLocal = recording::kHmc2MinMs;

    void SetUp() override {
        config.recording.enabled = false;
        config.heatmap.persistenceEnabled = false;
        config.defaultSymbols = {"btc-usd", "SOL-USD"};
        config.mdc.maxConnections = 2;
        config.mdc.sslCaBundle = std::string(SENTINEL_SOURCE_DIR) + "/resources/certs/ca-bundle.crt";
        model = std::make_unique<FeedClockModel>(config, recordingLocal);
        server = std::make_unique<SentinelStreamServer>(*model, auth, config, 0);
        model->registerMetrics(metrics);
        server->registerMetrics(metrics);
        MarketDataFeeds::Options options;
        options.manualPump = true;
        options.maxConnections = config.mdc.maxConnections;
        options.clock = [this] { return now; };
        options.limiter = std::make_shared<FeedConnectLimiter>();
        options.transportFactory = [this](const auto& symbol, auto& io, auto&) {
            auto scenario = std::make_shared<fixtures::WsScenario>();
            scenario->onAttempt = [](auto& t, int) { t.up(); };
            transports[symbol] = scenario;
            return std::make_unique<fixtures::FakeWsTransport>(io, scenario);
        };
        feeds = std::make_unique<MarketDataFeeds>(auth, config.mdc, options);
        feeds->onFeedLifecycle([this](const std::string& symbol, bool acquired) {
            const auto releasedAt = recordingLocal;
            QMetaObject::invokeMethod(model.get(), [this, symbol, acquired, releasedAt] {
                if (acquired) model->acquireGuiFeed(symbol);
                else model->releaseGuiFeed(symbol, releasedAt);
            }, Qt::QueuedConnection);
        });
        feeds->onConnectionStatus([this](const auto& symbol, bool up) {
            QMetaObject::invokeMethod(model.get(), [this, symbol, up] {
                model->onMarketDataConnectionChanged(symbol, up);
            }, Qt::QueuedConnection);
        });
        QObject::connect(server.get(), &SentinelStreamServer::clientSubscribed, model.get(), [this](const QString& symbol) {
            const auto result = feeds->add(symbol.toStdString());
            EXPECT_TRUE(result == MarketDataFeeds::AddResult::Added || result == MarketDataFeeds::AddResult::AlreadyPresent);
        }, Qt::QueuedConnection);
        server->setFeedAdmissionHandler([this](const std::string& symbol) {
            QMetaObject::invokeMethod(model.get(), [this, symbol] { model->acquireGuiFeed(symbol); },
                                      Qt::QueuedConnection);
            const auto result = feeds->add(symbol);
            if (result == MarketDataFeeds::AddResult::CapacityExceeded) {
                QMetaObject::invokeMethod(model.get(), [this, symbol] { model->releaseGuiFeed(symbol); },
                                          Qt::QueuedConnection);
                return SentinelStreamServer::FeedAdmission::CapacityExceeded;
            }
            if (result == MarketDataFeeds::AddResult::InvalidProduct) {
                QMetaObject::invokeMethod(model.get(), [this, symbol] { model->releaseGuiFeed(symbol); },
                                          Qt::QueuedConnection);
                return SentinelStreamServer::FeedAdmission::InvalidProduct;
            }
            return SentinelStreamServer::FeedAdmission::Accepted;
        });
        QObject::connect(server.get(), &SentinelStreamServer::clientUnsubscribed, model.get(), [this](const QString& symbol) {
            feeds->remove(symbol.toStdString());
            const auto native = symbol.toStdString();
            server->releaseIfNoSubscribers(native, [this, &native] {
                feeds->remove(native);
                model->releaseGuiFeed(native);
            });
        }, Qt::QueuedConnection);
        for (const auto& symbol : normalizedDefaultSymbols(config.defaultSymbols)) feeds->add(symbol, true);
        feeds->start();
        drain();
    }
    void drain() {
        for (int i = 0; i < 4; ++i) {
            QCoreApplication::sendPostedEvents();
            feeds->poll();
            io.restart(); io.poll();
        }
    }
    std::shared_ptr<Session> session() {
        auto s = std::make_shared<Session>(tcp::socket(io), tls, *model, server.get());
        // A sentinel in-flight write holds outgoing frames for inspection without TLS.
        s->write_queue_.push_back({"in-flight", false});
        s->pendingWriteBytes_ = 9;
        sessions.push_back(s);
        server->registerSession(s);
        return s;
    }
    nlohmann::json request(const std::shared_ptr<Session>& s, const std::string& symbol, const char* type = "subscribe") {
        s->handle_message(nlohmann::json{{"type", type}, {"symbol", symbol}}.dump());
        drain();
        return s->write_queue_.size() > 1 ? nlohmann::json::parse(s->write_queue_.back().payload) : nlohmann::json{};
    }
    void TearDown() override {
        for (auto& s : sessions) s->beginClose("fixture cleanup");
        drain();
        sessions.clear();
        feeds->stop();
        QCoreApplication::sendPostedEvents();
    }
    void checkCap();
    void checkMetrics();
    void checkGuiTenSwitches();
    void checkGuiRefusal();
    void checkRecorderRelease();
    void checkLegacyRelease();
    void checkTradeWire();
    bool sessionHas(const std::shared_ptr<Session>& s, const std::string& symbol) const {
        return s->subscriptions_.contains(symbol);
    }
    bool slotHeld(const std::string& symbol) const {
        return server->m_symbolSubscriptions.contains(symbol);
    }
    static void deliver(SentinelStreamClient& client, const std::string& message) { client.handleMessage(message); }
};

void ServerFeedAdmissionTest::checkCap() {
    auto a = session(), b = session();
    request(a, "btc-usd");
    request(a, "ETH-USD"); request(a, "DOGE-USD");
    for (int i = 0; i < 3; ++i) { now += 1'000'000; drain(); }
    request(a, "SOL-USD"); // a newly watched pinned product is admitted even at the cap
    request(b, "ETH-USD"); // shared product consumes one slot
    const auto before = b->write_queue_.size();
    const auto reply = request(b, "XRP-USD");
    EXPECT_EQ(b->write_queue_.size(), before + 1); // error only: no ack or snapshot
    ASSERT_EQ(reply.value("type", ""), "error");
    EXPECT_EQ(reply.value("context", ""), "subscribe");
    EXPECT_EQ(reply.value("code", ""), "connection_cap");
    EXPECT_EQ(reply.value("symbol", ""), "XRP-USD");
    EXPECT_EQ(reply.value("max_connections", 0), 2);
    EXPECT_NE(reply.value("message", "").find("XRP-USD"), std::string::npos);
    EXPECT_FALSE(b->subscriptions_.contains("XRP-USD"));
    EXPECT_FALSE(b->availability_.contains("XRP-USD"));
    EXPECT_FALSE(server->m_symbolSubscriptions.contains("XRP-USD"));
    EXPECT_FALSE(transports.contains("XRP-USD"));
    EXPECT_EQ(model->getSymbolsSnapshot().size(), 4u);
    EXPECT_EQ(feeds->stats().size(), 4u); // two pinned + two GUI
    request(a, "ETH-USD", "unsubscribe");
    EXPECT_EQ(request(b, "XRP-USD").value("type", ""), "error"); // b still watches ETH
    request(b, "ETH-USD", "unsubscribe");
    request(b, "XRP-USD");
    EXPECT_TRUE(b->subscriptions_.contains("XRP-USD"));
    EXPECT_TRUE(transports.contains("XRP-USD"));
    EXPECT_EQ(transports.at("ETH-USD")->closes, 1);
    b->beginClose("client gone"); drain();
    EXPECT_FALSE(server->m_symbolSubscriptions.contains("XRP-USD"));
    request(a, "ADA-USD");
    EXPECT_TRUE(a->subscriptions_.contains("ADA-USD"));
    EXPECT_EQ(transports.at("BTC-USD")->closes, 0);
    EXPECT_EQ(transports.at("SOL-USD")->closes, 0);


}

void ServerFeedAdmissionTest::checkTradeWire() {
    auto s = session();
    request(s, "BTC-USD");
    Trade trade{};
    trade.product_id = "BTC-USD";
    trade.timestamp = std::chrono::system_clock::time_point(std::chrono::milliseconds(1000));
    trade.price = 100;
    trade.size = 2;
    trade.side = AggressorSide::Buy;
    const auto before = s->write_queue_.size();
    s->on_trade(trade);
    drain(); // do_write posts to the session's Asio executor
    ASSERT_EQ(s->write_queue_.size(), before + 1);
    const auto message = nlohmann::json::parse(s->write_queue_.back().payload);
    EXPECT_EQ(message.at("type"), "trade");
    EXPECT_EQ(message.at("side"), "buy");
    EXPECT_EQ(message.at("side_basis"), "aggressor");
}

TEST_F(ServerFeedAdmissionTest, TradeWireDeclaresAggressorBasis) { checkTradeWire(); }

void ServerFeedAdmissionTest::checkMetrics() {
    auto s = session();
    request(s, "ETH-USD"); request(s, "DOGE-USD");
    for (int i = 0; i < 3; ++i) { now += 1'000'000; drain(); }
    auto text = metrics.render();
    EXPECT_NE(text.find("sentinel_mdc_connected{product=\"ETH-USD\",pinned=\"0\"} 1\n"), std::string::npos);
    EXPECT_NE(text.find("sentinel_mdc_transport_up_total{product=\"ETH-USD\",pinned=\"0\"} 1\n"), std::string::npos);
    EXPECT_NE(text.find("sentinel_mdc_max_connections 2\n"), std::string::npos);
    EXPECT_NE(text.find("sentinel_mdc_connections{pinned=\"0\"} 2\n"), std::string::npos);
    EXPECT_NE(text.find("sentinel_mdc_connections{pinned=\"1\"} 2\n"), std::string::npos);
    model->onMarketDataConnectionChanged("ETH-USD", false);
    EXPECT_NE(metrics.render().find("sentinel_mdc_transport_down_total{product=\"ETH-USD\",pinned=\"0\"} 1\n"), std::string::npos);
    request(s, "ETH-USD", "unsubscribe");
    model->onMarketDataConnectionChanged("ETH-USD", true); // late retired callback
    text = metrics.render();
    EXPECT_EQ(text.find("product=\"ETH-USD\""), std::string::npos);
    EXPECT_NE(text.find("sentinel_mdc_connected{product=\"BTC-USD\",pinned=\"1\"} 1\n"), std::string::npos);
    EXPECT_NE(text.find("sentinel_mdc_connections{pinned=\"0\"} 1\n"), std::string::npos);
    request(s, "ETH-USD");
    for (int i = 0; i < 10; ++i) request(s, "REFUSED" + std::to_string(i) + "-USD");
    request(s, "REFUSED9-USD");
    text = metrics.render();
    EXPECT_EQ(text.find("product=\"REFUSED0-USD\""), std::string::npos);
    EXPECT_EQ(server->m_refusals.size(), 8u);
    EXPECT_NE(text.find("sentinel_mdc_refused_total{product=\"REFUSED9-USD\"} 2\n"), std::string::npos);
    EXPECT_EQ(feeds->stats().size(), 4u);
}

TEST_F(ServerFeedAdmissionTest, CapRefusalIsAtomicAndReleaseOrCloseFreesOneSlot) { checkCap(); }
TEST_F(ServerFeedAdmissionTest, MetricsFollowActiveProductsAndBoundRefusalHistory) { checkMetrics(); }

TEST_F(ServerFeedAdmissionTest, ClientReceivesStructuredRefusal) {
    const nlohmann::json reply = {{"type", "error"}, {"context", "subscribe"}, {"code", "connection_cap"},
        {"symbol", "XRP-USD"}, {"max_connections", 2}, {"message", "Cannot subscribe to XRP-USD: cap 2 reached"}};
    SentinelStreamClient client("127.0.0.1", "0", config.mdc.sslCaBundle);
    QString symbol, message;
    int cap = 0;
    QObject::connect(&client, &SentinelStreamClient::subscriptionRefused,
        [&](const QString& s, int c, const QString& m) { symbol = s; cap = c; message = m; });
    deliver(client, reply.dump());
    EXPECT_EQ(symbol, "XRP-USD"); EXPECT_EQ(cap, 2); EXPECT_FALSE(message.isEmpty());
    auto invalid = reply;
    invalid["code"] = "invalid_product";
    invalid["symbol"] = "BAD-USD";
    deliver(client, invalid.dump());
    EXPECT_EQ(symbol, "BAD-USD");
    QString acknowledged;
    QObject::connect(&client, &SentinelStreamClient::subscriptionAcknowledged,
        [&](const QString& s) { acknowledged = s; });
    deliver(client, R"({"type":"ack","symbol":"ETH-USD"})");
    EXPECT_EQ(acknowledged, "ETH-USD");
}

void ServerFeedAdmissionTest::checkGuiTenSwitches() {
    // The fixture's cap is two. This exercises the same cap protocol with a
    // tighter limit than production's eight and checks the real Session count.
    auto s = session();
    SymbolSubscriptionManager leases;
    QString current;
    for (int i = 0; i < 10; ++i) {
        const QString next = QString("SYM%1-USD").arg(i);
        const auto subscribe = leases.request("main", next);
        ASSERT_EQ(subscribe.size(), 1);
        ASSERT_EQ(subscribe[0].kind, SymbolSubscriptionManager::Action::Subscribe);
        request(s, next.toStdString());
        ASSERT_TRUE(s->subscriptions_.contains(next.toStdString()));
        EXPECT_LE(server->m_symbolSubscriptions.size(), 2u);
        if (!current.isEmpty()) EXPECT_TRUE(s->subscriptions_.contains(current.toStdString()));

        const auto accepted = leases.acknowledged(next);
        ASSERT_EQ(accepted.size(), i ? 2 : 1);
        EXPECT_EQ(accepted[0].kind, SymbolSubscriptionManager::Action::Activate);
        current = accepted[0].symbol;
        if (i) {
            EXPECT_EQ(accepted[1].kind, SymbolSubscriptionManager::Action::Unsubscribe);
            request(s, accepted[1].symbol.toStdString(), "unsubscribe");
        }
        EXPECT_EQ(server->m_symbolSubscriptions.size(), 1u);
        EXPECT_EQ(feeds->stats().size(), 3u); // two pinned plus one GUI feed
    }
}

void ServerFeedAdmissionTest::checkGuiRefusal() {
    auto gui = session(), other = session();
    SymbolSubscriptionManager leases;
    leases.request("main", "BTC2-USD");
    request(gui, "BTC2-USD");
    leases.acknowledged("BTC2-USD");
    request(other, "OTHER-USD"); // fill the two GUI slots
    const auto pending = leases.request("main", "NEW-USD");
    ASSERT_EQ(pending.size(), 1);
    const auto reply = request(gui, "NEW-USD");
    EXPECT_EQ(reply.value("code", ""), "connection_cap");
    const auto refused = leases.refused("new-usd");
    ASSERT_EQ(refused.size(), 1);
    EXPECT_EQ(refused[0].kind, SymbolSubscriptionManager::Action::Refused);
    EXPECT_EQ(leases.held("main"), "BTC2-USD");
    EXPECT_TRUE(gui->subscriptions_.contains("BTC2-USD"));
    EXPECT_FALSE(gui->subscriptions_.contains("NEW-USD"));
    EXPECT_EQ(server->m_symbolSubscriptions.size(), 2u);
}

TEST_F(ServerFeedAdmissionTest, GuiSwitchesTenSymbolsWithoutLeakingUpstreamSlots) { checkGuiTenSwitches(); }
TEST_F(ServerFeedAdmissionTest, GuiRefusalLeavesOldServerSubscriptionAndChartLease) { checkGuiRefusal(); }

TEST_F(ServerFeedAdmissionTest, UpstreamCapacityFailureReleasesReservedSlotBeforeAck) {
    ASSERT_EQ(feeds->add("FILL-USD"), MarketDataFeeds::AddResult::Added);
    ASSERT_EQ(feeds->add("FILL2-USD"), MarketDataFeeds::AddResult::Added);
    auto gui = session();
    const auto reply = request(gui, "ETH-USD");
    EXPECT_EQ(reply.value("type", ""), "error");
    EXPECT_EQ(reply.value("context", ""), "subscribe");
    EXPECT_EQ(reply.value("code", ""), "connection_cap");
    EXPECT_EQ(reply.value("symbol", ""), "ETH-USD");
    EXPECT_FALSE(sessionHas(gui, "ETH-USD"));
    EXPECT_FALSE(slotHeld("ETH-USD"));
}

TEST_F(ServerFeedAdmissionTest, FastFlipKeepsResubscribedUpstreamFeed) {
    auto gui = session();
    request(gui, "ETH-USD");
    ASSERT_TRUE(transports.contains("ETH-USD"));
    const auto transport = transports.at("ETH-USD");
    gui->handle_message(R"({"type":"unsubscribe","symbol":"ETH-USD"})");
    gui->handle_message(R"({"type":"subscribe","symbol":"BTC-USD"})");
    gui->handle_message(R"({"type":"subscribe","symbol":"ETH-USD"})");
    ASSERT_TRUE(sessionHas(gui, "ETH-USD"));
    drain(); // queued release from the first unsubscribe runs only now
    EXPECT_TRUE(slotHeld("ETH-USD"));
    EXPECT_EQ(transport->closes, 0);
    EXPECT_EQ(feeds->stats().size(), 3u); // two pinned plus ETH
}

TEST_F(ServerFeedAdmissionTest, InvalidUpstreamProductGetsStructuredRefusal) {
    server->setFeedAdmissionHandler([](const std::string&) {
        return SentinelStreamServer::FeedAdmission::InvalidProduct;
    });
    auto gui = session();
    const auto reply = request(gui, "BAD-USD");
    EXPECT_EQ(reply.value("context", ""), "subscribe");
    EXPECT_EQ(reply.value("code", ""), "invalid_product");
    EXPECT_EQ(reply.value("symbol", ""), "BAD-USD");
    EXPECT_FALSE(sessionHas(gui, "BAD-USD"));
    EXPECT_FALSE(slotHeld("BAD-USD"));
}

TEST(ServerFeedAdmissionSource, AppChecksUpstreamResultBeforeAcknowledgement) {
    std::ifstream file(std::string(SENTINEL_SOURCE_DIR) + "/apps/sentinel-server/SentinelServerApp.cpp");
    ASSERT_TRUE(file.good());
    const std::string text((std::istreambuf_iterator<char>(file)), {});
    const auto handler = text.find("setFeedAdmissionHandler(");
    ASSERT_NE(handler, std::string::npos);
    const auto subscribeSignal = text.find("&SentinelStreamServer::clientSubscribed", handler);
    ASSERT_NE(subscribeSignal, std::string::npos);
    const auto admission = text.substr(handler, subscribeSignal - handler);
    EXPECT_NE(admission.find("m_marketDataCore->add(symbol)"), std::string::npos);
    EXPECT_NE(admission.find("AddResult::CapacityExceeded"), std::string::npos);
    EXPECT_NE(admission.find("AddResult::InvalidProduct"), std::string::npos);
    EXPECT_NE(text.find("m_server->releaseIfNoSubscribers(native"), std::string::npos);
}

TEST(ServerFeedConfig, DefaultOverrideAndInvalidCap) {
    EXPECT_EQ(ServerMdcConfig{}.maxConnections, 8);
    QTemporaryDir dir;
    const auto path = dir.filePath("server.yaml");
    for (const auto [value, valid] : {std::pair{3, true}, {0, false}, {-1, false}}) {
        QFile file(path); ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("server:\n  mdc:\n    max_connections: " + QByteArray::number(value) + "\n"); file.close();
        ServerConfig config;
        EXPECT_EQ(ConfigLoader::loadServerConfig(path.toStdString(), &config), valid);
        if (valid) EXPECT_EQ(config.mdc.maxConnections, value);
    }
}

TEST(ServerFeedConfig, RootKeysSurvivePartialMdcOverride) {
    QTemporaryDir dir;
    const auto path = dir.filePath("server.yaml");
    for (const auto& yaml : {
        QByteArray("host: root.example\nport: 1234\nssl_ca_bundle: root-ca.pem\n"
                   "mdc:\n  port: 5678\n  max_connections: 3\n"),
        QByteArray("server:\n  host: root.example\n  port: 1234\n"
                   "  ssl_ca_bundle: root-ca.pem\n  mdc:\n    port: 5678\n"
                   "    max_connections: 3\n")}) {
        QFile file(path); ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(yaml);
        file.close();
        ServerConfig config;
        ASSERT_TRUE(ConfigLoader::loadServerConfig(path.toStdString(), &config));
        EXPECT_EQ(config.mdc.host, "root.example");
        EXPECT_EQ(config.mdc.port, "5678");
        EXPECT_EQ(config.mdc.sslCaBundle, "root-ca.pem");
        EXPECT_EQ(config.mdc.maxConnections, 3);
    }
}

TEST(ServerFeedAlerts, A1bSelectsEachPinnedProductForFiveMinutes) {
    std::ifstream file(std::string(SENTINEL_SOURCE_DIR) + "/ops/monitoring/grafana/provisioning/alerting/rules.yaml");
    const std::string text((std::istreambuf_iterator<char>(file)), {});
    const auto start = text.find("uid: sentinel-a1b");
    ASSERT_NE(start, std::string::npos);
    const auto rule = text.substr(start, text.find("# A3a", start) - start);
    EXPECT_NE(rule.find("min by (product) (sentinel_mdc_connected{pinned=\"1\"})"), std::string::npos);
    EXPECT_NE(rule.find("for: 5m"), std::string::npos);
    EXPECT_NE(rule.find("evaluator: { type: lt, params: [1] }"), std::string::npos);
}

// Full Session -> feed lifecycle -> model -> both stores, with a deterministic
// recording clock and temp roots. Never connects to a provider or live server.
void ServerFeedAdmissionTest::checkRecorderRelease() {
    using namespace recording;
    const auto epoch = kHmc2MinMs;
    model->m_heatmapStreamer->stop();
    ServerHeatmapConfig legacy = config.heatmap;
    legacy.persistenceEnabled = true;
    legacy.persistenceDir = dir.filePath("legacy").toStdString();
    legacy.timeframesMs = {60000};
    model->m_heatmapStreamer = std::make_unique<HeatmapTwapStreamer>(*model, legacy);
    RecorderConfig rc;
    rc.root = dir.filePath("recording").toStdString();
    rc.layers = {{"near", 100, .5, 2, false}};
    rc.latenessMs = 2000;
    LiveCache cache;
    size_t publications = 0;
    rc.publisher = [&](RecordPtr r) { ++publications; EXPECT_TRUE(cache.publish(std::move(r))); };
    rc.onReleased = [&](const std::string& symbol) { cache.releaseSymbol(symbol); };
    model->m_recorder = std::make_unique<BookRecorder>(rc, [&] { return recordingLocal; });
    auto cleanup = qScopeGuard([&] { model->m_recorder.reset(); });
    const auto snapshot = [&](const QString& symbol) {
        model->onLiveOrderBookInitialized(symbol, {{99, 2}}, {{101, 4}}, recordingLocal);
        model->m_recorder->drainForTest();
    };
    auto s = session();
    request(s, "ETH-USD");
    recordingLocal = epoch + 10000;
    snapshot("ETH-USD"); snapshot("BTC-USD");
    model->m_recorder->onTick(epoch + 20000);
    model->m_recorder->drainForTest();
    ASSERT_TRUE(model->ensureSymbol("ETH-USD").bookValid);
    ASSERT_GT(cache.snapshot("ETH-USD", "near").revision, 0u);
    auto oldView = std::make_shared<recording::LiveService::Subscription>(
        recording::LiveView{"ETH-USD", "near", 60000, {90, 1, 20}, 1}, nullptr);
    auto oldRaw = std::make_shared<recording::LiveService::RawSubscription>(
        recording::RawTailView{"ETH-USD", {"hmc2.near"}, 1, 0}, nullptr);
    s->recordingView_ = oldView;
    s->rawViews_["ETH-USD"] = oldRaw;
    recordingLocal = epoch + 35000;
    request(s, "ETH-USD", "unsubscribe");
    EXPECT_FALSE(oldView->active.load());
    EXPECT_FALSE(oldRaw->active.load());
    EXPECT_FALSE(s->recordingView_);
    EXPECT_FALSE(s->rawViews_.contains("ETH-USD"));
    model->m_recorder->drainForTest();
    EXPECT_FALSE(model->ensureSymbol("ETH-USD").bookValid);
    EXPECT_TRUE(model->ensureSymbol("ETH-USD").liveBook.isEmpty());
    EXPECT_EQ(model->ensureSymbol("ETH-USD").liveBook.getTickSize(), 0);
    EXPECT_TRUE(model->ensureSymbol("BTC-USD").bookValid);
    EXPECT_EQ(cache.snapshot("ETH-USD", "near").revision, 0u);
    EXPECT_EQ(metrics.render().find("product=\"ETH-USD\""), std::string::npos);
    auto rows = Hmc2Store::readRange(rc.root, "ETH-USD", "near", 60000, epoch, epoch + 600000);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].observedMs, 25000u);
    EXPECT_TRUE(rows[0].flags & kPartial);
    EXPECT_FALSE(rows[0].flags & kProvisional);
    // A late callback cannot recreate a released product.
    snapshot("ETH-USD");
    EXPECT_FALSE(model->ensureSymbol("ETH-USD").bookValid);
    recordingLocal = epoch + 182000;
    model->m_recorder->onTick(recordingLocal);
    model->m_recorder->drainForTest();
    rows = Hmc2Store::readRange(rc.root, "ETH-USD", "near", 60000, epoch, epoch + 600000);
    EXPECT_EQ(rows.size(), 1u);
    EXPECT_EQ(model->recordingWatermarks("ETH-USD", "near").minuteThroughMs, 0);
    EXPECT_GT(model->recordingWatermarks("BTC-USD", "near").lastColumnMs, epoch);
    // The model guard is independent of app-level pinned filtering.
    model->releaseGuiFeed("BTC-USD", recordingLocal);
    EXPECT_TRUE(model->ensureSymbol("BTC-USD").bookValid);
    request(s, "ETH-USD");
    recordingLocal = epoch + 190000;
    snapshot("ETH-USD");
    recordingLocal = epoch + 205000;
    request(s, "ETH-USD", "unsubscribe");
    model->m_recorder->drainForTest();
    rows = Hmc2Store::readRange(rc.root, "ETH-USD", "near", 60000, epoch, epoch + 600000);
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows.back().bucketStartMs, epoch + 180000);
    EXPECT_EQ(rows.back().observedMs, 15000u);
    EXPECT_TRUE(rows.back().flags & kResynced);
    // Destroy while callbacks still have valid captures.
    model->m_recorder.reset();
}
TEST_F(ServerFeedAdmissionTest, ReleasedGuiFeedFinalizesPartialAndReacquiresFresh) { checkRecorderRelease(); }

void ServerFeedAdmissionTest::checkLegacyRelease() {
    model->m_heatmapStreamer->stop();
    auto legacy = config.heatmap;
    legacy.persistenceEnabled = true;
    legacy.persistenceDir = dir.filePath("legacy").toStdString();
    legacy.timeframesMs = {60000};
    legacy.activeTimeframeMs = 60000;
    model->m_heatmapStreamer = std::make_unique<HeatmapTwapStreamer>(*model, legacy);
    auto& streamer = *model->m_heatmapStreamer;
    auto s = session(); request(s, "ETH-USD");
    model->onLiveOrderBookInitialized("ETH-USD", {{99, 2}}, {{101, 4}}, recordingLocal);
    const auto epoch = recording::kHmc2MinMs;
    for (int64_t t = 0; t <= 90000; t += 1000) {
        recordingLocal = epoch + t;
        streamer.onSample();
    }
    std::vector<HeatmapColumnStore::LoadedColumn> before, after;
    ASSERT_TRUE(streamer.m_columnStore->loadRecent("ETH-USD", 60000, 100, before));
    ASSERT_FALSE(before.empty());
    recordingLocal = epoch + 95000;
    request(s, "ETH-USD", "unsubscribe");
    EXPECT_FALSE(streamer.m_symbols.contains("ETH-USD"));
    for (int64_t t = 96000; t <= 300000; t += 1000) {
        recordingLocal = epoch + t;
        streamer.onSample();
    }
    EXPECT_FALSE(streamer.m_symbols.contains("ETH-USD"));
    ASSERT_TRUE(streamer.m_columnStore->loadRecent("ETH-USD", 60000, 100, after));
    ASSERT_EQ(after.size(), before.size());
    EXPECT_EQ(after.back().bucketStartMs, before.back().bucketStartMs);
    EXPECT_EQ(after.back().intensity, before.back().intensity);
}
TEST_F(ServerFeedAdmissionTest, ReleasedGuiFeedStopsLegacyColumnStore) { checkLegacyRelease(); }
