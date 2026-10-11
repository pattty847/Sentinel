#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHostAddress>
#include <QNetworkInterface>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTcpServer>
#include <QTemporaryDir>
#include <gtest/gtest.h>

namespace {
// Run the real entry point from scratch config/storage, with an offline feed.
// Use scratch roots and an absent fan-out socket so no live service is reached.
struct ServerProcess {
    QTemporaryDir scratch;
    QTcpServer healthReservation;
    QProcess process;

    ~ServerProcess() {
        if (process.state() != QProcess::NotRunning) {
            process.terminate();
            if (!process.waitForFinished(3000)) {
                process.kill();
                process.waitForFinished(3000);
            }
        }
    }

    bool start(const QByteArray& address, bool prepareRecording = false) {
        if (!scratch.isValid() || !healthReservation.listen(QHostAddress::LocalHost, 0)) return false;
        QDir root(scratch.path());
        if (!root.mkdir("config") || !root.mkdir("logs")) return false;
        if (prepareRecording && !root.mkdir("recording")) return false;
        QFile config(scratch.filePath("config/server_config.yaml"));
        if (!config.open(QIODevice::WriteOnly)) return false;
        const QByteArray yaml =
            "bind_address: '" + address + "'\n"
            "stream_port: 0\n"
            "default_symbols: [BTC-USD]\n"
            "recording:\n  enabled: true\n  source: roller\n  dir: '" + scratch.filePath("primary").toUtf8() + "'\n  fallback_dir: ''\n"
            "roller_shadow:\n  enabled: true\n  dir: '" + scratch.filePath("recording").toUtf8() + "'\n"
            "  journal_dir: '" + scratch.filePath("journal").toUtf8() + "'\n"
            "  socket: '" + scratch.filePath("absent.sock").toUtf8() + "'\n  products: [BTC-USD]\n"
            "ssl_ca_bundle: '" + QByteArray(SENTINEL_SOURCE_DIR) + "/resources/certs/ca-bundle.crt'\n"
            "tls:\n  cert_file: missing-cert.pem\n  key_file: missing-key.pem\n";
        if (config.write(yaml) != yaml.size()) return false;
        config.close();
        auto env = QProcessEnvironment::systemEnvironment();
        env.insert("SENTINEL_HEALTH_PORT", QString::number(healthReservation.serverPort()));
        env.insert("SENTINEL_LOG_DIR", scratch.filePath("logs"));
        env.insert("SENTINEL_LOG_FILE", "1");
        process.setProcessEnvironment(env);
        process.setWorkingDirectory(scratch.path());
        process.start(QString::fromUtf8(SENTINEL_SERVER_BINARY), {"--require-recording"});
        return process.waitForStarted(3000);
    }

    QByteArray log() const {
        QFile file(scratch.filePath("logs/sentinel-server-latest.log"));
        if (!file.open(QIODevice::ReadOnly)) return {};
        return file.readAll();
    }
};
} // namespace

TEST(ServerStartup, InvalidBindAddressExitsBeforeRecordingOrFeeds) {
    int argc = 1; char name[] = "startup-invalid"; char* argv[] = {name, nullptr};
    QCoreApplication app(argc, argv);
    ASSERT_FALSE(QNetworkInterface::allAddresses().contains(QHostAddress("192.0.2.1")));
    for (const auto* address : {"not-an-address", "", "192.0.2.1"}) {
        SCOPED_TRACE(address);
        ServerProcess server;
        ASSERT_TRUE(server.start(address));
        ASSERT_TRUE(server.process.waitForFinished(5000));
        EXPECT_EQ(server.process.exitStatus(), QProcess::NormalExit);
        EXPECT_EQ(server.process.exitCode(), 1);
        const auto log = server.log();
        EXPECT_TRUE(log.contains("exe=" + QByteArray(SENTINEL_SERVER_BINARY) + " built="));
        EXPECT_TRUE(log.contains(" E app ")) << log.constData();
        EXPECT_TRUE(log.contains("Invalid server.bind_address: address=")) << log.constData();
        EXPECT_FALSE(log.contains("Initializing server components")) << log.constData();
        EXPECT_FALSE(QDir(server.scratch.filePath("recording")).exists());
    }
}

TEST(ServerStartup, MissingTlsLeavesTheRequiredRecorderRunning) {
    int argc = 1; char name[] = "startup-no-tls"; char* argv[] = {name, nullptr};
    QCoreApplication app(argc, argv);
    ServerProcess server;
    ASSERT_TRUE(server.start("127.0.0.1", true));
    QElapsedTimer timer;
    timer.start();
    while (!server.log().contains("Sentinel Server running") && timer.elapsed() < 5000 &&
           server.process.state() != QProcess::NotRunning) {
        server.process.waitForReadyRead(20);
        QCoreApplication::processEvents();
    }
    const auto log = server.log();
    EXPECT_TRUE(log.contains("exe=" + QByteArray(SENTINEL_SERVER_BINARY) + " built="));
    EXPECT_TRUE(log.contains(" E app ")) << log.constData();
    EXPECT_TRUE(log.contains("SentinelStreamServer start failed")) << log.constData();
    EXPECT_TRUE(log.contains("Recording served by the journal roller: dir=")) << log.constData();
    EXPECT_FALSE(log.contains("mdcHost=")) << log.constData();
    EXPECT_FALSE(log.contains("Server default subscribe")) << log.constData();
    EXPECT_TRUE(log.contains("Live feed: journal")) << log.constData();
    EXPECT_TRUE(log.contains("Sentinel Server running")) << log.constData();
    EXPECT_TRUE(QDir(server.scratch.filePath("recording")).exists());
    EXPECT_FALSE(server.process.waitForFinished(200)) << server.process.readAllStandardError().constData();
    EXPECT_EQ(server.process.state(), QProcess::Running);
}
