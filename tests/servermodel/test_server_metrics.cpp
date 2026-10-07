// sentinel-server's /metrics wiring without the live data: ServerDataModel records
// into a QTemporaryDir (never the configured recording root), and the route is
// served by the same MetricsHttpServer the app uses, on an ephemeral port.
#include "metrics/EventLoopLag.hpp"
#include "metrics/MetricsHttpServer.hpp"
#include "metrics/MetricsRegistry.hpp"
#include "servermodel/ServerDataModel.hpp"
#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QEvent>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <gtest/gtest.h>
#include <memory>
#include <thread>

using sentinel::metrics::MetricsHttpServer;
using sentinel::metrics::MetricsRegistry;

namespace sentinel::metrics {
struct EventLoopLagTestAccess {
    using Clock = std::chrono::steady_clock;
    static void record(EventLoopLagSampler& sampler, int64_t sentMs, int64_t deliveredMs) {
        sampler.recordQueueLatency(Clock::time_point(std::chrono::milliseconds(sentMs)),
                                  Clock::time_point(std::chrono::milliseconds(deliveredMs)));
    }
    static double quantile(EventLoopLagSampler& sampler, double q, int64_t nowMs) {
        return sampler.quantileMsAt(q, nowMs);
    }
    static size_t capacity(const EventLoopLagSampler& sampler) { return sampler.m_ring.size(); }
    static size_t samples(const EventLoopLagSampler& sampler) { return sampler.m_size; }
    static bool joined(const EventLoopLagSampler& sampler) { return !sampler.m_worker.joinable(); }
    static bool outstanding(const EventLoopLagSampler& sampler) {
        return sampler.m_outstanding.load(std::memory_order_acquire);
    }
    static bool waitForPost(EventLoopLagSampler& sampler) {
        const auto end = Clock::now() + std::chrono::seconds(1);
        while (Clock::now() < end) {
            if (outstanding(sampler)) {
                // The helper holds this mutex through invokeMethod. Taking it
                // ensures the event is queued before the test starts blocking.
                std::lock_guard lock(sampler.m_stopMutex);
                return outstanding(sampler);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return false;
    }
    static void deliver(EventLoopLagSampler& sampler) {
        QCoreApplication::sendPostedEvents(&sampler.m_receiver, QEvent::MetaCall);
    }
    static void enableManualPosts(EventLoopLagSampler& sampler) {
        std::lock_guard lock(sampler.m_stopMutex);
        sampler.m_stopping = false;
    }
    static bool post(EventLoopLagSampler& sampler) {
        std::lock_guard lock(sampler.m_stopMutex);
        return sampler.postProbe();
    }
};
} // namespace sentinel::metrics

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
// Value of the exposition line starting with `series` (labels included), or -1.
double value(const std::string& text, const std::string& series) {
    const auto at = text.find("\n" + series + " ");
    return at == std::string::npos ? -1 : std::stod(text.substr(at + series.size() + 2));
}
void pump(int ms) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
}
} // namespace

// D-b1 flip gate: main-thread event-loop lag over the real /metrics route. An
// idle loop stays near zero; a queued slot that blocks the main thread for
// 400 ms shows up in the max gauge and the late-tick counter.
TEST(ServerMetrics, EventLoopLagShowsABlockedMainThread) {
    int argc = 1;
    char name[] = "server-metrics";
    char* argv[] = {name, nullptr};
    QCoreApplication app(argc, argv);
    MetricsRegistry registry;
    sentinel::metrics::EventLoopLagSampler lag;
    lag.registerMetrics(registry);
    MetricsHttpServer server(registry);
    ASSERT_TRUE(server.listen(0));
    std::string text = scrape(server.port());
    EXPECT_FALSE(hasSeries(text, "sentinel_server_event_loop_lag_ms{")) << "no tick yet";
    EXPECT_TRUE(has(text, "sentinel_server_event_loop_late_ticks_total 0"));

    lag.start();
    pump(1200);
    text = scrape(server.port());
    const double idleP99 = value(text, "sentinel_server_event_loop_lag_ms{quantile=\"0.99\"}");
    ASSERT_GE(idleP99, 0) << text;
    EXPECT_LT(idleP99, 50);
    EXPECT_TRUE(has(text, "sentinel_server_event_loop_late_ticks_total 0"));

    QMetaObject::invokeMethod(&app, [] { std::this_thread::sleep_for(std::chrono::milliseconds(400)); },
                              Qt::QueuedConnection);
    pump(600);
    text = scrape(server.port());
    EXPECT_GE(value(text, "sentinel_server_event_loop_lag_ms{quantile=\"max\"}"), 250) << text;
    EXPECT_LT(value(text, "sentinel_server_event_loop_lag_ms{quantile=\"0.5\"}"), 50);
    EXPECT_GE(value(text, "sentinel_server_event_loop_late_ticks_total"), 1);
}

TEST(ServerMetrics, EventLoopLagSkipsOutstandingPosts) {
    using Driver = sentinel::metrics::EventLoopLagTestAccess;
    int argc = 1;
    char name[] = "server-metrics-skipped";
    char* argv[] = {name, nullptr};
    QCoreApplication app(argc, argv);
    MetricsRegistry registry;
    sentinel::metrics::EventLoopLagSampler lag;
    lag.registerMetrics(registry);
    lag.start();
    // No main-thread event processing: a worker can post once, then must skip.
    std::this_thread::sleep_for(std::chrono::milliseconds(450));
    EXPECT_GE(value(registry.render(), "sentinel_server_event_loop_skipped_ticks_total"), 2);
    EXPECT_EQ(lag.quantileMs(1.0), -1); // The outstanding event has not run.
    Driver::deliver(lag);
    EXPECT_EQ(Driver::samples(lag), 1u); // Never pile up four queued calls.
    EXPECT_GE(lag.quantileMs(1.0), 250);
}

TEST(ServerMetrics, EventLoopLagSteadyBusyMainThread) {
    using sentinel::metrics::EventLoopLagSampler;
    using Driver = sentinel::metrics::EventLoopLagTestAccess;
    int argc = 1;
    char name[] = "server-metrics-busy";
    char* argv[] = {name, nullptr};
    QCoreApplication app(argc, argv);
    MetricsRegistry registry;
    EventLoopLagSampler lag;
    lag.registerMetrics(registry);
    lag.start();
    // Synchronize with actual helper posts, then hold the main thread for
    // 50 ms of each 100 ms period. This exercises real Qt queued delivery.
    for (int i = 0; i < 20; ++i) {
        ASSERT_TRUE(Driver::waitForPost(lag));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        Driver::deliver(lag);
    }
    EXPECT_EQ(Driver::samples(lag), 20u);
    EXPECT_GE(lag.quantileMs(0.5), 45);
    EXPECT_LT(lag.quantileMs(0.5), 80);
    EXPECT_EQ(value(registry.render(), "sentinel_server_event_loop_late_ticks_total"), 0);
}

// Deterministic timestamps exercise the same slot arithmetic and scrape window
// without waiting a minute or injecting a clock into the production callback.
TEST(ServerMetrics, EventLoopLagQueueLatencyUsesSendTimeAndRecovers) {
    using sentinel::metrics::EventLoopLagSampler;
    using Driver = sentinel::metrics::EventLoopLagTestAccess;
    MetricsRegistry registry;
    EventLoopLagSampler lag;
    lag.registerMetrics(registry);
    // The helper's actual send time controls queue wait, independent of cadence.
    for (int i = 1; i <= 200; ++i) Driver::record(lag, i * 100 + 30, i * 100 + 80);
    EXPECT_DOUBLE_EQ(Driver::quantile(lag, 0.5, 20'080), 50);
    Driver::record(lag, 20'100, 20'950); // One 850 ms queue wait.
    EXPECT_DOUBLE_EQ(Driver::quantile(lag, 1.0, 20'950), 850);
    for (int64_t now = 21'000; now <= 43'000; now += 100) Driver::record(lag, now, now);
    EXPECT_DOUBLE_EQ(Driver::quantile(lag, 0.5, 43'000), 0);
    EXPECT_DOUBLE_EQ(Driver::quantile(lag, 1.0, 43'000), 850);
    EXPECT_EQ(value(registry.render(), "sentinel_server_event_loop_late_ticks_total"), 1);
    // Once the stall expires, only normally delivered ticks remain.
    EXPECT_DOUBLE_EQ(Driver::quantile(lag, 1.0, 80'951), 0);
}

TEST(ServerMetrics, EventLoopLagWindowExpiryAndRingWrap) {
    using sentinel::metrics::EventLoopLagSampler;
    using Driver = sentinel::metrics::EventLoopLagTestAccess;
    EventLoopLagSampler ranks;
    EXPECT_EQ(Driver::quantile(ranks, 0.5, 0), -1);
    for (int i = 1; i <= 4; ++i) Driver::record(ranks, i * 100, i * 110);
    // Queue waits 10, 20, 30, 40 ms; nearest-rank quantiles.
    EXPECT_DOUBLE_EQ(Driver::quantile(ranks, 0.5, 440), 20);
    for (double q : {0.95, 0.99, 1.0}) EXPECT_DOUBLE_EQ(Driver::quantile(ranks, q, 440), 40);
    EXPECT_DOUBLE_EQ(Driver::quantile(ranks, 0.5, 60'110), 20); // Boundary included.
    EXPECT_DOUBLE_EQ(Driver::quantile(ranks, 0.5, 60'111), 30); // First sample expired.
    EXPECT_DOUBLE_EQ(Driver::quantile(ranks, 1.0, 60'440), 40);
    EXPECT_EQ(Driver::quantile(ranks, 1.0, 60'441), -1); // Empty window.

    EventLoopLagSampler wrapped;
    Driver::record(wrapped, 100, 450); // Old 350 ms queue wait.
    int64_t now = 0;
    for (size_t i = 0; i < 3 * Driver::capacity(wrapped); ++i) {
        now = 600 + int64_t(i) * 100;
        Driver::record(wrapped, now - (25 + int64_t(i % 4) * 10), now);
    }
    // Several ring wraps must evict the stall and preserve the newest window.
    // The inclusive window contains 601 samples: 150 each of 25/35/45 ms,
    // and 151 of 55 ms. Keeping only the last value would fail the median.
    EXPECT_DOUBLE_EQ(Driver::quantile(wrapped, 0.5, now), 45);
    for (double q : {0.95, 0.99, 1.0}) EXPECT_DOUBLE_EQ(Driver::quantile(wrapped, q, now), 55);
    EXPECT_DOUBLE_EQ(Driver::quantile(wrapped, 1.0, now + 60'000), 55);
    EXPECT_EQ(Driver::quantile(wrapped, 1.0, now + 60'001), -1);
}

TEST(ServerMetrics, EventLoopLagOutstandingGateIsBounded) {
    using Driver = sentinel::metrics::EventLoopLagTestAccess;
    int argc = 1;
    char name[] = "server-metrics-gate";
    char* argv[] = {name, nullptr};
    QCoreApplication app(argc, argv);
    MetricsRegistry registry;
    sentinel::metrics::EventLoopLagSampler lag;
    lag.registerMetrics(registry);
    Driver::enableManualPosts(lag);
    EXPECT_TRUE(Driver::post(lag));
    for (int i = 0; i < 4; ++i) EXPECT_FALSE(Driver::post(lag));
    EXPECT_EQ(value(registry.render(), "sentinel_server_event_loop_skipped_ticks_total"), 4);
    EXPECT_EQ(Driver::samples(lag), 0u);
    Driver::deliver(lag);
    EXPECT_EQ(Driver::samples(lag), 1u);
    EXPECT_FALSE(Driver::outstanding(lag));
    EXPECT_TRUE(Driver::post(lag)); // Delivery opens the gate again.
    Driver::deliver(lag);
    EXPECT_EQ(Driver::samples(lag), 2u);
}

TEST(ServerMetrics, EventLoopLagShutdownJoinsAndCancelsPendingPost) {
    using Driver = sentinel::metrics::EventLoopLagTestAccess;
    using Clock = std::chrono::steady_clock;
    int argc = 1;
    char name[] = "server-metrics-stop";
    char* argv[] = {name, nullptr};
    QCoreApplication app(argc, argv);
    MetricsRegistry registry;
    sentinel::metrics::EventLoopLagSampler lag;
    lag.registerMetrics(registry);
    lag.start();
    ASSERT_TRUE(Driver::waitForPost(lag));
    const auto begin = Clock::now();
    lag.stop();
    const double stopMs = std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
    EXPECT_LT(stopMs, 75);
    EXPECT_TRUE(Driver::joined(lag));
    EXPECT_FALSE(Driver::outstanding(lag));
    EXPECT_FALSE(Driver::post(lag)); // The stopped gate rejects new posts.
    const auto skipped = value(registry.render(), "sentinel_server_event_loop_skipped_ticks_total");
    std::this_thread::sleep_for(std::chrono::milliseconds(220));
    Driver::deliver(lag);
    EXPECT_EQ(Driver::samples(lag), 0u); // Pending pre-stop event was cancelled.
    EXPECT_EQ(value(registry.render(), "sentinel_server_event_loop_skipped_ticks_total"), skipped);

    lag.start();
    ASSERT_TRUE(Driver::waitForPost(lag));
    Driver::deliver(lag);
    EXPECT_EQ(Driver::samples(lag), 1u); // No stale call survived the restart.
    lag.stop();
    // The destructor must also join while a post is still pending.
    auto pending = std::make_unique<sentinel::metrics::EventLoopLagSampler>();
    pending->start();
    ASSERT_TRUE(Driver::waitForPost(*pending));
    const auto destroyBegin = Clock::now();
    pending.reset();
    const double destroyMs = std::chrono::duration<double, std::milli>(Clock::now() - destroyBegin).count();
    EXPECT_LT(destroyMs, 75);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
}
