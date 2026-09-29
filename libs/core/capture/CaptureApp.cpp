#include "CaptureSession.hpp"
#include "CaptureVerifier.hpp"
#include "SentinelLogging.hpp"
#include "Version.hpp"
#include "marketdata/MarketDataCoreEngine.hpp"
#include "marketdata/rest/CoinbaseRestClient.hpp"
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QLockFile>
#include <QTimer>
#include <atomic>
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
    QCoreApplication::setApplicationName("sentinel-capture");
    QCoreApplication::setApplicationVersion(QString::fromStdString(Sentinel::getFullVersionString()));
    QCommandLineParser parser;
    parser.setApplicationDescription("Pristine Coinbase market-data capture; independent of sentinel-server. SIGTERM drains and seals files.");
    parser.addHelpOption(); parser.addVersionOption();
    parser.addOptions({
        {"root", "Mounted storage root (never the server recording directory).", "directory", "/Volumes/T7/sentinel-data/raw-l2"},
        {"symbol", "Coinbase product.", "product", "BTC-USD"},
        {"verify", "Offline verify one file or a directory; JSON report, exit 2 if incomplete/invalid.", "path"},
        {"block-ms", "Maximum target block latency in milliseconds.", "ms", "1000"},
        {"block-bytes", "Target uncompressed block bytes (large frames remain whole).", "bytes", "1048576"},
        {"fsync-blocks", "Fsync every N blocks; 0 only syncs at close.", "N", "1"},
        {"zstd-level", "Compression level 1..19.", "level", "3"},
        {"queue-mib", "Bounded disk queue; overflow exits with an error.", "MiB", "64"},
        {"duration", "Stop cleanly after N seconds (0: until SIGTERM/SIGINT).", "seconds", "0"},
        {"key-file", "Optional existing Coinbase credentials; public channels need no key.", "path", "key.json"},
        {"jwt", "Use existing Coinbase JWT auth (requires --key-file)."},
        {"ca-bundle", "TLS CA bundle for the existing REST/WS clients.", "path", "resources/certs/ca-bundle.crt"}
    });
    parser.process(app);
    if (parser.isSet("verify")) {
        const auto report = verify(parser.value("verify"));
        std::cout << report.json.dump(2) << '\n';
        return report.ok ? 0 : 2;
    }
    WriterConfig config;
    config.root = validateRoot(parser.value("root"));
    config.symbol = parser.value("symbol").toStdString();
    validateSymbol(config.symbol);
    config.blockBytes = number(parser, "block-bytes", 1, MaxRecordBytes);
    config.blockInterval = std::chrono::milliseconds(number(parser, "block-ms", 1, 60000));
    config.fsyncBlocks = number(parser, "fsync-blocks", 0, 1000000);
    config.compressionLevel = number(parser, "zstd-level", 1, 19);
    const auto queueBytes = size_t(number(parser, "queue-mib", 1, 1024)) * 1024 * 1024;
    const auto duration = number(parser, "duration", 0, 365 * 86400);
    const auto productDirectory = prepareDirectory(config.root + '/' + QString::fromStdString(config.symbol));
    QLockFile lock(productDirectory + "/.capture.lock");
    lock.setStaleLockTime(0);
    if (!lock.tryLock()) throw std::runtime_error("another capture owns this product directory");
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
    const auto metadata = rest.fetchProductMetadata(config.symbol);
    if (!metadata.ok) throw std::runtime_error("product metadata fetch failed (no connection opened): " + metadata.error);
    DecimalGrid quote(metadata.quoteIncrement), base(metadata.baseIncrement);
    if (stopSignal) return 0;
    const auto fetched = Stamp::now();
    nlohmann::json header = {{"tool", "sentinel-capture"}, {"tool_version", Sentinel::getFullVersionString()},
        {"build", Sentinel::getBuildInfo()}, {"product_metadata", metadata.metadata},
        {"metadata_source", "https://api.coinbase.com" + metadata.sourcePath},
        {"metadata_fetched_system_ns", fetched.systemNs}, {"queue_bytes", queueBytes},
        {"duration_seconds", duration}, {"ca_bundle", mdc.sslCaBundle},
        {"ws", {{"host", mdc.host}, {"port", mdc.port}, {"target", mdc.target}, {"use_jwt", mdc.useJwt}}},
        {"clock", "nanoseconds since system_clock/steady_clock epoch; sampled at engine pre-parse ingest seam"}};
    Session session(config, std::move(header), queueBytes);
    std::atomic<uint64_t> connection{0};
    std::string transportReason; // accessed only by the current engine I/O thread
    std::atomic<int64_t> disconnectedSince{fetched.steadyNs};
    const auto event = [&](Kind kind, const std::string& reason) noexcept {
        try { session.submit({kind, Stamp::now(), connection.load(), nlohmann::json({{"reason", reason}}).dump()}); }
        catch (const std::exception& e) { session.fail(e.what()); }
    };
    event(Kind::CaptureStarted, "capture started");
    std::unique_ptr<MarketDataCoreEngine> engine;
    const auto startEngine = [&] {
        engine = std::make_unique<MarketDataCoreEngine>(auth, mdc);
        engine->onIngest([&](const MarketDataCoreEngine::IngestObservation& observation) noexcept {
            try {
                using Ingest = MarketDataCoreEngine::IngestKind;
                Kind kind = Kind::Frame;
                switch (observation.kind) {
                case Ingest::Frame: kind = Kind::Frame; break;
                case Ingest::TransportUp:
                    kind = Kind::TransportUp; ++connection; disconnectedSince = 0; transportReason.clear(); break;
                case Ingest::TransportDown: {
                    kind = Kind::TransportDown;
                    int64_t zero = 0; disconnectedSince.compare_exchange_strong(zero, observation.steadyNs); break;
                }
                case Ingest::BookInvalidated: kind = Kind::BookInvalidated; break;
                case Ingest::ResyncRequested: kind = Kind::ResyncRequested; transportReason = observation.reason; break;
                }
                auto reason = observation.reason;
                if (kind == Kind::TransportUp) reason = "websocket connected";
                if (kind == Kind::TransportDown) reason = transportReason.empty() ? std::string_view("transport closed") : transportReason;
                auto payload = kind == Kind::Frame ? std::string(observation.payload) :
                    nlohmann::json({{"product", observation.product}, {"reason", reason}}).dump();
                session.submit({kind, {observation.systemNs, observation.steadyNs}, connection.load(), std::move(payload)});
            } catch (const std::exception& e) {
                session.fail(e.what(), RecordLocation{{observation.systemNs, observation.steadyNs}, connection.load(),
                    observation.kind == MarketDataCoreEngine::IngestKind::Frame ? Kind::Frame : Kind::EngineError});
            }
        });
        engine->onError([&](const std::string& error) noexcept {
            try { transportReason = error; event(Kind::EngineError, error); }
            catch (const std::exception& e) { session.fail(e.what()); }
        });
        // Stage products before starting the I/O thread (no cross-thread mutation).
        engine->subscribeToSymbols({config.symbol});
        engine->start();
    };
    startEngine();
    const auto started = Stamp::now().steadyNs;
    auto lastStats = started;
    bool stopped = false;
    std::string stopReason;
    QTimer timer;
    timer.setInterval(100);
    QObject::connect(&timer, &QTimer::timeout, &app, [&] {
        const auto now = Stamp::now().steadyNs;
        if (stopSignal || !session.error().empty() || (duration && now - started >= int64_t(duration) * 1000000000)) {
            stopReason = stopSignal ? "signal=" + std::to_string(stopSignal) :
                         !session.error().empty() ? "capture failure" : "duration elapsed";
            stopped = true; app.quit(); return;
        }
        // The shared engine handles sequence/heartbeat resync. A failed initial
        // handshake never starts its heartbeat watchdog; recover that stalled
        // engine here without altering sentinel-server's reconnect behavior.
        const auto down = disconnectedSince.load();
        if (down && now - down > 60LL * 1000000000) {
            engine->stop(); engine.reset();
            event(Kind::ResyncRequested, "capture supervisor: transport unavailable for 60 seconds");
            disconnectedSince = now;
            try { startEngine(); }
            catch (const std::exception& e) { session.fail(e.what()); app.quit(); return; }
        }
        if (now - lastStats >= 60LL * 1000000000) {
            const auto stats = session.stats();
            sLog_App("Capture stats: symbol=" << config.symbol << " frames=" << stats.frames
                << " receivedBytes=" << stats.frameBytes << " fileBytes=" << stats.fileBytes
                << " blocks=" << stats.blocks << " connections=" << connection.load()
                << " queuedBytes=" << session.queuedBytes());
            lastStats = now;
        }
    }, Qt::QueuedConnection);
    timer.start();
    sLog_App("Capture running: symbol=" << config.symbol << " root=" << config.root << " pid=" << QCoreApplication::applicationPid());
    app.exec();
    timer.stop();
    if (engine) { engine->stop(); engine.reset(); } // join the producer before draining the writer
    if (!stopped) stopReason = "application exit";
    session.close(stopReason);
    if (const auto error = session.error(); !error.empty()) {
        sLog_Error("Capture incomplete: error=" << error); return 1;
    }
    const auto stats = session.stats();
    sLog_App("Capture closed: reason=" << stopReason << " frames=" << stats.frames << " fileBytes=" << stats.fileBytes);
    return 0;
}
} // namespace sentinel::capture
