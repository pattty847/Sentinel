#include <gtest/gtest.h>
#include "capture/RawCapture.hpp"
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
    while (!output.contains("FIXTURE_READY\n") && deadline.elapsed() < 5000 && child.state() != QProcess::NotRunning) {
        child.waitForReadyRead(100);
        output += child.readAllStandardOutput();
    }
    ASSERT_TRUE(output.contains("FIXTURE_READY\n")) << child.readAllStandardError().toStdString();
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
