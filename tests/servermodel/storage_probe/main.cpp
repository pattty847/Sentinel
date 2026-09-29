#include "Encoders.hpp"
#include "protocol/SentinelStreamClient.hpp"
#include "SentinelLogging.hpp"
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QSaveFile>
#include <QTimer>
#include <nlohmann/json.hpp>
#include <iostream>

using namespace storage_probe;
using Json = nlohmann::json;
namespace {
Json counters(const Stats& s, double seconds) {
    return {{"encoded_bytes", s.bytes}, {"compressed_payload_bytes", s.payloadBytes},
        {"uncompressed_record_bytes", s.rawBytes}, {"blocks", s.blocks},
        {"keyframes", s.keyframes}, {"delta_columns", s.deltas}, {"entries_or_messages", s.entries},
        {"observed_ms", s.observedMs}, {"partial_columns", s.partialColumns},
        {"bytes_per_hour", seconds > 0 ? Json(s.bytes * 3600.0 / seconds) : Json(nullptr)},
        {"bytes_per_day", seconds > 0 ? Json(s.bytes * 86400.0 / seconds) : Json(nullptr)}};
}
struct Measurement {
    RawEncoder raw;
    std::array<Columns, 4> columns;
    std::array<double, 5> cpu{};
    uint64_t snapshots = 0, messages = 0, levels = 0;
    explicit Measurement(bool peak) : columns{Columns(100, peak), Columns(1000, peak),
        Columns(10'000, peak), Columns(60'000, peak)} {}
    template<class F> void timed(size_t i, F&& f) {
        const auto begin = threadCpuSeconds(); f(); cpu[i] += threadCpuSeconds() - begin;
    }
    void event(const Event& e) {
        ++messages; levels += e.levels.size(); if (e.kind == Kind::Snapshot) ++snapshots;
        timed(0, [&] { raw.add(e); });
        for (size_t i = 0; i < 4; ++i) timed(i + 1, [&] { columns[i].add(e); });
    }
    void tick(int64_t t) {
        for (size_t i = 0; i < 4; ++i) timed(i + 1, [&] { columns[i].advance(t); });
    }
    void finish(int64_t t) {
        timed(0, [&] { raw.flush(); });
        for (size_t i = 0; i < 4; ++i) timed(i + 1, [&] { columns[i].finish(t); });
    }
    Json report(double seconds) const {
        auto out = Json::array();
        auto r = counters(raw.stats(), seconds);
        r["name"] = "raw_local_l2"; r["cpu_seconds"] = cpu[0]; out.push_back(r);
        constexpr std::array<int, 4> tfs{100, 1000, 10'000, 60'000};
        for (size_t i = 0; i < 4; ++i) {
            const auto& c = columns[i];
            const auto bytes = c.codecs()[0].stats().bytes + c.codecs()[1].stats().bytes;
            out.push_back({{"name", "twap"}, {"base_ms", tfs[i]}, {"cpu_seconds", cpu[i + 1]},
                {"encoded_bytes", bytes}, {"bytes_per_hour", seconds > 0 ? Json(bytes * 3600.0 / seconds) : Json(nullptr)},
                {"bytes_per_day", seconds > 0 ? Json(bytes * 86400.0 / seconds) : Json(nullptr)},
                {"near", counters(c.codecs()[0].stats(), seconds)},
                {"deep", counters(c.codecs()[1].stats(), seconds)},
                {"invalidations", c.invalidations()}, {"ignored_deltas", c.ignoredDeltas()}});
        }
        return out;
    }
};
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("storage_probe");
    QCommandLineParser parser;
    parser.setApplicationDescription("Research-only BTC-USD storage simulation; local stream is not exchange raw L2.");
    parser.addHelpOption();
    parser.addOptions({{"minutes", "Duration after first snapshot (0 < N <= 1440).", "N", "5"},
        {"out", "JSON result path.", "path"}, {"ca", "Server certificate.", "path", "certs/sentinel-server.crt"},
        {"no-peak", "Encode TWAP only (peak field retained as zero)."},
        {"synthetic", "Deterministic offline fixture; never a market size estimate."}});
    parser.process(app);
    bool ok = false;
    const double minutes = parser.value("minutes").toDouble(&ok);
    if (!ok || !std::isfinite(minutes) || minutes <= 0 || minutes > 1440 || parser.value("out").isEmpty())
        parser.showHelp(2);
    const auto duration = int64_t(std::llround(minutes * 60000));
    if (duration < 1) parser.showHelp(2);
    const bool synthetic = parser.isSet("synthetic"), peak = !parser.isSet("no-peak");
    Measurement measurement(peak);
    int64_t start = 0, end = 0;
    std::string error;
    uint64_t maxTimerDelay = 0;
    bool finalized = false;
    auto makeReport = [&](const char* status) {
        const double seconds = std::max<int64_t>(0, end - start) / 1000.0;
        return Json{{"schema", 1}, {"checkpoint_utc_ms", QDateTime::currentMSecsSinceEpoch()},
            {"checkpoint_interval_ms", 60000}, {"finalized", finalized},
            {"pending_raw_bytes", measurement.raw.pendingBytes()}, {"status", status}, {"error", error},
            {"source", synthetic ? "synthetic_fixture" : "sentinel_local_stream"}, {"symbol", "BTC-USD"},
            {"clock", synthetic ? "deterministic_fixture_ms" : "monotonic_elapsed_anchored_to_first_snapshot_dequeue_utc_ms"},
            {"upstream_sequence_and_validity_available", false}, {"peak", peak},
            {"simulation", "same events interleaved on one thread; each TWAP encoder maintains its own book"},
            {"byte_accounting", "compressed frames + modeled stream/file headers; no disk writes/fsync/index"},
            {"cpu_accounting", "thread CPU; book maintenance + integration + serialization + zstd; excludes network/JSON"},
            {"zstd_level", 3}, {"raw_block_target_ms", 1000}, {"raw_block_soft_cap_bytes", 1048576},
            {"column_keyframe_interval_ms", 900000}, {"requested_minutes", minutes},
            {"start_ms", start}, {"end_ms", end}, {"measured_seconds", seconds},
            {"max_timer_delay_ms", maxTimerDelay}, {"messages", measurement.messages},
            {"snapshots", measurement.snapshots}, {"levels", measurement.levels},
            {"encoders", measurement.report(seconds)},
            {"limitations", {"Local stream uses server indexed book and float quantities; not full-depth exchange L2.",
                "Upstream gaps/resync and malformed messages may be invisible to this client.",
                "Short-run extrapolation includes startup snapshot/keyframe and final partial bucket.",
                "HMC2-style log means are lossy; this does not measure an exact-sums format.",
                "Synthetic results are fixture measurements, not BTC storage forecasts."}}};
    };
    auto writeResult = [&](const char* status, bool progress) {
        auto result = makeReport(status);
        QSaveFile output(parser.value("out"));
        const auto json = result.dump(2) + "\n";
        if (!output.open(QIODevice::WriteOnly) ||
            output.write(json.data(), qint64(json.size())) != qint64(json.size()) || !output.commit())
            throw std::runtime_error("cannot atomically save result: " + parser.value("out").toStdString());
        if (progress) {
            std::string line = "storage_probe progress: elapsed_ms=" + std::to_string(end - start) +
                " events=" + std::to_string(measurement.messages) + " status=" + status;
            for (const auto& encoder : result["encoders"]) {
                const auto name = encoder["name"].get<std::string>() +
                    (encoder.contains("base_ms") ? "_" + std::to_string(encoder["base_ms"].get<int>()) : "");
                line += " " + name + "_bytes=" + std::to_string(encoder["encoded_bytes"].get<uint64_t>());
            }
            sLog_App(line); // Qt's default handler writes stderr; one line per minute.
        }
        return result;
    };
    int64_t nextCheckpoint = 60000;
    auto checkpoint = [&] {
        if (start && end - start >= nextCheckpoint) {
            // Do not flush/finalize encoders here: checkpoints must not change
            // block boundaries or double-count the open column on continuation.
            writeResult("partial", true);
            nextCheckpoint = ((end - start) / 60000 + 1) * 60000;
        }
    };
    auto guard = [&](auto&& operation) {
        try { operation(); } catch (const std::exception& e) { error = e.what(); app.exit(1); }
    };
    try {
        writeResult("starting", false); // Check the output path before opening a socket.
        if (synthetic) {
            start = recording::kHmc2MinMs;
            std::vector<Level> snapshot;
            // 4000 price levels: sparse near band plus wide deep walls.
            for (int i = 0; i < 2000; ++i) {
                snapshot.push_back({true, 60'000.0 - i * 10, .01 * (1 + i % 23)});
                snapshot.push_back({false, 60'002.0 + i * 50, .01 * (1 + i % 31)});
            }
            measurement.event({start, Kind::Snapshot, snapshot});
            end = start;
            writeResult("partial", false);
            for (int64_t t = 20; t < duration; t += 20) {
                const int k = int((t / 20) % 2000);
                end = start + t;
                measurement.event({end, Kind::Delta,
                    {{true, 60'000.0 - k * 10, .01 * (1 + (t / 20) % 43)}}});
                checkpoint();
            }
            end = start + duration;
        } else {
            const auto cert = parser.value("ca");
            // Validate with the same TLS library before the client's dev-mode
            // fallback could silently disable peer verification.
            boost::asio::ssl::context certCheck(boost::asio::ssl::context::tlsv13_client);
            certCheck.load_verify_file(cert.toStdString());
            SentinelStreamClient client("127.0.0.1", "8080", cert.toStdString());
            QElapsedTimer elapsed;
            QTimer timer, startup;
            timer.setTimerType(Qt::PreciseTimer); timer.setInterval(25);
            startup.setSingleShot(true); startup.setInterval(15000);
            auto now = [&] { return start + std::min<int64_t>(elapsed.elapsed(), duration); };
            auto accept = [&](Kind kind, std::vector<Level> levels) {
                if (!error.empty()) return;
                guard([&] {
                    const bool firstSnapshot = !start;
                    if (!start) {
                        if (kind != Kind::Snapshot) return;
                        start = QDateTime::currentMSecsSinceEpoch(); elapsed.start(); startup.stop(); timer.start();
                    }
                    end = now();
                    if (end < start + duration) measurement.event({end, kind, std::move(levels)});
                    else app.quit();
                    if (firstSnapshot) writeResult("partial", false);
                    checkpoint();
                });
            };
            QObject::connect(&client, &SentinelStreamClient::connected, &app,
                [&] { client.subscribe("BTC-USD"); }, Qt::QueuedConnection);
            QObject::connect(&client, &SentinelStreamClient::snapshotReceived, &app,
                [&](const QString& symbol, const auto& bids, const auto& asks) {
                    if (symbol != "BTC-USD") return;
                    if (bids.empty() || asks.empty()) {
                        error = "server snapshot is not two-sided; warm the server book and rerun";
                        app.exit(1);
                        return;
                    }
                    std::vector<Level> levels; levels.reserve(bids.size() + asks.size());
                    for (auto l : bids) levels.push_back({true, l.price, l.size});
                    for (auto l : asks) levels.push_back({false, l.price, l.size});
                    accept(Kind::Snapshot, std::move(levels));
                }, Qt::QueuedConnection);
            QObject::connect(&client, &SentinelStreamClient::l2UpdateReceived, &app,
                [&](const QString& symbol, const auto& updates) {
                    if (symbol != "BTC-USD") return;
                    std::vector<Level> levels; levels.reserve(updates.size());
                    for (auto l : updates) levels.push_back({l.isBid, l.price, l.quantity});
                    accept(Kind::Delta, std::move(levels));
                }, Qt::QueuedConnection);
            auto fail = [&](const std::string& message) {
                error = message;
                if (start) { end = now(); guard([&] { measurement.event({end, Kind::Invalid, {}}); }); }
                app.exit(1);
            };
            QObject::connect(&client, &SentinelStreamClient::errorOccurred, &app,
                [&](const QString& e) { fail(e.toStdString()); }, Qt::QueuedConnection);
            QObject::connect(&client, &SentinelStreamClient::disconnected, &app,
                [&] { fail("stream disconnected"); }, Qt::QueuedConnection);
            QObject::connect(&startup, &QTimer::timeout, &app, [&] { fail("no snapshot within 15 seconds"); });
            int64_t lastTick = 0;
            QObject::connect(&timer, &QTimer::timeout, &app, [&] { guard([&] {
                end = now();
                const auto t = elapsed.elapsed();
                maxTimerDelay = std::max(maxTimerDelay, uint64_t(std::max<int64_t>(0, t - lastTick - 25)));
                lastTick = t;
                measurement.tick(end);
                checkpoint();
                if (end >= start + duration) app.quit();
            }); });
            sLog_App("storage_probe: sampling local parsed stream; sequence/upstream validity unavailable");
            startup.start(); client.connectToServer(); app.exec(); client.disconnectFromServer();
        }
        measurement.finish(end);
        finalized = true;
    } catch (const std::exception& e) { error = e.what(); }
    try {
        auto result = writeResult(error.empty() ? "complete" : "failed", true);
        std::cout << result.dump(2) << '\n';
    } catch (const std::exception& e) {
        sLog_Error("storage_probe: " << e.what());
        return 1;
    }
    return error.empty() ? 0 : 1;
}
