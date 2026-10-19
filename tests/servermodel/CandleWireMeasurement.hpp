#pragma once
#include "metrics/EventLoopLag.hpp"
#include "metrics/ProcessMetrics.hpp"
#include <QElapsedTimer>
#include <QTimer>
#include <sstream>

namespace candle_measurement {
inline std::atomic<uint64_t> updates{0}, closed{0};
inline void countProbes(QtMsgType type, const QMessageLogContext &, const QString &text) {
    if (text.contains("[candles.update]")) ++updates;
    if (text.contains("[candles.closed]")) ++closed;
    if (type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg) std::cerr << text.toStdString() << '\n';
}
inline double rss(const sentinel::metrics::MetricsRegistry &metrics) {
    std::istringstream input(metrics.render());
    std::string line;
    while (std::getline(input, line)) {
        const std::string prefix = "process_resident_memory_bytes ";
        if (line.starts_with(prefix)) return std::stod(line.substr(prefix.size()));
    }
    throw std::runtime_error("RSS sampler did not publish");
}

inline int run(QCoreApplication &app, bool hundred) {
    qputenv("SENTINEL_PROBES", "candles.update,candles.closed");
    const auto previous = qInstallMessageHandler(countProbes);
    const auto restore = qScopeGuard([&] { qInstallMessageHandler(previous); });
    sentinel::metrics::MetricsRegistry metrics;
    sentinel::metrics::registerProcessMetrics(metrics);
    sentinel::metrics::EventLoopLagSampler lag;
    lag.registerMetrics(metrics);
    CandleFixture fixture(hundred);
    const std::vector<std::string> products{"BTC-USD",  "ETH-USD", "SOL-USD", "XRP-USD",
                                            "DOGE-USD", "ADA-USD", "AVAX-USD"};
    const double before = rss(metrics);
    // Prefill 35 journal minutes without waiting 35 wall-clock minutes.
    const int64_t epoch = (QDateTime::currentMSecsSinceEpoch() / 1000) * 1000;
    for (int second = -2100; second < 0; ++second) {
        for (const auto &product : products) {
            tradeAt(*fixture.model, epoch + second * 1000, product);
            fixture.model->onFeedWatermark(product, epoch + second * 1000);
        }
    }
    Json retained = Json::object();
    for (const auto &product : products) {
        const auto bars = fixture.model->getHistory(product, hundred ? 100 : 1000, 22000);
        retained[product] = bars.size();
        if (hundred && (bars.size() < 18000 || bars.size() > 19800 || bars.front().timestamp_ms <= epoch - 2100000))
            throw std::runtime_error("prefill did not reach bounded 100ms retention");
    }
    const double prefilled = rss(metrics);
    fixture.start();
    StalledCandleClient stalled(fixture.port());
    auto sessions = RecordingServerStopTest::sessions(*fixture.server);
    if (sessions.size() != 1 || !spinUntil([&] { return fixture.subscribed(sessions.front()); }))
        throw std::runtime_error("stalled subscriber was not admitted");
    auto stalledSession = sessions.front();
    SentinelStreamClient paging("127.0.0.1", std::to_string(fixture.port()));
    int64_t cursor = epoch - 100;
    const int64_t tf = hundred ? 100 : 1000;
    uint64_t pages = 0;
    bool pagingConnected = false;
    auto request = [&] { paging.requestCandleHistory("BTC-USD", tf, cursor, 350); };
    QObject::connect(&paging, &SentinelStreamClient::connected, &app, [&] {
        pagingConnected = true;
        request();
    });
    QObject::connect(&paging, &SentinelStreamClient::candleHistoryReceived, &app,
                     [&](const QString &, int64_t, int64_t, int64_t, const auto &bars) {
                         ++pages;
                         cursor = bars.empty() ? QDateTime::currentMSecsSinceEpoch() : bars.front().timeStartMs - tf;
                         QTimer::singleShot(100, &paging, request);
                     });
    paging.connectToServer();
    if (!spinUntil([&] { return pagingConnected && pages > 0; }))
        throw std::runtime_error("paging client was not ready");
    sessions = RecordingServerStopTest::sessions(*fixture.server);
    if (sessions.size() != 2) throw std::runtime_error("expected two measurement sessions");
    lag.start();
    QElapsedTimer elapsed;
    elapsed.start();
    double peakRss = prefilled, peakP99 = 0;
    int64_t nextTrade = 0;
    int lastSecond = -1;
    uint64_t previousUpdates = updates.load(), previousClosed = closed.load();
    bool anyClosed = false, resumed = false;
    size_t peakModel = 0, peakWrite = 0;
    beast::flat_buffer drainBuffer;
    std::function<void()> read;
    read = [&] {
        stalled.ws.async_read(drainBuffer, [&](beast::error_code ec, size_t) {
            if (ec) {
                if (ec != net::error::operation_aborted)
                    qWarning() << "Fixture reader stopped:" << QString::fromStdString(ec.message());
                return;
            }
            drainBuffer.consume(drainBuffer.size());
            read();
        });
    };
    std::thread reader;
    const auto cleanup = qScopeGuard([&] {
        paging.disconnectFromServer();
        if (reader.joinable()) {
            net::post(stalled.io, [&] {
                beast::error_code ec;
                beast::get_lowest_layer(stalled.ws).socket().close(ec);
            });
            reader.join();
        }
        lag.stop();
    });
    QTimer ticks;
    ticks.setTimerType(Qt::PreciseTimer);
    QObject::connect(&ticks, &QTimer::timeout, &app, [&] {
        const int64_t ms = elapsed.elapsed();
        while (nextTrade <= ms && nextTrade < 300000) {
            tradeAt(*fixture.model, epoch + nextTrade);
            if (nextTrade % 1000 == 0) {
                for (size_t i = 1; i < products.size(); ++i) tradeAt(*fixture.model, epoch + nextTrade, products[i]);
                for (const auto &product : products) fixture.model->onFeedWatermark(product, epoch + nextTrade);
            }
            nextTrade += 20;
        }
        if (ms >= 20000 && !resumed) {
            resumed = true;
            read();
            reader = std::thread([&] { stalled.io.run(); });
        }
        for (const auto &s : sessions) {
            peakModel = std::max(peakModel, RecordingServerStopTest::peakModelEvents(*s));
            peakWrite = std::max(peakWrite, RecordingServerStopTest::peakWriteBytes(*s));
            anyClosed |= RecordingServerStopTest::closePosted(*s) || RecordingServerStopTest::closing(*s);
        }
        const int second = static_cast<int>(ms / 1000);
        if (second != lastSecond) {
            lastSecond = second;
            const double resident = rss(metrics);
            const double p99 = lag.quantileMs(.99);
            peakRss = std::max(peakRss, resident);
            peakP99 = std::max(peakP99, p99);
            const auto u = updates.load(), c = closed.load();
            std::cout << Json{{"second", second},
                              {"updates", u - previousUpdates},
                              {"closed", c - previousClosed},
                              {"p99_ms", p99},
                              {"rss_bytes", resident},
                              {"peak_pending_model", peakModel},
                              {"peak_pending_write", peakWrite},
                              {"session_closed", anyClosed}}
                             .dump()
                      << std::endl;
            previousUpdates = u;
            previousClosed = c;
        }
        if (ms >= 300000 || anyClosed) app.quit();
    });
    std::cout << Json{{"measurement", hundred ? "100ms" : "without100"},
                      {"sizeof_bar", sizeof(OHLCVBar)},
                      {"exe", QCoreApplication::applicationFilePath().toStdString()},
                      {"built", __DATE__ " " __TIME__},
                      {"prefilled_bars", retained},
                      {"prefill", "35 minutes synthetic journal timestamps"},
                      {"rss_before", before},
                      {"rss_prefilled", prefilled}}
                     .dump()
              << std::endl;
    ticks.start(10);
    app.exec();
    std::cout << Json{{"summary", hundred ? "100ms" : "without100"},
                      {"max_p99_ms", peakP99},
                      {"rss_growth_bytes", peakRss - before},
                      {"steady_rss_growth_bytes", peakRss - prefilled},
                      {"peak_pending_model", peakModel},
                      {"peak_pending_write", peakWrite},
                      {"session_closed", anyClosed},
                      {"history_pages", pages},
                      {"updates", updates.load()},
                      {"closed", closed.load()}}
                     .dump()
              << std::endl;
    return anyClosed || peakModel >= 256 || peakRss - before >= 40 * 1024 * 1024 ? 1 : 0;
}
} // namespace candle_measurement
