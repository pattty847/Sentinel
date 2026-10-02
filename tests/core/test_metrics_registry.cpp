#include "metrics/MetricsHttpServer.hpp"
#include "metrics/MetricsRegistry.hpp"
#include "metrics/ProcessMetrics.hpp"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTcpSocket>
#include <QTimer>
#include <gtest/gtest.h>
#include <clocale>
#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

using sentinel::metrics::MetricsHttpServer;
using sentinel::metrics::MetricsRegistry;

TEST(MetricsRegistry, RendersPrometheusTextFormatInRegistrationOrder) {
    MetricsRegistry r;
    r.counter("sentinel_test_events_total", "Events seen.").inc(3);
    auto& g = r.gauge("sentinel_test_age_seconds", "Age.", {{"product", "BTC-USD"}, {"layer", "near"}});
    g.set(1.5);
    r.gauge("sentinel_test_age_seconds", "Age.", {{"product", "ETH-USD"}, {"layer", "deep"}}).set(-2);
    r.gaugeFn("sentinel_test_ratio", "Ratio.", {}, [] { return std::optional<double>(0.1); });
    r.counterFn("sentinel_test_omitted_total", "Omitted until known.", {}, [] { return std::optional<double>(); });
    const std::string expected =
        "# HELP sentinel_test_events_total Events seen.\n"
        "# TYPE sentinel_test_events_total counter\n"
        "sentinel_test_events_total 3\n"
        "# HELP sentinel_test_age_seconds Age.\n"
        "# TYPE sentinel_test_age_seconds gauge\n"
        "sentinel_test_age_seconds{product=\"BTC-USD\",layer=\"near\"} 1.5\n"
        "sentinel_test_age_seconds{product=\"ETH-USD\",layer=\"deep\"} -2\n"
        "# HELP sentinel_test_ratio Ratio.\n"
        "# TYPE sentinel_test_ratio gauge\n"
        "sentinel_test_ratio 0.1\n"
        "# HELP sentinel_test_omitted_total Omitted until known.\n"
        "# TYPE sentinel_test_omitted_total counter\n";
    EXPECT_EQ(r.render(), expected);
}

TEST(MetricsRegistry, EscapesLabelValuesAndHelp) {
    MetricsRegistry r;
    r.gauge("sentinel_test_escape", "Line one\nback\\slash \"quotes stay\".", {{"reason", "a\\b \"c\"\nd"}}).set(1);
    const std::string expected = "# HELP sentinel_test_escape Line one\\nback\\\\slash \"quotes stay\".\n"
                                 "# TYPE sentinel_test_escape gauge\n"
                                 "sentinel_test_escape{reason=\"a\\\\b \\\"c\\\"\\nd\"} 1\n";
    EXPECT_EQ(r.render(), expected);
}

TEST(MetricsRegistry, FormatsSpecialAndLargeValues) {
    EXPECT_EQ(MetricsRegistry::formatValue(std::numeric_limits<double>::quiet_NaN()), "NaN");
    EXPECT_EQ(MetricsRegistry::formatValue(std::numeric_limits<double>::infinity()), "+Inf");
    EXPECT_EQ(MetricsRegistry::formatValue(-std::numeric_limits<double>::infinity()), "-Inf");
    EXPECT_EQ(MetricsRegistry::formatValue(1759363200.25), "1759363200.25");
    EXPECT_EQ(MetricsRegistry::formatValue(48234496), "48234496");
    const double third = 1.0 / 3.0;
    EXPECT_EQ(std::strtod(MetricsRegistry::formatValue(third).c_str(), nullptr), third); // round-trips
}

// Qt's QCoreApplication calls setlocale(LC_ALL, ""), so a de_DE user locale
// reaches the C library; Prometheus text must still use '.' as the decimal point.
TEST(MetricsRegistry, NumbersIgnoreTheProcessLocale) {
    const std::string saved = std::setlocale(LC_NUMERIC, nullptr);
    const char* commaLocale = nullptr;
    for (const char* name : {"de_DE.UTF-8", "de_DE.utf8", "de_DE", "fr_FR.UTF-8", "fr_FR.utf8"})
        if (std::setlocale(LC_NUMERIC, name)) {
            commaLocale = name;
            break;
        }
    if (!commaLocale) GTEST_SKIP() << "no comma-decimal locale installed";
    char probe[16];
    std::snprintf(probe, sizeof probe, "%.1f", 1.5);
    EXPECT_STREQ(probe, "1,5") << "locale " << commaLocale << " does not use a decimal comma";
    MetricsRegistry r;
    r.gauge("sentinel_test_locale", "x").set(1.5);
    r.gaugeFn("sentinel_test_locale_fn", "x", {}, [] { return std::optional<double>(1759363200.25); });
    const std::string text = r.render();
    std::setlocale(LC_NUMERIC, saved.c_str());
    EXPECT_NE(text.find("\nsentinel_test_locale 1.5\n"), std::string::npos) << text;
    EXPECT_NE(text.find("\nsentinel_test_locale_fn 1759363200.25\n"), std::string::npos) << text;
}

TEST(MetricsRegistry, RejectsInvalidNamesAndConflicts) {
    MetricsRegistry r;
    EXPECT_THROW(r.counter("1bad", "x"), std::invalid_argument);
    EXPECT_THROW(r.counter("bad-name", "x"), std::invalid_argument);
    EXPECT_THROW(r.gauge("sentinel_ok", "x", {{"bad-label", "v"}}), std::invalid_argument);
    EXPECT_THROW(r.gauge("sentinel_ok", "x", {{"__reserved", "v"}}), std::invalid_argument);
    EXPECT_THROW(r.gauge("sentinel_ok", "x", {{"a", "1"}, {"a", "2"}}), std::invalid_argument);
    auto& c = r.counter("sentinel_same_total", "x", {{"k", "v"}});
    EXPECT_EQ(&r.counter("sentinel_same_total", "x", {{"k", "v"}}), &c); // same series, same object
    EXPECT_THROW(r.gauge("sentinel_same_total", "x"), std::logic_error);  // type conflict
    r.gaugeFn("sentinel_fn", "x", {}, [] { return std::optional<double>(1); });
    EXPECT_THROW(r.gaugeFn("sentinel_fn", "x", {}, [] { return std::optional<double>(2); }), std::logic_error);
    EXPECT_THROW(r.gauge("sentinel_fn", "x"), std::logic_error);
}

TEST(MetricsRegistry, ConcurrentIncrementsAreNotLost) {
    MetricsRegistry r;
    auto& c = r.counter("sentinel_test_concurrent_total", "x");
    auto& g = r.gauge("sentinel_test_concurrent_gauge", "x");
    constexpr int kThreads = 8, kPerThread = 100'000;
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t)
        threads.emplace_back([&] {
            for (int i = 0; i < kPerThread; ++i) {
                c.inc();
                g.add(1);
            }
        });
    // Scrapes race with the writers and must not block or crash.
    for (int i = 0; i < 50; ++i) EXPECT_FALSE(r.render().empty());
    for (auto& t : threads) t.join();
    EXPECT_EQ(c.value(), uint64_t(kThreads) * kPerThread);
    EXPECT_EQ(g.value(), double(kThreads) * kPerThread);
}

TEST(MetricsRegistry, ProcessMetricsReportThisProcess) {
    MetricsRegistry r;
    sentinel::metrics::registerProcessMetrics(r);
    const std::string text = r.render();
    EXPECT_NE(text.find("sentinel_build_info{version=\""), std::string::npos);
    EXPECT_NE(text.find("\nprocess_start_time_seconds "), std::string::npos);
#if defined(__APPLE__) || defined(__linux__)
    EXPECT_NE(text.find("# TYPE process_cpu_seconds_total counter\nprocess_cpu_seconds_total "), std::string::npos);
    const auto pos = text.find("\nprocess_resident_memory_bytes ");
    ASSERT_NE(pos, std::string::npos);
    EXPECT_GT(std::strtod(text.c_str() + pos + 31, nullptr), 1e6); // a live process holds > 1 MB
#else
    // No sampler on this platform: the family is declared and the sample omitted.
    EXPECT_NE(text.find("# TYPE process_cpu_seconds_total counter\n# HELP"), std::string::npos);
    EXPECT_EQ(text.find("\nprocess_resident_memory_bytes "), std::string::npos);
#endif
}

namespace {
// One HTTP exchange against the server, pumping the event loop it lives on.
QByteArray httpExchange(uint16_t port, const QByteArray& request) {
    QTcpSocket client;
    client.connectToHost(QHostAddress::LocalHost, port);
    QByteArray response;
    QElapsedTimer timer;
    timer.start();
    bool sent = false;
    while (timer.elapsed() < 3000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (!sent && client.state() == QAbstractSocket::ConnectedState) {
            client.write(request);
            sent = true;
        }
        client.waitForReadyRead(5);
        response += client.readAll();
        if (sent && client.state() == QAbstractSocket::UnconnectedState) break;
    }
    return response;
}

QByteArray body(const QByteArray& response) { return response.mid(response.indexOf("\r\n\r\n") + 4); }
} // namespace

TEST(MetricsHttpServer, ServesPingMetricsAndErrors) {
    int argc = 1;
    char name[] = "metrics-http";
    char* argv[] = {name, nullptr};
    QCoreApplication app(argc, argv);
    MetricsRegistry r;
    r.counter("sentinel_test_http_total", "Requests.").inc(7);
    MetricsHttpServer server(r);
    ASSERT_TRUE(server.listen(0)) << server.errorString().toStdString();
    ASSERT_NE(server.port(), 0);

    const QByteArray metrics = httpExchange(server.port(), "GET /metrics HTTP/1.1\r\nHost: x\r\n\r\n");
    EXPECT_TRUE(metrics.startsWith("HTTP/1.1 200 OK\r\n")) << metrics.toStdString();
    EXPECT_TRUE(metrics.contains("Content-Type: text/plain; version=0.0.4; charset=utf-8\r\n"));
    EXPECT_EQ(body(metrics).toStdString(), r.render());

    // The query string is not part of the route.
    const QByteArray query = httpExchange(server.port(), "GET /metrics?x=1 HTTP/1.1\r\n\r\n");
    EXPECT_EQ(body(query).toStdString(), r.render());

    const QByteArray ping = httpExchange(server.port(), "GET /ping HTTP/1.1\r\n\r\n");
    EXPECT_TRUE(ping.startsWith("HTTP/1.1 200 OK\r\n"));
    EXPECT_EQ(body(ping), "OK");

    EXPECT_TRUE(httpExchange(server.port(), "GET /nope HTTP/1.1\r\n\r\n").startsWith("HTTP/1.1 404"));
    EXPECT_TRUE(httpExchange(server.port(), "POST /metrics HTTP/1.1\r\n\r\n").startsWith("HTTP/1.1 405"));
    const QByteArray head = httpExchange(server.port(), "HEAD /metrics HTTP/1.1\r\n\r\n");
    EXPECT_TRUE(head.startsWith("HTTP/1.1 200 OK\r\n"));
    EXPECT_TRUE(body(head).isEmpty());
    // One byte over the limit with no header end: rejected without waiting for more.
    EXPECT_TRUE(httpExchange(server.port(), QByteArray(MetricsHttpServer::kMaxRequestBytes + 1, 'a'))
                    .startsWith("HTTP/1.1 400"));
}

TEST(MetricsHttpServer, WaitsForTheWholeRequestHeader) {
    int argc = 1;
    char name[] = "metrics-http-split";
    char* argv[] = {name, nullptr};
    QCoreApplication app(argc, argv);
    MetricsRegistry r;
    r.gauge("sentinel_test_split", "x").set(2);
    MetricsHttpServer server(r);
    ASSERT_TRUE(server.listen(0));
    QTcpSocket client;
    client.connectToHost(QHostAddress::LocalHost, server.port());
    QElapsedTimer timer;
    timer.start();
    while (client.state() != QAbstractSocket::ConnectedState && timer.elapsed() < 3000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    client.write("GET /metrics HTTP/1.1\r\n");
    client.flush();
    QByteArray early;
    for (int i = 0; i < 20; ++i) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        client.waitForReadyRead(5);
        early += client.readAll();
    }
    EXPECT_TRUE(early.isEmpty()) << "answered before the header ended: " << early.toStdString();
    client.write("Host: x\r\n\r\n");
    QByteArray response;
    while (timer.elapsed() < 3000 && client.state() != QAbstractSocket::UnconnectedState) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        client.waitForReadyRead(5);
        response += client.readAll();
    }
    response += client.readAll();
    EXPECT_EQ(body(response).toStdString(), r.render());
}

namespace {
void pumpFor(int ms) {
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
}
} // namespace

// A header terminator after the limit must not let an oversized request through.
TEST(MetricsHttpServer, RejectsOversizedCompleteRequest) {
    int argc = 1;
    char name[] = "metrics-http-oversized";
    char* argv[] = {name, nullptr};
    QCoreApplication app(argc, argv);
    MetricsRegistry r;
    MetricsHttpServer server(r);
    ASSERT_TRUE(server.listen(0));
    const QByteArray request = "GET /metrics HTTP/1.1\r\nX-Pad: " +
                               QByteArray(MetricsHttpServer::kMaxRequestBytes + 4096, 'a') + "\r\n\r\n";
    const QByteArray response = httpExchange(server.port(), request);
    // The answer is a 400, or nothing when the close resets the unread rest.
    EXPECT_FALSE(response.startsWith("HTTP/1.1 200")) << response.left(64).toStdString();
    if (!response.isEmpty()) EXPECT_TRUE(response.startsWith("HTTP/1.1 400")) << response.left(64).toStdString();
    pumpFor(50);
    EXPECT_EQ(server.activeConnections(), 0);
}

// Many clients that never finish a request: the cap holds, the main loop keeps
// running, the absolute timeout frees every socket and the endpoint recovers.
TEST(MetricsHttpServer, CapsConnectionsAndTimesOutIncompleteRequests) {
    int argc = 1;
    char name[] = "metrics-http-flood";
    char* argv[] = {name, nullptr};
    QCoreApplication app(argc, argv);
    MetricsRegistry r;
    MetricsHttpServer server(r);
    server.setRequestTimeoutMs(400);
    ASSERT_TRUE(server.listen(0));
    int ticks = 0;
    QTimer heartbeat;
    QObject::connect(&heartbeat, &QTimer::timeout, [&] { ++ticks; });
    heartbeat.start(10);

    constexpr int kClients = 20;
    std::vector<std::unique_ptr<QTcpSocket>> clients;
    for (int i = 0; i < kClients; ++i) {
        clients.push_back(std::make_unique<QTcpSocket>());
        clients.back()->connectToHost(QHostAddress::LocalHost, server.port());
    }
    pumpFor(150);
    for (auto& c : clients)
        if (c->state() == QAbstractSocket::ConnectedState) c->write("GET /metrics HTTP/1.1\r\n");
    pumpFor(100);
    EXPECT_EQ(server.activeConnections(), MetricsHttpServer::kMaxConnections);
    EXPECT_EQ(server.refusedConnections(), uint64_t(kClients - MetricsHttpServer::kMaxConnections));
    EXPECT_GT(ticks, 10) << "main loop stalled under the flood";

    pumpFor(600); // past the 400 ms deadline
    EXPECT_EQ(server.activeConnections(), 0);
    int open = 0;
    for (auto& c : clients) open += c->state() == QAbstractSocket::ConnectedState;
    EXPECT_EQ(open, 0) << "server kept sockets past the timeout";
    EXPECT_GT(ticks, 50);
    EXPECT_EQ(body(httpExchange(server.port(), "GET /ping HTTP/1.1\r\n\r\n")), "OK");
}
