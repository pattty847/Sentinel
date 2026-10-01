#include <QCoreApplication>
#include <QDir>
#include "SentinelServerApp.hpp"
#include "SentinelLogging.hpp"
#include "SentinelLogSink.hpp"
#include "ConfigLoader.hpp"
#include "servermodel/RecordingDir.hpp"

int main(int argc, char *argv[]) {
    sentinel::logging::installLogSink("sentinel-server", argc, argv);
    sLog_App("Starting Sentinel Server...");

    QCoreApplication app(argc, argv);

    ServerConfig serverConfig;
    if (!ConfigLoader::loadServerConfig("config/server_config.yaml", &serverConfig)) {
        sLog_Warning("Server config not loaded, using defaults: path=config/server_config.yaml"
                     << " cwd=" << QDir::currentPath());
    }
    ConfigLoader::loadServerConfig("config/.server_config.yaml", &serverConfig);

    // --require-recording (used by the launchd service): never fall back to the
    // system disk. If the recording volume is not mounted or not accessible
    // (for example a missing Full Disk Access grant), exit with EX_TEMPFAIL so
    // launchd retries instead of recording somewhere else.
    if (QCoreApplication::arguments().contains(QStringLiteral("--require-recording"))) {
        auto &rc = serverConfig.recording;
        rc.fallbackDir.clear();
        if (rc.enabled && recording::resolveRecordingDir(rc.dir, rc.fallbackDir).dir.empty()) {
            sLog_Error("Recording required but the volume for " << rc.dir
                       << " is not mounted or not accessible; exiting for retry");
            return 75;
        }
    }
    
    SentinelServerApp serverApp(serverConfig);
    if (!serverApp.initialize()) {
        sLog_Error("Failed to initialize server application");
        return 1;
    }

    sLog_App("Sentinel Server running. Press Ctrl+C to stop.");
    return app.exec();
}

