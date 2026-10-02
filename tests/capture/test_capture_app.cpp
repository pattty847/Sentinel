#include <gtest/gtest.h>
#include "capture/RawCapture.hpp"
#include "capture/CaptureSession.hpp"
#include "../marketdata/fixtures/coinbase_messages.hpp"
#include "capture/CaptureVerifier.hpp"
#include <QDirIterator>
#include <QElapsedTimer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <map>
#include <set>
#ifndef _WIN32
#include <csignal>
#include <unistd.h>
#endif

using namespace sentinel::capture;
TEST(CaptureApplication, SigtermDrainsAndSealsAfterReconnectWithRealConnectionIds) {
#ifdef _WIN32
    GTEST_SKIP() << "POSIX SIGTERM integration test";
#else
    QTemporaryDir dir;
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
    child.start(CAPTURE_APP_FIXTURE, {"--root", dir.path() + "/raw", "--symbol", "BTC-USD",
        "--ca-bundle", SENTINEL_TEST_CA, "--key-file", dir.path() + "/absent-key.json",
        "--block-ms", "60000"}); // SIGTERM must flush the unfinished block
    ASSERT_TRUE(child.waitForStarted(5000)) << child.errorString().toStdString();
    QByteArray output;
    QElapsedTimer deadline; deadline.start();
    while (!output.contains("FIXTURE_READY\n") && deadline.elapsed() < 35000 && child.state() != QProcess::NotRunning) {
        child.waitForReadyRead(100);
        output += child.readAllStandardOutput();
    }
    ASSERT_TRUE(output.contains("FIXTURE_READY\n")) << child.readAllStandardError().toStdString();
    EXPECT_FALSE(output.contains("FIXTURE_EARLY_FRAME\n"));
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
        QTemporaryDir dir;
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
        args << "--root" << dir.path() + "/raw" << "--ca-bundle" << SENTINEL_TEST_CA
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
        // Wait for the disk thread to publish a readable block (read-only).
        QElapsedTimer flushDeadline; flushDeadline.start();
        while (flushDeadline.elapsed() < 10000) {
            QDirIterator files(dir.path() + "/raw", {"*.rawl2"}, QDir::Files, QDirIterator::Subdirectories);
            int readable = 0;
            while (files.hasNext()) {
                try { if (!scan(files.next()).index.empty()) ++readable; } catch (...) { }
            }
            if (readable >= 7) break;
            child.waitForReadyRead(50);
        }
        const auto live = verify(dir.path() + "/raw");
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
    QTemporaryDir dir;
    QProcess child;
    struct Cleanup { QProcess& p; ~Cleanup() { if (p.state() != QProcess::NotRunning) { p.kill(); p.waitForFinished(5000); } } } cleanup{child};
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("SENTINEL_LOG_DIR", dir.path() + "/logs");
    environment.insert("SENTINEL_FIXTURE_UNEVEN", "1");
    child.setProcessEnvironment(environment);
    const auto root = dir.path() + "/raw";
    child.start(CAPTURE_APP_FIXTURE, {"--root", root, "--symbols", "BTC-USD,ETH-USD",
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
    Session legacy(std::move(products));
    EXPECT_TRUE(legacy.submit({Kind::CaptureStarted, Stamp::now(), 0, "{}"}));
    EXPECT_TRUE(legacy.submit({Kind::TransportUp, Stamp::now(), 1, "{}"}));
    uint64_t sequence = 0;
    for (const auto* symbol : {"BTC-USD", "ETH-USD"}) {
        auto snapshot = fixtures::coinbaseL2Snapshot(symbol, {{100, 1}}, {{101, 2}});
        snapshot["sequence_num"] = sequence++;
        EXPECT_TRUE(legacy.submit({Kind::Frame, Stamp::now(), 1, snapshot.dump()}));
    }
    legacy.close(); ASSERT_TRUE(legacy.error().empty()) << legacy.error();
    const auto mixed = verify(root); EXPECT_TRUE(mixed.ok) << mixed.json.dump(2);
    EXPECT_TRUE(mixed.json["complete"]); EXPECT_EQ(mixed.json["products"].size(), 2u);
    std::set<int> versions;
    QDirIterator allFiles(root, {"*.rawl2"}, QDir::Files, QDirIterator::Subdirectories);
    while (allFiles.hasNext()) versions.insert(readHeader(allFiles.next())["format_version"].get<int>());
    EXPECT_EQ(versions, (std::set<int>{1, 2}));
    verifyCli();
#endif
}
