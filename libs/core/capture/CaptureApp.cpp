#include "CaptureApp.hpp"
#include "CaptureMetrics.hpp"
#include "CaptureSession.hpp"
#include "CaptureVerifier.hpp"
#include "SentinelLogging.hpp"
#include "Version.hpp"
#include "marketdata/MarketDataFeeds.hpp"
#include "marketdata/rest/CoinbaseRestClient.hpp"
#include "metrics/MetricsHttpServer.hpp"
#include "metrics/MetricsRegistry.hpp"
#include "metrics/ProcessMetrics.hpp"
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QLockFile>
#include <QTimer>
#include <atomic>
#include <map>
#include <algorithm>
#include <csignal>
#include <iostream>
#include <stdexcept>

namespace sentinel::capture {
namespace {
volatile std::sig_atomic_t stopSignal = 0;
extern "C" void captureSignal(int signal) { stopSignal = signal; }
uint32_t number(const QCommandLineParser& parser, const QString& option, uint32_t min, uint32_t max) {
    bool ok = false;
    const auto value = parser.value(option).toULongLong(&ok);
    if (!ok || value < min || value > max) throw std::runtime_error("invalid --" + option.toStdString());
    return static_cast<uint32_t>(value);
}
} // namespace

int runApplication(QCoreApplication& app) {
    return runApplication(app, {});
}

int runApplication(QCoreApplication& app, const ApplicationDependencies& dependencies) {
    QCoreApplication::setApplicationName("sentinel-capture");
    QCoreApplication::setApplicationVersion(QString::fromStdString(Sentinel::getFullVersionString()));
    QCommandLineParser parser;
    parser.setApplicationDescription("Pristine Coinbase market-data capture; independent of sentinel-server. SIGTERM drains and seals files.");
    parser.addHelpOption(); parser.addVersionOption();
    parser.addOptions({
        {"root", "Mounted storage root (never the server recording directory).", "directory", "/Volumes/T7/sentinel-data/raw-l2"},
        {"symbol", "Coinbase product; repeat for several (default BTC-USD).", "product"},
        {"symbols", "Comma-separated Coinbase products, one connection each.", "A,B,C"},
        {"strict-trades", "With --verify, exit 2 for any unfilled trade-id gap; archive integrity is unchanged."},
        {"verify", "Offline verify one file or a directory; JSON report, exit 2 for integrity failures, 3 for valid incomplete/open captures.", "path"},
        {"block-ms", "Maximum target block latency in milliseconds.", "ms", "1000"},
        {"block-bytes", "Target uncompressed block bytes (large frames remain whole).", "bytes", "1048576"},
        {"fsync-blocks", "Fsync every N blocks; 0 only syncs at close.", "N", "1"},
        {"zstd-level", "Compression level 1..19.", "level", "3"},
        {"queue-mib", "Shared disk queue pool for all products, allocated only as frames queue; overflow exits with an error.", "MiB", "512"},
        {"queue-floor-mib", "Per-product share of the pool that no other product can take; products x floor <= pool.", "MiB", "2"},
        {"metrics-port", "Prometheus /metrics and /ping on 127.0.0.1 (0: no listener).", "port", "8091"},
        {"duration", "Stop cleanly after N seconds (0: until SIGTERM/SIGINT).", "seconds", "0"},
        {"key-file", "Optional existing Coinbase credentials; public channels need no key.", "path", "key.json"},
        {"jwt", "Use existing Coinbase JWT auth (requires --key-file)."},
        {"ca-bundle", "TLS CA bundle for the existing REST/WS clients.", "path", "resources/certs/ca-bundle.crt"}
    });
    parser.process(app);
    if (parser.isSet("strict-trades") && !parser.isSet("verify")) throw std::runtime_error("--strict-trades requires --verify");
    if (parser.isSet("verify")) {
        const auto report = verify(parser.value("verify"));
        std::cout << report.json.dump(2) << '\n';
        return report.exitCode(parser.isSet("strict-trades"));
    }
    WriterConfig config;
    config.root = validateRoot(parser.value("root"));
    QStringList requested = parser.values("symbol");
    for (const auto& list : parser.values("symbols")) requested.append(list.split(','));
    if (requested.empty()) requested.append("BTC-USD");
    std::vector<std::string> symbols;
    for (const auto& value : requested) {
        const auto symbol = value.trimmed().toStdString();
        validateSymbol(symbol);
        symbols.push_back(symbol);
    }
    std::sort(symbols.begin(), symbols.end());
    symbols.erase(std::unique(symbols.begin(), symbols.end()), symbols.end());
    if (symbols.size() > MaxProducts) throw std::runtime_error("at most 32 products per capture");
    const auto symbolList = nlohmann::json(symbols).dump();
    config.blockBytes = number(parser, "block-bytes", 1, MaxRecordBytes);
    config.blockInterval = std::chrono::milliseconds(number(parser, "block-ms", 1, 60000));
    config.fsyncBlocks = number(parser, "fsync-blocks", 0, 1000000);
    config.compressionLevel = number(parser, "zstd-level", 1, 19);
    const auto queueBytes = size_t(number(parser, "queue-mib", 1, 4096)) * 1024 * 1024;
    const auto floorBytes = size_t(number(parser, "queue-floor-mib", 0, 256)) * 1024 * 1024;
    if (floorBytes * symbols.size() > queueBytes)
        throw std::runtime_error("--queue-floor-mib x products exceeds --queue-mib");
    const auto metricsPort = static_cast<uint16_t>(number(parser, "metrics-port", 0, 65535));
    const auto duration = number(parser, "duration", 0, 365 * 86400);
    std::vector<std::unique_ptr<QLockFile>> locks;
    for (const auto& symbol : symbols) {
        const auto directory = prepareDirectory(config.root + '/' + QString::fromStdString(symbol));
        auto lock = std::make_unique<QLockFile>(directory + "/.capture.lock");
        lock->setStaleLockTime(0);
        if (!lock->tryLock()) throw std::runtime_error("another capture owns product directory: " + symbol);
        locks.push_back(std::move(lock));
    }
    stopSignal = 0;
    const auto previousTerm = std::signal(SIGTERM, captureSignal);
    const auto previousInt = std::signal(SIGINT, captureSignal);
    struct RestoreSignals {
        decltype(previousTerm) term, interrupt;
        ~RestoreSignals() { std::signal(SIGTERM, term); std::signal(SIGINT, interrupt); }
    } restore{previousTerm, previousInt};

    Authenticator auth(parser.value("key-file").toStdString());
    ServerMdcConfig mdc;
    mdc.useJwt = parser.isSet("jwt");
    mdc.sslCaBundle = parser.value("ca-bundle").toStdString();
    if (mdc.useJwt && !auth.hasCredentials()) throw std::runtime_error("--jwt requires valid credentials");
    CoinbaseRestClient rest(auth, "api.coinbase.com", "443", mdc.sslCaBundle);
    std::vector<ProductCapture> products;
    for (const auto& symbol : symbols) {
        const auto metadata = dependencies.fetchMetadata ? dependencies.fetchMetadata(rest, symbol) :
                                                       rest.fetchProductMetadata(symbol);
        if (!metadata.ok) throw std::runtime_error("product metadata fetch failed (no connection opened): " + symbol + ": " + metadata.error);
        DecimalGrid quote(metadata.quoteIncrement), base(metadata.baseIncrement);
        if (stopSignal) return 0;
        const auto fetched = Stamp::now();
        nlohmann::json header = {{"tool", "sentinel-capture"}, {"tool_version", Sentinel::getFullVersionString()},
            {"build", Sentinel::getBuildInfo()}, {"product_metadata", metadata.metadata},
            {"metadata_source", "https://api.coinbase.com" + metadata.sourcePath},
            {"metadata_fetched_system_ns", fetched.systemNs}, {"queue_bytes", queueBytes}, {"queue_floor_bytes", floorBytes},
            {"duration_seconds", duration}, {"ca_bundle", mdc.sslCaBundle},
            {"ws", {{"host", mdc.host}, {"port", mdc.port}, {"target", mdc.target}, {"use_jwt", mdc.useJwt}}},
            {"clock", "nanoseconds since system_clock/steady_clock epoch; sampled at engine pre-parse ingest seam"}};
        auto productConfig = config;
        productConfig.symbol = symbol;
        products.push_back({std::move(productConfig), std::move(header)});
    }
    // One connection, one session and one RAWL2 v1 stream per product, all
    // accounting to one shared queue pool (per-symbol connections, decision 7).
    struct ProductState {
        std::unique_ptr<Session> session;
        std::unique_ptr<FeedMetrics> feed;
        uint64_t connection = 0;
        std::string transportReason;
    };
    // Declared before the states: samplers point into them, and the listener
    // (declared after them) is destroyed first, so no scrape renders a dead session.
    sentinel::metrics::MetricsRegistry registry;
    std::map<std::string, ProductState> states;
    auto pool = std::make_shared<QueuePool>(queueBytes, floorBytes, products.size());
    const auto startedSteady = Stamp::now().steadyNs;
    std::vector<CaptureMetricsSource> metricSources;
    for (auto& product : products) {
        auto symbol = product.config.symbol;
        auto& state = states[symbol];
        state.feed = std::make_unique<FeedMetrics>(startedSteady);
        state.session = std::make_unique<Session>(std::move(product.config), std::move(product.metadata), pool, states.size() - 1);
        state.session->submit({Kind::CaptureStarted, Stamp::now(), 0, R"({"reason":"capture started"})"});
        metricSources.push_back({symbol, state.feed.get(), state.session.get()});
    }
    sentinel::metrics::registerProcessMetrics(registry);
    registerCaptureMetrics(registry, pool, std::move(metricSources));
    std::unique_ptr<sentinel::metrics::MetricsHttpServer> metricsServer;
    if (metricsPort) {
        metricsServer = std::make_unique<sentinel::metrics::MetricsHttpServer>(registry);
        if (metricsServer->listen(metricsPort))
            sLog_App("Capture metrics endpoint listening on 127.0.0.1:" << metricsPort << " (/ping, /metrics)");
        else
            sLog_Warning("Capture metrics endpoint failed to bind on 127.0.0.1:" << metricsPort
                         << " error=" << metricsServer->errorString());
    }
    const auto error = [&]() -> std::string {
        for (const auto& [symbol, state] : states)
            if (auto e = state.session->error(); !e.empty()) return symbol + ": " + e;
        return {};
    };
    const auto submit = [](ProductState& state, Record record) {
        state.session->submit(std::move(record));
    };
    auto feeds = dependencies.makeFeeds ? dependencies.makeFeeds(auth, mdc) : std::make_unique<MarketDataFeeds>(auth, mdc);
    if (!feeds) throw std::runtime_error("capture feeds factory returned null");
    feeds->onIngest([&](const MarketDataCoreEngine::IngestObservation& observation) noexcept {
        auto& state = states.at(std::string(observation.product));
        auto& session = *state.session;
        try {
            using Ingest = MarketDataCoreEngine::IngestKind;
            Kind kind = Kind::Frame;
            state.connection = observation.connection;
            switch (observation.kind) {
            case Ingest::Frame: kind = Kind::Frame; break;
            case Ingest::TransportUp: kind = Kind::TransportUp; state.transportReason.clear();
                state.feed->transport(true, observation.connection, observation.steadyNs); break;
            case Ingest::TransportDown: kind = Kind::TransportDown;
                state.feed->transport(false, observation.connection, observation.steadyNs); break;
            case Ingest::BookInvalidated: kind = Kind::BookInvalidated; break;
            case Ingest::ResyncRequested: kind = Kind::ResyncRequested; state.transportReason = observation.reason; break;
            }
            auto reason = observation.reason;
            if (kind == Kind::TransportUp) reason = "websocket connected";
            if (kind == Kind::TransportDown) reason = state.transportReason.empty() ? std::string_view("transport closed") : state.transportReason;
            auto payload = kind == Kind::Frame ? std::string(observation.payload) :
                nlohmann::json({{"product", observation.product}, {"reason", reason}}).dump();
            Record record{kind, {observation.systemNs, observation.steadyNs}, state.connection, std::move(payload)};
            submit(state, std::move(record));
        } catch (const std::exception& e) {
            session.fail(e.what(), RecordLocation{{observation.systemNs, observation.steadyNs}, state.connection,
                observation.kind == MarketDataCoreEngine::IngestKind::Frame ? Kind::Frame : Kind::EngineError});
        }
    });
    feeds->onError([&](const std::string& product, const std::string& message) noexcept {
        auto& state = states.at(product);
        try {
            state.transportReason = message;
            submit(state, {Kind::EngineError, Stamp::now(), state.connection,
                nlohmann::json({{"product", product}, {"reason", message}}).dump()});
        } catch (const std::exception& e) { state.session->fail(e.what()); }
    });
    for (const auto& symbol : symbols) feeds->add(symbol);
    feeds->start();
    const auto started = Stamp::now().steadyNs;
    auto lastStats = started;
    bool stopped = false;
    std::string stopReason;
    QTimer timer;
    timer.setInterval(100);
    QObject::connect(&timer, &QTimer::timeout, &app, [&] {
        const auto now = Stamp::now().steadyNs;
        if (stopSignal || !error().empty() || (duration && now - started >= int64_t(duration) * 1000000000)) {
            stopReason = stopSignal ? "signal=" + std::to_string(stopSignal) :
                         !error().empty() ? "capture failure" : "duration elapsed";
            stopped = true; app.quit(); return;
        }
        if (now - lastStats >= 60LL * 1000000000) {
            // Atomics only: the main thread never waits on the I/O or disk threads.
            for (const auto& [symbol, state] : states) {
                const auto& feed = *state.feed;
                sLog_App("Capture stats: product=" << symbol << " conn=" << feed.connection.load(std::memory_order_relaxed)
                    << " up=" << feed.up.load(std::memory_order_relaxed) << " storedFrames=" << state.session->storedFrames()
                    << " fileBytes=" << state.session->storedFileBytes() << " queuedBytes=" << state.session->queuedBytes());
            }
            sLog_App("Capture queue: usedBytes=" << pool->used() << " poolBytes=" << pool->total()
                << " floorBytes=" << pool->floor() << " products=" << states.size());
            lastStats = now;
        }
    }, Qt::QueuedConnection);
    timer.start();
    sLog_App("Capture running: products=" << symbolList << " root=" << config.root << " pid=" << QCoreApplication::applicationPid()
             << " queuePoolBytes=" << queueBytes << " queueFloorBytes=" << floorBytes);
    app.exec();
    timer.stop();
    feeds->stop(); feeds.reset(); // join the producer before draining the writers
    if (!stopped) stopReason = "application exit";
    for (auto& [_, state] : states) state.session->close(stopReason);
    if (const auto failure = error(); !failure.empty()) {
        sLog_Error("Capture incomplete: error=" << failure); return 1;
    }
    sLog_App("Capture closed: reason=" << stopReason << " products=" << symbolList);
    return 0;
}
} // namespace sentinel::capture
