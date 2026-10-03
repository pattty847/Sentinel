#include <gtest/gtest.h>
#include "capture/RawCapture.hpp"
#include "capture/CaptureSession.hpp"
#include "../marketdata/fixtures/coinbase_messages.hpp"
#include "capture/CaptureVerifier.hpp"
#include "legacy_v2_fixture.hpp"
#include <QCoreApplication>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QProcess>
#include <QLocalSocket>
#include <QProcessEnvironment>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <map>
#include <set>
#ifndef _WIN32
#include <csignal>
#include <unistd.h>
#include <sys/stat.h>
#endif

using namespace sentinel::capture;
namespace {
void ensureApplication() {
    static int argc = 1;
    static char name[] = "test_capture_app";
    static char* argv[] = {name, nullptr};
    if (!QCoreApplication::instance()) static QCoreApplication app(argc, argv);
}
uint16_t freePort() {
    ensureApplication();
    QTcpServer probe;
    if (!probe.listen(QHostAddress::LocalHost, 0)) return 0;
    return probe.serverPort();
}
std::string scrape(uint16_t port) {
    ensureApplication();
    QTcpSocket client;
    client.connectToHost(QHostAddress::LocalHost, port);
    if (!client.waitForConnected(3000)) return "connect failed: " + client.errorString().toStdString();
    client.write("GET /metrics HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
    QByteArray response;
    QElapsedTimer timer; timer.start();
    while (timer.elapsed() < 3000 && client.state() != QAbstractSocket::UnconnectedState) {
        client.waitForReadyRead(50);
        response += client.readAll();
    }
    response += client.readAll();
    if (!response.startsWith("HTTP/1.1 200 OK\r\n")) return "bad response: " + response.toStdString();
    return response.mid(response.indexOf("\r\n\r\n") + 3).toStdString(); // keep the leading '\n'
}
} // namespace

TEST(CaptureApplication, FloorsLargerThanThePoolRefuseToStart) {
    QTemporaryDir dir(QDir::tempPath() + "/sca-XXXXXX");
    QProcess child;
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("SENTINEL_LOG_DIR", dir.path() + "/logs");
    environment.insert("SENTINEL_LOG_STDERR", "1");
    child.setProcessEnvironment(environment);
    child.start(CAPTURE_APP_FIXTURE, {"--fanout-socket", dir.path() + "/capture.sock", "--root", dir.path() + "/raw", "--symbols", "BTC-USD,ETH-USD",
        "--queue-mib", "3", "--queue-floor-mib", "2", "--ca-bundle", SENTINEL_TEST_CA});
    ASSERT_TRUE(child.waitForFinished(10000));
    EXPECT_EQ(child.exitCode(), 1);
    EXPECT_TRUE(child.readAllStandardError().contains("--queue-floor-mib x products exceeds --queue-mib"));
    EXPECT_FALSE(QFileInfo::exists(dir.path() + "/raw/BTC-USD")); // refused before any directory or connection
}

TEST(CaptureApplication, SigtermDrainsAndSealsAfterReconnectWithRealConnectionIds) {
#ifdef _WIN32
    GTEST_SKIP() << "POSIX SIGTERM integration test";
#else
    QTemporaryDir dir(QDir::tempPath() + "/sca-XXXXXX");
    ASSERT_TRUE(dir.isValid());
    QProcess child;
    struct Cleanup {
        QProcess& child;
        ~Cleanup() { if (child.state() != QProcess::NotRunning) { child.kill(); child.waitForFinished(5000); } }
    } cleanup{child};
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("SENTINEL_LOG_DIR", dir.path() + "/logs");
    environment.insert("SENTINEL_LOG_FILE", "1");
    environment.insert("SENTINEL_LOG_STDERR", "0");
    child.setProcessEnvironment(environment);
    const auto metricsPort = freePort();
    ASSERT_NE(metricsPort, 0);
    child.start(CAPTURE_APP_FIXTURE, {"--fanout-socket", dir.path() + "/capture.sock", "--root", dir.path() + "/raw", "--symbol", "BTC-USD",
        "--ca-bundle", SENTINEL_TEST_CA, "--key-file", dir.path() + "/absent-key.json",
        "--block-ms", "60000", // SIGTERM must flush the unfinished block
        "--metrics-port", QString::number(metricsPort)});
    ASSERT_TRUE(child.waitForStarted(5000)) << child.errorString().toStdString();
    QByteArray output;
    QElapsedTimer deadline; deadline.start();
    while (!output.contains("FIXTURE_READY\n") && deadline.elapsed() < 35000 && child.state() != QProcess::NotRunning) {
        child.waitForReadyRead(100);
        output += child.readAllStandardOutput();
    }
    ASSERT_TRUE(output.contains("FIXTURE_READY\n")) << child.readAllStandardError().toStdString();
    EXPECT_FALSE(output.contains("FIXTURE_EARLY_FRAME\n"));
    // The running capture serves its own /metrics on 127.0.0.1 (slice 2 scrape target).
    const auto metrics = scrape(metricsPort);
    EXPECT_NE(metrics.find("\nsentinel_capture_feed_up{product=\"BTC-USD\"} 1\n"), std::string::npos) << metrics;
    EXPECT_NE(metrics.find("\nsentinel_capture_connection{product=\"BTC-USD\"} 2\n"), std::string::npos) << metrics;
    EXPECT_NE(metrics.find("\nsentinel_capture_feed_down_seconds{product=\"BTC-USD\"} 0\n"), std::string::npos) << metrics;
    EXPECT_NE(metrics.find("\nsentinel_capture_queue_pool_bytes 536870912\n"), std::string::npos) << metrics; // 512 MiB default
    EXPECT_NE(metrics.find("\nsentinel_capture_queue_floor_bytes 2097152\n"), std::string::npos) << metrics;
    EXPECT_NE(metrics.find("\nsentinel_capture_stored_frames_total{product=\"BTC-USD\"} "), std::string::npos) << metrics;
    EXPECT_NE(metrics.find("\nprocess_resident_memory_bytes "), std::string::npos) << metrics;
    QLocalSocket fanout; fanout.connectToServer(dir.path() + "/capture.sock");
    ASSERT_TRUE(fanout.waitForConnected(3000));
    fanout.write("{\"type\":\"hello\",\"version\":1,\"product\":\"BTC-USD\"}\n");
    fanout.waitForBytesWritten(1000);
    ASSERT_TRUE(fanout.waitForReadyRead(3000));
    EXPECT_NE(scrape(metricsPort).find("sentinel_fanout_clients 1\n"), std::string::npos);
    ASSERT_EQ(::kill(static_cast<pid_t>(child.processId()), SIGTERM), 0);
    ASSERT_TRUE(child.waitForFinished(5000));
    ASSERT_EQ(child.exitStatus(), QProcess::NormalExit) << child.readAllStandardError().toStdString();
    ASSERT_EQ(child.exitCode(), 0) << child.readAllStandardError().toStdString();

    const auto report = verify(dir.path() + "/raw/BTC-USD");
    ASSERT_TRUE(report.ok) << report.json.dump(2);
    EXPECT_EQ(report.json["connections"], 2);
    EXPECT_EQ(report.json["reconnects"], 1);
    EXPECT_EQ(report.json["capture_stops"], 1);
    EXPECT_EQ(report.json["explicit_capture_gaps"], 0);
    EXPECT_EQ(report.json["snapshots"], 2);
    EXPECT_EQ(report.json["empty_connections"], 0);
    std::set<uint64_t> frameConnections;
    uint64_t currentConnection = 0;
    int stopMarkers = 0;
    QDirIterator files(dir.path() + "/raw", {"*.rawl2"}, QDir::Files, QDirIterator::Subdirectories);
    std::map<uint64_t, QString> segments;
    while (files.hasNext()) {
        const auto path = files.next();
        segments.emplace(readHeader(path).at("segment").get<uint64_t>(), path);
    }
    for (const auto& [ordinal, path] : segments) {
        const auto result = scan(path, [&](const Record& record) {
            if (record.kind == Kind::TransportUp) EXPECT_EQ(record.connection, ++currentConnection);
            if (record.kind == Kind::Frame) {
                EXPECT_EQ(record.connection, currentConnection); // up was persisted first
                frameConnections.insert(record.connection);
                const auto json = nlohmann::json::parse(record.payload);
                if (json["channel"] == "l2_data") {
                    EXPECT_TRUE(record.payload.starts_with(" \n"));
                    EXPECT_TRUE(record.payload.ends_with('\t')); // exact pre-parse text
                }
            }
            if (record.kind == Kind::CaptureStopped) {
                ++stopMarkers;
                EXPECT_EQ(record.connection, 2);
                EXPECT_EQ(nlohmann::json::parse(record.payload)["reason"], "signal=15");
            }
        });
        EXPECT_TRUE(result.indexed); EXPECT_FALSE(result.tornTail);
    }
    EXPECT_EQ(frameConnections, (std::set<uint64_t>{1, 2}));
    EXPECT_EQ(stopMarkers, 1);
    QFile log(dir.path() + "/logs/sentinel-capture-latest.log");
    ASSERT_TRUE(log.open(QIODevice::ReadOnly));
    EXPECT_TRUE(log.readAll().contains("Capture closed: reason=signal=15"));
#endif
}

TEST(CaptureApplication, SeveralCliFormsUseSevenConnectionsAndVerifyIndependentLiveRuns) {
#ifdef _WIN32
    GTEST_SKIP() << "POSIX SIGTERM integration test";
#else
    const QStringList symbols{"BTC-USD", "ETH-USD", "SOL-USD", "FARTCOIN-USD", "PEPE-USD", "DOGE-USD", "AVAX-USD"};
    QStringList repeated;
    for (const auto& symbol : symbols) repeated << "--symbol" << symbol;
    const std::vector<QStringList> forms{repeated, {"--symbols", symbols.join(',')},
        {"--symbol", "BTC-USD", "--symbols", symbols.join(',')}}; // duplicate is subscribed only once
    for (const auto& form : forms) {
        QTemporaryDir dir(QDir::tempPath() + "/sca-XXXXXX");
        QProcess child;
        struct Cleanup {
            QProcess& child;
            ~Cleanup() { if (child.state() != QProcess::NotRunning) { child.kill(); child.waitForFinished(5000); } }
        } cleanup{child};
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("SENTINEL_LOG_DIR", dir.path() + "/logs");
        environment.insert("SENTINEL_LOG_STDERR", "0");
        child.setProcessEnvironment(environment);
        auto args = form;
        args << "--fanout-socket" << dir.path() + "/capture.sock" << "--root" << dir.path() + "/raw" << "--ca-bundle" << SENTINEL_TEST_CA
             << "--key-file" << dir.path() + "/absent.json" << "--block-ms" << "1" << "--fsync-blocks" << "0";
        child.start(CAPTURE_APP_FIXTURE, args);
        ASSERT_TRUE(child.waitForStarted(5000));
        QByteArray output;
        QElapsedTimer deadline; deadline.start();
        while (!output.contains("FIXTURE_READY\n") && deadline.elapsed() < 35000 && child.state() != QProcess::NotRunning) {
            child.waitForReadyRead(100); output += child.readAllStandardOutput();
        }
        ASSERT_TRUE(output.contains("FIXTURE_READY\n")) << child.readAllStandardError().toStdString();
        EXPECT_FALSE(output.contains("FIXTURE_EARLY_FRAME\n"));
        EXPECT_EQ(output.count("FIXTURE_ENGINE\n"), 7);
        const auto lineStart = output.indexOf("FIXTURE_SENDS ") + 14;
        const auto lineEnd = output.indexOf('\n', lineStart);
        const auto sends = nlohmann::json::parse(output.mid(lineStart, lineEnd - lineStart).toStdString());
        ASSERT_EQ(sends.size(), 6); // three channels, two connection lifetimes, same transport
        std::set<std::string> expected;
        for (const auto& symbol : symbols) expected.insert(symbol.toStdString());
        std::map<std::string, int> channels;
        for (const auto& bytes : sends) {
            const auto message = nlohmann::json::parse(bytes.get<std::string>());
            EXPECT_EQ(message["type"], "subscribe");
            const auto channel = message["channel"].get<std::string>();
            ++channels[channel];
            if (channel == "heartbeats") EXPECT_FALSE(message.contains("product_ids"));
            else {
                EXPECT_EQ(message["product_ids"].size(), 1u);
                EXPECT_TRUE(expected.contains(message["product_ids"][0].get<std::string>()));
            }
        }
        EXPECT_EQ(channels, (std::map<std::string, int>{{"heartbeats", 2}, {"level2", 2}, {"market_trades", 2}}));
        // An open run is enough for live verification; a quiet product need
        // not have a sealed/indexed data block yet.
        QElapsedTimer flushDeadline; flushDeadline.start();
        const auto sevenOpen = [](const auto& report) {
            return report.ok && report.json.contains("totals") &&
                report.json["totals"]["open_runs"] == 7 && report.json["products"].size() == 7;
        };
        auto live = verify(dir.path() + "/raw");
        while (flushDeadline.elapsed() < 30000 && !sevenOpen(live)) {
            child.waitForReadyRead(100);
            live = verify(dir.path() + "/raw");
        }
        ASSERT_TRUE(sevenOpen(live))
            << "seven independent open runs were not visible within 30 s: " << live.json.dump(2);
        EXPECT_TRUE(live.ok) << live.json.dump(2);
        EXPECT_EQ(live.json["complete"], false); EXPECT_EQ(live.json["totals"]["open_runs"], 7);
        EXPECT_EQ(live.json["totals"]["incomplete_runs"], 7);
        EXPECT_EQ(live.json["totals"]["closed_runs"], 0);
        EXPECT_EQ(live.json["routing_checks_deferred"], 0);
        EXPECT_EQ(live.json["products"].size(), 7);
        QProcess liveVerifier;
        liveVerifier.setProcessEnvironment(environment);
        liveVerifier.start(CAPTURE_APP_FIXTURE, {"--verify", dir.path() + "/raw"});
        ASSERT_TRUE(liveVerifier.waitForFinished(10000));
        EXPECT_EQ(liveVerifier.exitCode(), 3) << liveVerifier.readAllStandardError().toStdString();
        ASSERT_EQ(::kill(static_cast<pid_t>(child.processId()), SIGTERM), 0);
        ASSERT_TRUE(child.waitForFinished(5000));
        ASSERT_EQ(child.exitCode(), 0) << child.readAllStandardError().toStdString();
        const auto report = verify(dir.path() + "/raw");
        EXPECT_TRUE(report.ok) << report.json.dump(2);
        EXPECT_EQ(report.json["totals"]["connections"], 14);
        EXPECT_EQ(report.json["totals"]["snapshots"], 14);
        EXPECT_EQ(report.json["routing_checks_deferred"], 0);
        for (const auto& product : report.json["products"]) {
            EXPECT_EQ(product["snapshots"], 2);
            EXPECT_EQ(product["sequence_gaps"], 0);
            EXPECT_EQ(product["unanchored_l2_events"], 0);
        }
        QProcess verifier;
        verifier.setProcessEnvironment(environment);
        verifier.start(CAPTURE_APP_FIXTURE, {"--verify", dir.path() + "/raw"});
        ASSERT_TRUE(verifier.waitForFinished(5000));
        EXPECT_EQ(verifier.exitCode(), 0);
        EXPECT_EQ(nlohmann::json::parse(verifier.readAllStandardOutput().toStdString())["totals"]["snapshots"], 14);
    }
#endif
}

TEST(CaptureApplication, UnevenReconnectsFailedAttemptAndAuditsVerifyAlongsideLegacyV2Run) {
#ifdef _WIN32
    GTEST_SKIP() << "POSIX SIGTERM integration test";
#else
    QTemporaryDir dir(QDir::tempPath() + "/sca-XXXXXX");
    QProcess child;
    struct Cleanup { QProcess& p; ~Cleanup() { if (p.state() != QProcess::NotRunning) { p.kill(); p.waitForFinished(5000); } } } cleanup{child};
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("SENTINEL_LOG_DIR", dir.path() + "/logs");
    environment.insert("SENTINEL_FIXTURE_UNEVEN", "1");
    child.setProcessEnvironment(environment);
    const auto root = dir.path() + "/raw";
    child.start(CAPTURE_APP_FIXTURE, {"--fanout-socket", dir.path() + "/capture.sock", "--root", root, "--symbols", "BTC-USD,ETH-USD",
        "--ca-bundle", SENTINEL_TEST_CA, "--key-file", dir.path() + "/absent.json"});
    ASSERT_TRUE(child.waitForStarted(5000));
    QByteArray output;
    QElapsedTimer deadline; deadline.start();
    while (!output.contains("FIXTURE_READY\n") && deadline.elapsed() < 15000 && child.state() != QProcess::NotRunning) {
        child.waitForReadyRead(100); output += child.readAllStandardOutput();
    }
    ASSERT_TRUE(output.contains("FIXTURE_READY\n")) << child.readAllStandardError().toStdString();
    ASSERT_EQ(::kill(static_cast<pid_t>(child.processId()), SIGTERM), 0);
    ASSERT_TRUE(child.waitForFinished(5000)); ASSERT_EQ(child.exitCode(), 0);
    auto verifyCli = [&] {
        QProcess verifier; verifier.setProcessEnvironment(environment);
        verifier.start(CAPTURE_APP_FIXTURE, {"--verify", root});
        EXPECT_TRUE(verifier.waitForFinished(5000));
        EXPECT_EQ(verifier.exitCode(), 0) << verifier.readAllStandardError().toStdString();
    };
    verifyCli(); // CLI must accept successful IDs after failed attempt 2
    const auto btc = verify(root + "/BTC-USD"), eth = verify(root + "/ETH-USD");
    ASSERT_TRUE(btc.ok) << btc.json.dump(2); ASSERT_TRUE(eth.ok) << eth.json.dump(2);
    EXPECT_EQ(btc.json["connections"], 2); EXPECT_EQ(btc.json["reconnects"], 1);
    EXPECT_EQ(eth.json["connections"], 3); EXPECT_EQ(eth.json["reconnects"], 2);
    EXPECT_EQ(btc.json["empty_connections"], 0); EXPECT_EQ(eth.json["empty_connections"], 1);
    EXPECT_EQ(eth.json["snapshots"], 2);
    EXPECT_EQ(btc.json["trades"], 2); EXPECT_EQ(eth.json["trades"], 2);
    EXPECT_EQ(btc.json["reconnect_trade_gaps"], 1); EXPECT_EQ(btc.json["reconnect_missing_trades"], 2);
    EXPECT_EQ(eth.json["reconnect_trade_gaps"], 0); EXPECT_EQ(eth.json["upstream_trade_gaps"], 0);
    verifyCli(); // actual per-product application output, failed attempt 2 between real connections
    QDirIterator v1Files(root, {"*.rawl2"}, QDir::Files, QDirIterator::Subdirectories);
    while (v1Files.hasNext()) EXPECT_EQ(readHeader(v1Files.next())["format_version"], 1);

    // Existing v2 layout with both products on one historical connection.
    std::vector<ProductCapture> products;
    for (const auto* symbol : {"BTC-USD", "ETH-USD"}) {
        WriterConfig config; config.root = root; config.symbol = symbol;
        products.push_back({config, {{"product_metadata", {{"product_id", symbol},
            {"quote_increment", "0.01"}, {"base_increment", "0.00000001"}}}}});
    }
    // Written by the test-only v2 writer: production writes v1 only.
    std::vector<Record> legacy{{Kind::CaptureStarted, Stamp::now(), 0, "{}"}, {Kind::TransportUp, Stamp::now(), 1, "{}"}};
    uint64_t sequence = 0;
    for (const auto* symbol : {"BTC-USD", "ETH-USD"}) {
        auto snapshot = fixtures::coinbaseL2Snapshot(symbol, {{100, 1}}, {{101, 2}});
        snapshot["sequence_num"] = sequence++;
        legacy.push_back({Kind::Frame, Stamp::now(), 1, snapshot.dump()});
    }
    legacy.push_back({Kind::CaptureStopped, Stamp::now(), 1, R"({"reason":"session closed"})"});
    ASSERT_NO_THROW(writeLegacyV2(std::move(products), legacy));
    const auto mixed = verify(root); EXPECT_TRUE(mixed.ok) << mixed.json.dump(2);
    EXPECT_TRUE(mixed.json["complete"]); EXPECT_EQ(mixed.json["products"].size(), 2u);
    std::set<int> versions;
    QDirIterator allFiles(root, {"*.rawl2"}, QDir::Files, QDirIterator::Subdirectories);
    while (allFiles.hasNext()) versions.insert(readHeader(allFiles.next())["format_version"].get<int>());
    EXPECT_EQ(versions, (std::set<int>{1, 2}));
    verifyCli();
#endif
}

TEST(CaptureApplication, FanoutResnapshotReachesOnlyRequestedEngine) {
#ifdef _WIN32
    GTEST_SKIP() << "Unix fanout";
#else
    QTemporaryDir dir(QDir::tempPath() + "/sca-XXXXXX");
    QProcess child;
    struct Cleanup { QProcess& p; ~Cleanup() { if (p.state() != QProcess::NotRunning) { p.kill(); p.waitForFinished(5000); } } } cleanup{child};
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("SENTINEL_LOG_DIR", dir.path() + "/logs");
    child.setProcessEnvironment(environment);
    const auto port = freePort();
    child.start(CAPTURE_APP_FIXTURE, {"--fanout-socket", dir.path()+"/capture.sock", "--root", dir.path()+"/raw",
        "--symbols", "BTC-USD,ETH-USD", "--ca-bundle", SENTINEL_TEST_CA, "--metrics-port", QString::number(port)});
    ASSERT_TRUE(child.waitForStarted(5000));
    QByteArray output; QElapsedTimer deadline; deadline.start();
    while (!output.contains("FIXTURE_READY\n") && deadline.elapsed() < 20000 && child.state()!=QProcess::NotRunning) {
        child.waitForReadyRead(50); output += child.readAllStandardOutput();
    }
    ASSERT_TRUE(output.contains("FIXTURE_READY\n")) << child.readAllStandardError().toStdString();
    QLocalSocket socket; socket.connectToServer(dir.path()+"/capture.sock"); ASSERT_TRUE(socket.waitForConnected(3000));
    socket.write("{\"type\":\"hello\",\"version\":1,\"product\":\"BTC-USD\"}\n{\"type\":\"resnapshot\",\"product\":\"BTC-USD\"}\n");
    socket.waitForBytesWritten(1000);
    deadline.restart(); std::string text;
    do { text=scrape(port); if(text.find("sentinel_capture_connection{product=\"BTC-USD\"} 3\n")!=std::string::npos) break;
        child.waitForReadyRead(50); } while(deadline.elapsed()<5000);
    EXPECT_NE(text.find("sentinel_capture_connection{product=\"BTC-USD\"} 3\n"),std::string::npos) << text;
    EXPECT_NE(text.find("sentinel_capture_connection{product=\"ETH-USD\"} 2\n"),std::string::npos) << text;
    ASSERT_EQ(::kill(static_cast<pid_t>(child.processId()),SIGTERM),0); ASSERT_TRUE(child.waitForFinished(5000)); EXPECT_EQ(child.exitCode(),0);
    bool resync=false; QDirIterator files(dir.path()+"/raw/BTC-USD",{"*.rawl2"},QDir::Files,QDirIterator::Subdirectories);
    while(files.hasNext()) scan(files.next(),[&](const Record& r){ if(r.kind==Kind::ResyncRequested) resync=true; });
    EXPECT_TRUE(resync);
#endif
}

TEST(CaptureApplication, FanoutUnavailableKeepsJournalingAndRecovers) {
#ifdef _WIN32
    GTEST_SKIP() << "Unix fanout";
#else
    QTemporaryDir dir(QDir::tempPath()+"/sca-XXXXXX");
    ASSERT_TRUE(QDir().mkdir(dir.path()+"/socket"));
    ASSERT_EQ(::chmod((dir.path()+"/socket").toStdString().c_str(),0755),0);
    QProcess child;
    struct Cleanup { QProcess& p; ~Cleanup(){if(p.state()!=QProcess::NotRunning){p.kill();p.waitForFinished(5000);}} } cleanup{child};
    auto environment=QProcessEnvironment::systemEnvironment();
    environment.insert("SENTINEL_LOG_DIR",dir.path()+"/logs"); environment.insert("SENTINEL_LOG_STDERR","1");
    child.setProcessEnvironment(environment);
    const auto port=freePort();
    child.start(CAPTURE_APP_FIXTURE,{"--fanout-socket",dir.path()+"/socket/capture.sock","--root",dir.path()+"/raw",
        "--symbols","BTC-USD,ETH-USD","--ca-bundle",SENTINEL_TEST_CA,"--metrics-port",QString::number(port)});
    ASSERT_TRUE(child.waitForStarted(5000));
    QByteArray output; QElapsedTimer deadline; deadline.start();
    while(!output.contains("FIXTURE_READY\n") && deadline.elapsed()<20000 && child.state()!=QProcess::NotRunning) {
        child.waitForReadyRead(50); output+=child.readAllStandardOutput();
    }
    ASSERT_TRUE(output.contains("FIXTURE_READY\n")) << child.readAllStandardError().toStdString();
    EXPECT_NE(scrape(port).find("sentinel_fanout_running 0\n"),std::string::npos);
    EXPECT_TRUE(child.readAllStandardError().contains("Capture fanout unavailable; journal continues:"));
    // Inspect completed blocks while capture is still running and the socket is down.
    for(const auto* product:{"BTC-USD","ETH-USD"}) {
        size_t frames=0; QDirIterator files(dir.path()+"/raw/"+product,{"*.rawl2"},QDir::Files,QDirIterator::Subdirectories);
        while(files.hasNext()) scan(files.next(),[&](const Record& r){if(r.kind==Kind::Frame)++frames;});
        EXPECT_GT(frames,0) << product;
    }
    ASSERT_EQ(::chmod((dir.path()+"/socket").toStdString().c_str(),0700),0);
    deadline.restart(); std::string metrics;
    do { metrics=scrape(port); if(metrics.find("sentinel_fanout_running 1\n")!=std::string::npos) break;
        child.waitForReadyRead(100); } while(deadline.elapsed()<35000 && child.state()!=QProcess::NotRunning);
    ASSERT_NE(metrics.find("sentinel_fanout_running 1\n"),std::string::npos) << metrics;
    QLocalSocket socket; socket.connectToServer(dir.path()+"/socket/capture.sock"); ASSERT_TRUE(socket.waitForConnected(3000));
    socket.write("{\"type\":\"hello\",\"version\":1,\"product\":\"BTC-USD\"}\n");socket.waitForBytesWritten(1000);
    EXPECT_TRUE(socket.waitForReadyRead(3000));
    ASSERT_EQ(::kill(static_cast<pid_t>(child.processId()),SIGTERM),0);
    ASSERT_TRUE(child.waitForFinished(5000)); EXPECT_EQ(child.exitCode(),0);
    for(const auto* product:{"BTC-USD","ETH-USD"}) {auto report=verify(dir.path()+"/raw/"+product); EXPECT_TRUE(report.ok)<<report.json.dump();}
#endif
}
