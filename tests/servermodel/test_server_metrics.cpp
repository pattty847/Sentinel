// sentinel-server's /metrics wiring without the live data: ServerDataModel records
// into a QTemporaryDir (never the configured recording root), and the route is
// served by the same MetricsHttpServer the app uses, on an ephemeral port.
#include "metrics/MetricsHttpServer.hpp"
#include "metrics/MetricsRegistry.hpp"
#include "servermodel/ServerDataModel.hpp"
#include "capture/CaptureFanout.hpp"
#include "roller/ShadowRoller.hpp"
#include <chrono>
#include <thread>
#include <stdexcept>
#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <gtest/gtest.h>

using sentinel::metrics::MetricsHttpServer;
using sentinel::metrics::MetricsRegistry;

namespace {
std::string scrape(uint16_t port) {
    QTcpSocket client;
    client.connectToHost(QHostAddress::LocalHost, port);
    QByteArray response;
    bool sent = false;
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 3000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (!sent && client.state() == QAbstractSocket::ConnectedState) {
            client.write("GET /metrics HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
            sent = true;
        }
        client.waitForReadyRead(5);
        response += client.readAll();
        if (sent && client.state() == QAbstractSocket::UnconnectedState) break;
    }
    EXPECT_TRUE(response.startsWith("HTTP/1.1 200 OK\r\n")) << response.toStdString();
    return response.mid(response.indexOf("\r\n\r\n") + 4).toStdString();
}

bool has(const std::string& text, const std::string& line) { return text.find("\n" + line + "\n") != std::string::npos; }
bool hasSeries(const std::string& text, const std::string& prefix) {
    return text.find("\n" + prefix) != std::string::npos;
}

ServerConfig recordingConfig(const QTemporaryDir& dir) {
    ServerConfig config;
    config.recording.enabled = true;
    config.recording.dir = dir.path().toStdString();
    config.recording.fallbackDir.clear();
    config.heatmap.persistenceEnabled = false;
    config.defaultSymbols = {"btc-usd"};
    return config;
}
} // namespace

TEST(ServerMetrics, RecorderAndConnectionSeriesOverHttp) {
    int argc = 1;
    char name[] = "server-metrics";
    char* argv[] = {name, nullptr};
    QCoreApplication app(argc, argv);
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ServerDataModel model(recordingConfig(dir));
    ASSERT_TRUE(model.recordingAvailable());
    ASSERT_EQ(model.recordingDir()->string(), dir.path().toStdString());
    MetricsRegistry registry;
    model.registerMetrics(registry);
    MetricsHttpServer server(registry);
    ASSERT_TRUE(server.listen(0));

    std::string text = scrape(server.port());
    EXPECT_TRUE(has(text, "sentinel_recorder_running 1"));
    EXPECT_TRUE(has(text, "sentinel_mdc_connected{product=\"BTC-USD\",pinned=\"1\"} 0"));
    EXPECT_TRUE(has(text, "sentinel_mdc_transport_up_total{product=\"BTC-USD\",pinned=\"1\"} 0"));
    EXPECT_TRUE(has(text, "sentinel_recorder_columns_written_total 0"));
    EXPECT_TRUE(has(text, "sentinel_recorder_disk_errors_total 0"));
    EXPECT_TRUE(has(text, "sentinel_recorder_live_publish_drops_total 0"));
    EXPECT_TRUE(has(text, "# TYPE sentinel_recorder_queue_drops_total counter"));
    // No column yet and disconnected: both per-series gauges stay absent, so an
    // alert on them cannot fire on a series that has nothing to measure.
    EXPECT_FALSE(hasSeries(text, "sentinel_recorder_last_column_timestamp_seconds{"));
    EXPECT_FALSE(hasSeries(text, "sentinel_recorder_column_overdue_seconds{"));

    model.onMarketDataConnectionChanged("BTC-USD", true);
    model.onMarketDataConnectionChanged("BTC-USD", true); // repeated status is not a transition
    text = scrape(server.port());
    EXPECT_TRUE(has(text, "sentinel_mdc_connected{product=\"BTC-USD\",pinned=\"1\"} 1"));
    EXPECT_TRUE(has(text, "sentinel_mdc_transport_up_total{product=\"BTC-USD\",pinned=\"1\"} 1"));
    EXPECT_TRUE(has(text, "sentinel_mdc_transport_down_total{product=\"BTC-USD\",pinned=\"1\"} 0"));
    // Just connected: every pinned symbol x layer is on time.
    EXPECT_TRUE(has(text, "sentinel_recorder_column_overdue_seconds{product=\"BTC-USD\",layer=\"near\"} 0"));
    EXPECT_TRUE(has(text, "sentinel_recorder_column_overdue_seconds{product=\"BTC-USD\",layer=\"deep\"} 0"));

    model.onMarketDataConnectionChanged("BTC-USD", false);
    text = scrape(server.port());
    EXPECT_TRUE(has(text, "sentinel_mdc_connected{product=\"BTC-USD\",pinned=\"1\"} 0"));
    EXPECT_TRUE(has(text, "sentinel_mdc_transport_down_total{product=\"BTC-USD\",pinned=\"1\"} 1"));
    EXPECT_FALSE(hasSeries(text, "sentinel_recorder_column_overdue_seconds{"));

    // An upstream invalidation reaches the recorder's counter (worker thread).
    model.onLiveOrderBookInitialized("BTC-USD", {{100.0, 2.0}}, {{101.0, 1.0}}, QDateTime::currentMSecsSinceEpoch());
    model.onLiveOrderBookInvalidated("BTC-USD", "disconnected");
    QElapsedTimer timer;
    timer.start();
    while (!has(text, "sentinel_recorder_invalidations_total 1") && timer.elapsed() < 3000)
        text = scrape(server.port());
    EXPECT_TRUE(has(text, "sentinel_recorder_invalidations_total 1")) << text;
}

// Per-product connections: only pinned (recorder) symbols drive the health series.
// A GUI chart's product going down must not page A1b or hide BTC's overdue
// deadline, and a pinned product down must show even while a GUI product is up.
TEST(ServerMetrics, GuiProductConnectionCannotMoveThePinnedHealthSeries) {
    int argc = 1;
    char name[] = "server-metrics-gui";
    char* argv[] = {name, nullptr};
    QCoreApplication app(argc, argv);
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ServerConfig config = recordingConfig(dir);
    config.defaultSymbols = {"btc-usd", "sol-usd"};
    ServerDataModel model(config);
    ASSERT_TRUE(model.recordingAvailable());
    MetricsRegistry registry;
    model.registerMetrics(registry);
    const std::string btcOverdue = "sentinel_recorder_column_overdue_seconds{product=\"BTC-USD\",layer=\"near\"}";
    const std::string ethOverdue = "sentinel_recorder_column_overdue_seconds{product=\"ETH-USD\"";

    model.onMarketDataConnectionChanged("BTC-USD", true);
    std::string text = registry.render();
    EXPECT_TRUE(has(text, "sentinel_mdc_connected{product=\"SOL-USD\",pinned=\"1\"} 0")) << "SOL-USD (pinned) is still down";
    model.onMarketDataConnectionChanged("SOL-USD", true);
    text = registry.render();
    EXPECT_TRUE(has(text, "sentinel_mdc_connected{product=\"BTC-USD\",pinned=\"1\"} 1"));
    EXPECT_TRUE(has(text, "sentinel_mdc_transport_up_total{product=\"BTC-USD\",pinned=\"1\"} 1"));
    EXPECT_TRUE(has(text, btcOverdue + " 0"));

    model.acquireGuiFeed("ETH-USD");
    // A GUI chart opens and closes ETH-USD: nothing pinned moves.
    for (const bool up : {true, false, true, false}) {
        model.onMarketDataConnectionChanged("ETH-USD", up);
        text = registry.render();
        EXPECT_TRUE(has(text, "sentinel_mdc_connected{product=\"BTC-USD\",pinned=\"1\"} 1")) << "ETH up=" << up;
        EXPECT_TRUE(has(text, "sentinel_mdc_transport_up_total{product=\"BTC-USD\",pinned=\"1\"} 1"));
        EXPECT_TRUE(has(text, "sentinel_mdc_transport_down_total{product=\"BTC-USD\",pinned=\"1\"} 0"));
        EXPECT_TRUE(has(text, btcOverdue + " 0")) << "ETH up=" << up;
        EXPECT_FALSE(hasSeries(text, ethOverdue)) << "only pinned symbols have recorder series";
    }

    model.releaseGuiFeed("ETH-USD");
    model.onMarketDataConnectionChanged("ETH-USD", true); // late callback cannot recreate it
    text = registry.render();
    EXPECT_FALSE(hasSeries(text, "sentinel_mdc_connected{product=\"ETH-USD\""));
    model.acquireGuiFeed("ETH-USD");
    // BTC alone down while the GUI's ETH is up: the gauge drops, BTC's overdue goes absent.
    model.onMarketDataConnectionChanged("ETH-USD", true);
    model.onMarketDataConnectionChanged("BTC-USD", false);
    text = registry.render();
    EXPECT_TRUE(has(text, "sentinel_mdc_connected{product=\"BTC-USD\",pinned=\"1\"} 0"));
    EXPECT_TRUE(has(text, "sentinel_mdc_transport_down_total{product=\"BTC-USD\",pinned=\"1\"} 1"));
    EXPECT_FALSE(hasSeries(text, btcOverdue));
    EXPECT_TRUE(hasSeries(text, "sentinel_recorder_column_overdue_seconds{product=\"SOL-USD\",layer=\"near\"}"));
}

TEST(ServerMetrics, RecorderOffExportsOnlyItsState) {
    int argc = 1;
    char name[] = "server-metrics-off";
    char* argv[] = {name, nullptr};
    QCoreApplication app(argc, argv);
    QTemporaryDir dir;
    ServerConfig config = recordingConfig(dir);
    config.recording.enabled = false;
    ServerDataModel model(config);
    MetricsRegistry registry;
    model.registerMetrics(registry);
    const std::string text = registry.render();
    EXPECT_TRUE(has(text, "sentinel_recorder_running 0"));
    EXPECT_EQ(text.find("sentinel_recorder_columns_written_total"), std::string::npos);
}

namespace {
// Real RAWL2 -> durable fanout -> ShadowRoller -> HMC2, all under /tmp.
// No provider, owner socket, settings or recording root is used.
struct RollerMetricsFixture {
    QTemporaryDir dir{"/tmp/server-metrics-XXXXXX"};
    MetricsRegistry registry, fanoutRegistry;
    ServerConfig config;
    std::unique_ptr<ServerDataModel> model;
    std::unique_ptr<sentinel::capture::CaptureFanout> fanout;
    std::vector<std::unique_ptr<sentinel::capture::Writer>> writers;
    std::unique_ptr<sentinel::roller::ShadowRoller> roller;
    const int64_t epoch = sentinel::roller::parseTime("2026-10-01T00:00:00Z");

    ~RollerMetricsFixture() {
        roller.reset(); // join before clearing the process-wide failure hook
        recording::Hmc2Store::setDirectorySyncHookForTest({});
    }
    void prepare() {
        ASSERT_TRUE(dir.isValid());
        config = recordingConfig(dir);
        config.recording.dir = (dir.path() + "/primary").toStdString();
        config.recording.source = "roller";
        config.defaultSymbols = {"BTC-USD", "SOL-USD"};
        auto& c = config.rollerShadow;
        c.enabled = true;
        c.products = config.defaultSymbols;
        c.journalRoot = (dir.path() + "/raw").toStdString();
        c.outputRoot = (dir.path() + "/rolled").toStdString();
        c.socketPath = (dir.path() + "/capture.sock").toStdString();
        c.from = "2026-10-01T00:00:00Z";
        c.retryMin = std::chrono::milliseconds(20);
        c.retryMax = std::chrono::milliseconds(100);
        model = std::make_unique<ServerDataModel>(config);
        ASSERT_TRUE(model->servesRoller());
        model->registerMetrics(registry); // same registration order as the app
        sentinel::capture::FanoutConfig f;
        f.socketPath = QString::fromStdString(c.socketPath);
        fanout = std::make_unique<sentinel::capture::CaptureFanout>(
            f, c.products, fanoutRegistry, [](const auto&) {});
        for (size_t i = 0; i < c.products.size(); ++i) {
            sentinel::capture::WriterConfig w;
            w.root = QString::fromStdString(c.journalRoot);
            w.fsyncBlocks = 1;
            w.onJournal = [this, i](const auto& event) { fanout->publish(i, event); };
            writers.push_back(std::make_unique<sentinel::capture::Writer>(w,
                nlohmann::json{{"product_metadata", {{"product_id", c.products[i]},
                    {"quote_increment", "0.01"}, {"base_increment", "0.00000001"}}}}));
            const auto snapshot = nlohmann::json{
                {"channel", "l2_data"}, {"events", nlohmann::json::array({
                    {{"type", "snapshot"}, {"product_id", c.products[i]},
                     {"updates", nlohmann::json::array({
                         {{"side", "bid"}, {"price_level", "99.99"}, {"new_quantity", "2"}},
                         {{"side", "offer"}, {"price_level", "100.01"}, {"new_quantity", "3"}}})}}
                })}}.dump();
            append(i, 0, sentinel::capture::Kind::Frame, snapshot);
        }
    }
    void append(size_t product, int64_t ms, sentinel::capture::Kind kind,
                const std::string& payload = "{\"channel\":\"heartbeats\"}") {
        writers[product]->append({kind, {(epoch + ms) * 1000000, (ms + 1) * 1000000}, 1, payload});
        writers[product]->flush();
    }
    void start() {
        auto c = config.rollerShadow;
        c.publisher = model->rollerPublisher();
        c.retractLive = model->rollerRetract();
        c.ensureLiveFinal = model->rollerEnsureFinal();
        c.liveNowForTest = [this] { return epoch; }; // lead never advances history
        roller = std::make_unique<sentinel::roller::ShadowRoller>(
            c, c.products, config.recording.dir, registry);
        model->attachRoller(
            [this](const auto& product, const auto& layer) { return roller->watermarks(product, layer); },
            [this](const auto& product) { return roller->running(product); });
    }
    bool waitFor(const std::string& line) {
        QElapsedTimer timer;
        timer.start();
        do {
            if (has(registry.render(), line)) return true;
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        } while (timer.elapsed() < 5000);
        return has(registry.render(), line);
    }
};
} // namespace

TEST(ServerMetrics, RollerColumnsAndInvalidationsOverHttp) {
    int argc = 1;
    char name[] = "roller-metrics";
    char* argv[] = {name, nullptr};
    QCoreApplication app(argc, argv);
    RollerMetricsFixture f;
    f.prepare();
    ASSERT_FALSE(HasFatalFailure());
    // The server exports throughput even before the roller attaches.
    EXPECT_TRUE(has(f.registry.render(), "sentinel_recorder_columns_written_total 0"));
    f.start();
    MetricsHttpServer server(f.registry);
    ASSERT_TRUE(server.listen(0));
    auto text = scrape(server.port());
    for (const auto* product : {"BTC-USD", "SOL-USD"}) {
        EXPECT_TRUE(has(text, std::string("sentinel_roller_write_errors_total{product=\"") + product + "\"} 0"));
        EXPECT_TRUE(hasSeries(text, std::string("sentinel_roller_invalidations_total{product=\"") + product + "\"}"));
    }
    EXPECT_TRUE(has(text, "# TYPE sentinel_roller_invalidations_total counter"));
    EXPECT_TRUE(has(text, "# TYPE sentinel_roller_write_errors_total counter"));
    for (const auto* metric : {"sentinel_recorder_disk_errors_total", "sentinel_recorder_queue_drops_total",
                               "sentinel_recorder_invalidations_total", "sentinel_recorder_late_events_total",
                               "sentinel_recorder_backward_steps_total"})
        EXPECT_EQ(text.find(metric), std::string::npos);
    // Both products, both layers: one complete minute each, no lead appends.
    for (size_t i = 0; i < 2; ++i) f.append(i, 63000, sentinel::capture::Kind::Frame);
    ASSERT_TRUE(f.waitFor("sentinel_recorder_columns_written_total 4")) << f.registry.render();
    EXPECT_TRUE(has(scrape(server.port()), "sentinel_recorder_columns_written_total 4"));
    f.append(0, 123000, sentinel::capture::Kind::Frame);
    ASSERT_TRUE(f.waitFor("sentinel_recorder_columns_written_total 6")) << f.registry.render();
    // Each initial journal boundary invalidates once; one explicit BTC event
    // must add exactly one and leave SOL's counter alone.
    ASSERT_TRUE(f.waitFor("sentinel_roller_invalidations_total{product=\"BTC-USD\"} 1"));
    f.append(0, 124000, sentinel::capture::Kind::BookInvalidated,
             "{\"product\":\"BTC-USD\",\"reason\":\"test invalidation\"}");
    ASSERT_TRUE(f.waitFor("sentinel_roller_invalidations_total{product=\"BTC-USD\"} 2"));
    EXPECT_TRUE(has(f.registry.render(), "sentinel_roller_invalidations_total{product=\"SOL-USD\"} 1"));
    // Replacing the daily recorder cannot reset the process counter or count
    // replayed minutes below the checkpoint as fresh appends.
    f.roller.reset();
    f.start();
    ASSERT_TRUE(f.waitFor("sentinel_roller_shadow_running{product=\"BTC-USD\"} 1"));
    f.append(0, 183000, sentinel::capture::Kind::Frame);
    ASSERT_TRUE(f.waitFor("sentinel_recorder_columns_written_total 8")) << f.registry.render();
    EXPECT_TRUE(has(scrape(server.port()), "sentinel_recorder_columns_written_total 8"));
}

TEST(ServerMetrics, RollerHmc2AppendFailureIsExported) {
    int argc = 1;
    char name[] = "roller-write-error-metrics";
    char* argv[] = {name, nullptr};
    QCoreApplication app(argc, argv);
    RollerMetricsFixture f;
    f.prepare();
    ASSERT_FALSE(HasFatalFailure());
    // Fail directory durability inside BTC's minute append, after store setup.
    // The other product still commits. Keep the hook installed through join.
    recording::Hmc2Store::setDirectorySyncHookForTest([](const auto& path) {
        if (path.filename() == "near-60000" && path.parent_path().filename() == "BTC-USD")
            throw std::runtime_error("injected HMC2 directory sync failure");
    });
    f.start();
    for (size_t i = 0; i < 2; ++i) f.append(i, 63000, sentinel::capture::Kind::Frame);
    ASSERT_TRUE(f.waitFor("sentinel_recorder_last_column_timestamp_seconds{product=\"SOL-USD\",layer=\"near\"} " +
                         std::to_string(f.epoch / 1000))) << f.registry.render();
    // Retry can cause more than one failed append; no failed write may count
    // as a successful minute. BTC deep (1) plus both SOL layers (2) = 3.
    ASSERT_TRUE(f.registry.hasSeries("sentinel_roller_write_errors_total", {{"product", "BTC-USD"}}));
    auto& failures = f.registry.counter("sentinel_roller_write_errors_total", "", {{"product", "BTC-USD"}});
    auto& retries = f.registry.counter("sentinel_roller_shadow_setup_failures_total", "", {{"product", "BTC-USD"}});
    QElapsedTimer timer;
    timer.start();
    while (retries.value() < 2 && timer.elapsed() < 5000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    MetricsHttpServer server(f.registry);
    ASSERT_TRUE(server.listen(0));
    const auto text = scrape(server.port());
    EXPECT_GT(failures.value(), 0u) << text;
    EXPECT_GE(retries.value(), 2u) << "The test must exercise failed-minute replay";
    EXPECT_TRUE(has(text, "sentinel_roller_write_errors_total{product=\"SOL-USD\"} 0"));
    EXPECT_TRUE(has(text, "sentinel_recorder_columns_written_total 3")) << text;
}


TEST(ServerMetrics, RollerMinuteCountExcludesHourlyRollups) {
    int argc = 1;
    char name[] = "roller-hour-metrics";
    char* argv[] = {name, nullptr};
    QCoreApplication app(argc, argv);
    RollerMetricsFixture f;
    f.prepare();
    ASSERT_FALSE(HasFatalFailure());
    f.start();
    // Only BTC advances: 60 minutes x 2 layers, plus a deep hourly rollup
    // on disk. The rollup must never become a 121st minute in the counter.
    f.append(0, 3603000, sentinel::capture::Kind::Frame);
    ASSERT_TRUE(f.waitFor("sentinel_recorder_columns_written_total 120")) << f.registry.render();
    QElapsedTimer timer;
    timer.start();
    while (f.roller->watermarks("BTC-USD", "deep").hourThroughMs < f.epoch + 3600000 &&
           timer.elapsed() < 5000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_GE(f.roller->watermarks("BTC-USD", "deep").hourThroughMs, f.epoch + 3600000);
    EXPECT_TRUE(has(f.registry.render(), "sentinel_recorder_columns_written_total 120"));
    const auto hours = recording::Hmc2Store::readRange(
        f.config.rollerShadow.outputRoot, "BTC-USD", "deep", 3600000, f.epoch, f.epoch + 3600000);
    ASSERT_EQ(hours.size(), 1u) << "The test must actually write an hourly rollup";
}
