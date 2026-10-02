// sentinel-server's /metrics wiring without the live data: ServerDataModel records
// into a QTemporaryDir (never the configured recording root), and the route is
// served by the same MetricsHttpServer the app uses, on an ephemeral port.
#include "metrics/MetricsHttpServer.hpp"
#include "metrics/MetricsRegistry.hpp"
#include "servermodel/ServerDataModel.hpp"
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
    EXPECT_TRUE(has(text, "sentinel_mdc_connected 0"));
    EXPECT_TRUE(has(text, "sentinel_mdc_transport_up_total 0"));
    EXPECT_TRUE(has(text, "sentinel_recorder_columns_written_total 0"));
    EXPECT_TRUE(has(text, "sentinel_recorder_disk_errors_total 0"));
    EXPECT_TRUE(has(text, "sentinel_recorder_live_publish_drops_total 0"));
    EXPECT_TRUE(has(text, "# TYPE sentinel_recorder_queue_drops_total counter"));
    // No column yet and disconnected: both per-series gauges stay absent, so an
    // alert on them cannot fire on a series that has nothing to measure.
    EXPECT_FALSE(hasSeries(text, "sentinel_recorder_last_column_timestamp_seconds{"));
    EXPECT_FALSE(hasSeries(text, "sentinel_recorder_column_overdue_seconds{"));

    model.onMarketDataConnectionChanged(true);
    model.onMarketDataConnectionChanged(true); // repeated status is not a transition
    text = scrape(server.port());
    EXPECT_TRUE(has(text, "sentinel_mdc_connected 1"));
    EXPECT_TRUE(has(text, "sentinel_mdc_transport_up_total 1"));
    EXPECT_TRUE(has(text, "sentinel_mdc_transport_down_total 0"));
    // Just connected: every pinned symbol x layer is on time.
    EXPECT_TRUE(has(text, "sentinel_recorder_column_overdue_seconds{product=\"BTC-USD\",layer=\"near\"} 0"));
    EXPECT_TRUE(has(text, "sentinel_recorder_column_overdue_seconds{product=\"BTC-USD\",layer=\"deep\"} 0"));

    model.onMarketDataConnectionChanged(false);
    text = scrape(server.port());
    EXPECT_TRUE(has(text, "sentinel_mdc_connected 0"));
    EXPECT_TRUE(has(text, "sentinel_mdc_transport_down_total 1"));
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
