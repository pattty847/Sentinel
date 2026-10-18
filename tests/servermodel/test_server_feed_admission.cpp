// Actual Session admission/close/write paths, captured products, no sockets/services.
#include "protocol/SentinelStreamServer.cpp"
#include "protocol/SentinelStreamClient.hpp"
#include "marketdata/auth/Authenticator.hpp"
#include "ConfigLoader.hpp"
#include "mainwindow/SymbolSubscriptionManager.hpp"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
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
    std::unique_ptr<FeedClockModel> model;
    std::unique_ptr<SentinelStreamServer> server;
    sentinel::metrics::MetricsRegistry metrics;
    net::io_context io;
    ssl::context tls{ssl::context::tls_server};
    std::vector<std::shared_ptr<Session>> sessions;
    int64_t now = 1'000'000;
    int64_t recordingLocal = recording::kHmc2MinMs;

    void SetUp() override {
        config.recording.enabled = false;
        config.rollerShadow.products = {"BTC-USD", "SOL-USD", "ETH-USD"};
        config.mdc.sslCaBundle = std::string(SENTINEL_SOURCE_DIR) + "/resources/certs/ca-bundle.crt";
        model = std::make_unique<FeedClockModel>(config, recordingLocal);
        server = std::make_unique<SentinelStreamServer>(*model, auth, config, 0);
        model->registerMetrics(metrics);
        server->setFeedAdmissionHandler([this](const std::string& symbol) {
            const auto products = rollerProducts(config);
            return std::find(products.begin(), products.end(), symbol) != products.end()
                ? SentinelStreamServer::FeedAdmission::Accepted : SentinelStreamServer::FeedAdmission::InvalidProduct;
        });
        drain();
    }
    void drain() {
        for (int i = 0; i < 4; ++i) {
            QCoreApplication::sendPostedEvents();
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
    void seedEthMetadata() {
        model->onFeedMetadata("ETH-USD", nlohmann::json{{"product_id", "ETH-USD"},
            {"quote_increment", "0.01"}, {"base_increment", "0.00000001"}});
    }
    void TearDown() override {
        for (auto& s : sessions) s->beginClose("fixture cleanup");
        drain();
        sessions.clear();
        QCoreApplication::sendPostedEvents();
    }
    void checkGuiRefusal();
    void checkMetadataGate();
    void checkTradeWire();
    void checkRefusal();
    bool sessionHas(const std::shared_ptr<Session>& s, const std::string& symbol) const {
        return s->subscriptions_.contains(symbol);
    }
    static void deliver(SentinelStreamClient& client, const std::string& message) { client.handleMessage(message); }
};

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

TEST_F(ServerFeedAdmissionTest, ClientReceivesStructuredRefusal) {
    const nlohmann::json reply = {{"type", "error"}, {"context", "subscribe"}, {"code", "invalid_product"},
        {"symbol", "XRP-USD"}, {"message", "Cannot subscribe to XRP-USD: invalid product"}};
    SentinelStreamClient client("127.0.0.1", "0", config.mdc.sslCaBundle);
    QString symbol, message;
    int cap = 0;
    QObject::connect(&client, &SentinelStreamClient::subscriptionRefused,
        [&](const QString& s, int c, const QString& m) { symbol = s; cap = c; message = m; });
    deliver(client, reply.dump());
    EXPECT_EQ(symbol, "XRP-USD"); EXPECT_EQ(cap, 0); EXPECT_FALSE(message.isEmpty());
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

void ServerFeedAdmissionTest::checkGuiRefusal() {
    auto gui = session();
    SymbolSubscriptionManager leases;
    leases.request("main", "BTC-USD");
    request(gui, "BTC-USD");
    leases.acknowledged("BTC-USD");
    const auto pending = leases.request("main", "NEW-USD");
    ASSERT_EQ(pending.size(), 1);
    const auto reply = request(gui, "NEW-USD");
    EXPECT_EQ(reply.value("code", ""), "invalid_product");
    const auto refused = leases.refused("new-usd");
    ASSERT_EQ(refused.size(), 1);
    EXPECT_EQ(refused[0].kind, SymbolSubscriptionManager::Action::Refused);
    EXPECT_EQ(leases.held("main"), "BTC-USD");
    EXPECT_TRUE(gui->subscriptions_.contains("BTC-USD"));
    EXPECT_FALSE(gui->subscriptions_.contains("NEW-USD"));
}

TEST_F(ServerFeedAdmissionTest, GuiRefusalLeavesOldServerSubscriptionAndChartLease) { checkGuiRefusal(); }

TEST(ServerFeedConfig, BindAddressDefaultsFlatWrappedAndOverride) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto path = dir.filePath("server.yaml");
    const auto load = [&](const QByteArray& yaml, ServerConfig& config) {
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
        if (file.write(yaml) != yaml.size()) return false;
        file.close();
        return ConfigLoader::loadServerConfig(path.toStdString(), &config);
    };
    ServerConfig config;
    ASSERT_TRUE(load("schema_version: 1\n", config));
    EXPECT_EQ(config.bindAddress, "127.0.0.1");
    ASSERT_TRUE(load("bind_address: '100.89.99.21'\n", config));
    EXPECT_EQ(config.bindAddress, "100.89.99.21");
    ASSERT_TRUE(load("server:\n  bind_address: '::1'\n", config));
    EXPECT_EQ(config.bindAddress, "::1");
    ASSERT_TRUE(load("server:\n  stream_port: 12345\n", config));
    EXPECT_EQ(config.bindAddress, "::1"); // partial overrides retain the selected address
    // Parsing the address belongs to listener startup, which must propagate failure.
    ASSERT_TRUE(load("server:\n  bind_address: 'invalid-address'\n", config));
    EXPECT_EQ(config.bindAddress, "invalid-address");
}

namespace {
QStringList* g_configWarnings = nullptr;
}

TEST(ServerFeedConfig, HistoryBarsParseClampAndIgnoreUnconfigured) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto path = dir.filePath("server.yaml");
    QStringList warnings;
    g_configWarnings = &warnings;
    const auto previous = qInstallMessageHandler([](QtMsgType type, const QMessageLogContext&, const QString& text) {
        if (type == QtWarningMsg && g_configWarnings) g_configWarnings->push_back(text);
    });
    const auto restore = qScopeGuard([&] { qInstallMessageHandler(previous); g_configWarnings = nullptr; });
    const auto load = [&](const QByteArray& bars) {
        warnings.clear();
        ServerConfig config;
        QFile file(path);
        EXPECT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write("server:\n  chart:\n    timeframes_ms: [100, 1000, 60000]\n" + bars);
        file.close();
        EXPECT_TRUE(ConfigLoader::loadServerConfig(path.toStdString(), &config));
        return config.chart.historyBars;
    };
    const auto warned = [&](const QString& text) {
        return std::any_of(warnings.begin(), warnings.end(), [&](const QString& w) { return w.contains(text); });
    };
    EXPECT_EQ(load("    history_bars: {100: 18000}\n"), (std::map<int64_t, int>{{100, 18'000}}));
    EXPECT_FALSE(warned("history_bars"));
    for (const QByteArray low : {"10", "0", "-5"}) {
        EXPECT_EQ(load("    history_bars: {100: " + low + "}\n"), (std::map<int64_t, int>{{100, 350}})) << low.toStdString();
        EXPECT_TRUE(warned("Clamped chart.history_bars.100=" + QString::fromLatin1(low) + " to 350")) << low.toStdString();
    }
    EXPECT_EQ(load("    history_bars: {100: 500000}\n"), (std::map<int64_t, int>{{100, 200'000}}));
    EXPECT_TRUE(warned("Clamped chart.history_bars.100=500000 to 200000"));
    EXPECT_TRUE(load("    history_bars: {250: 5000}\n").empty());
    EXPECT_TRUE(warned("Ignored chart.history_bars.250: not in chart.timeframes_ms"));
    EXPECT_TRUE(load("").empty()); // absent key: no entry, so the default trim rule applies
    EXPECT_FALSE(warned("history_bars"));
}

TEST(ServerFeedConfig, CheckedInConfigDefaultsToLoopback) {
    ServerConfig config;
    // Distinguish loading the file's value from retaining the struct default.
    config.bindAddress = "0.0.0.0";
    ASSERT_TRUE(ConfigLoader::loadServerConfig(std::string(SENTINEL_SOURCE_DIR) +
                                               "/config/server_config.yaml", &config));
    EXPECT_EQ(config.bindAddress, "127.0.0.1");
}

TEST(ServerFeedConfig, OnlyRestCaBundleIsReadFromMdc) {
    QTemporaryDir dir;
    const auto path = dir.filePath("server.yaml");
    for (const bool wrapped : {false, true}) {
        for (const bool nested : {false, true}) {
            const QByteArray indent = wrapped ? "  " : "";
            QByteArray yaml = wrapped ? "server:\n" : "";
            yaml += indent + "recording:\n" + indent + "  source: roller\n";
            yaml += indent + "roller_shadow:\n" + indent + "  enabled: true\n";
            yaml += indent + "host: ignored.example\n" + indent + "port: ignored\n";
            yaml += indent + "ssl_ca_bundle: root-ca.pem\n";
            yaml += indent + "mdc:\n";
            for (const auto* value : {"host: ignored", "port: ignored", "target: ignored", "use_jwt: invalid",
                                      "connect_timeout_ms: invalid", "close_timeout_ms: invalid", "max_connections: invalid"})
                yaml += indent + "  " + value + "\n";
            if (nested) yaml += indent + "  ssl_ca_bundle: nested-ca.pem\n";
            QFile file(path); ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            file.write(yaml); file.close();
            ServerConfig config;
            ASSERT_TRUE(ConfigLoader::loadServerConfig(path.toStdString(), &config));
            EXPECT_EQ(recordingSourceProblem(config), "");
            EXPECT_EQ(config.mdc.sslCaBundle, nested ? "nested-ca.pem" : "root-ca.pem");
            EXPECT_EQ(config.mdc.host, ServerMdcConfig{}.host);
            EXPECT_EQ(config.mdc.port, ServerMdcConfig{}.port);
            EXPECT_EQ(config.mdc.target, ServerMdcConfig{}.target);
        }
    }
}

TEST(ServerFeedAlerts, A1bSelectsEachCapturedProductForFiveMinutes) {
    std::ifstream file(std::string(SENTINEL_SOURCE_DIR) + "/ops/monitoring/grafana/provisioning/alerting/rules.yaml");
    const std::string text((std::istreambuf_iterator<char>(file)), {});
    const auto start = text.find("uid: sentinel-a1b");
    ASSERT_NE(start, std::string::npos);
    const auto rule = text.substr(start, text.find("# A3a", start) - start);
    EXPECT_NE(rule.find("min by (product) (sentinel_live_feed_up)"), std::string::npos);
    EXPECT_NE(rule.find("for: 5m"), std::string::npos);
    EXPECT_NE(rule.find("evaluator: { type: lt, params: [1] }"), std::string::npos);
}

void ServerFeedAdmissionTest::checkMetadataGate() {
    using namespace recording;
    const auto epoch = kHmc2MinMs;
    auto s = session();
    request(s, "ETH-USD"); // No product metadata response yet.
    recordingLocal = epoch + 10000;
    model->onLiveOrderBookInitialized("ETH-USD", {{99, 2}}, {{101, 4}}, recordingLocal);
    EXPECT_FALSE(model->ensureSymbol("ETH-USD").bookValid);
    recordingLocal = epoch + 15000;
    model->onLiveOrderBookLevelUpdates("ETH-USD", {{true, 99, 5}}, recordingLocal);
    recordingLocal = epoch + 70000;
    seedEthMetadata();
    EXPECT_TRUE(model->ensureSymbol("ETH-USD").bookValid);
    EXPECT_DOUBLE_EQ(model->ensureSymbol("ETH-USD").liveBook.getBidVolume(), 5.0);
}
TEST_F(ServerFeedAdmissionTest, MetadataGateRetainsNativeSnapshotAndDeltas) { checkMetadataGate(); }

void ServerFeedAdmissionTest::checkRefusal() {
    auto s = session();
    request(s, "btc-usd");
    ASSERT_TRUE(sessionHas(s, "BTC-USD"));
    const auto before = s->write_queue_.size();
    const auto reply = request(s, "XRP-USD");
    EXPECT_EQ(s->write_queue_.size(), before + 1); // error only: no ack or snapshot
    EXPECT_EQ(reply.at("type"), "error");
    EXPECT_EQ(reply.at("context"), "subscribe");
    EXPECT_EQ(reply.at("code"), "invalid_product");
    EXPECT_EQ(reply.at("symbol"), "XRP-USD");
    EXPECT_FALSE(reply.contains("max_connections"));
    EXPECT_NE(reply.at("message").get<std::string>().find("XRP-USD"), std::string::npos);
    EXPECT_FALSE(sessionHas(s, "XRP-USD"));
    EXPECT_FALSE(s->availability_.contains("XRP-USD"));
    EXPECT_TRUE(sessionHas(s, "BTC-USD"));
    request(s, "BTC-USD", "unsubscribe");
    request(s, "BTC-USD");
    EXPECT_TRUE(sessionHas(s, "BTC-USD"));
}
TEST_F(ServerFeedAdmissionTest, UncapturedProductRefusalIsAtomic) { checkRefusal(); }

TEST(ServerFeedAdmissionSource, ServerHasOnlyTheJournalFeed) {
    const auto read = [](const char* path) {
        std::ifstream file(std::string(SENTINEL_SOURCE_DIR) + path);
        EXPECT_TRUE(file.good());
        return std::string((std::istreambuf_iterator<char>(file)), {});
    };
    const auto app = read("/apps/sentinel-server/SentinelServerApp.cpp") +
                     read("/apps/sentinel-server/SentinelServerApp.hpp");
    for (const auto* removed : {"MarketDataFeeds", "m_marketDataCore", "broadcastCoinbaseLatency", "requestBookProductMetadata"})
        EXPECT_EQ(app.find(removed), std::string::npos) << removed;
    EXPECT_NE(app.find("rollerProducts(m_serverConfig)"), std::string::npos);
    const auto model = read("/libs/core/servermodel/ServerDataModel.cpp") +
                       read("/libs/core/servermodel/ServerDataModel.hpp");
    EXPECT_EQ(model.find("logTrade"), std::string::npos);
    EXPECT_EQ(model.find("productMetadataRequested"), std::string::npos);
    EXPECT_EQ(read("/libs/core/protocol/SentinelStreamServer.cpp").find("fetchProductMetadata"), std::string::npos);
}

TEST_F(ServerFeedAdmissionTest, TradesNeverCreateLegacyTickLogs) {
    const auto old = QDir::currentPath();
    const auto restore = qScopeGuard([&] { QDir::setCurrent(old); });
    ASSERT_TRUE(QDir::setCurrent(dir.path()));
    int delivered = 0;
    QObject::connect(model.get(), &ServerDataModel::tradeBroadcast, [&](const Trade&) { ++delivered; });
    Trade trade{};
    trade.product_id = "BTC-USD";
    trade.price = 100;
    trade.size = 1;
    for (int i = 0; i < 100; ++i) {
        trade.timestamp = std::chrono::system_clock::time_point(std::chrono::milliseconds(1000 + i));
        model->onTrade(trade);
    }
    EXPECT_EQ(delivered, 100);
    EXPECT_FALSE(QDir(dir.filePath("data/market")).exists());
}
