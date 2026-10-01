#include <QCoreApplication>
#include <QDir>
#include "SentinelServerApp.hpp"
#include "SentinelLogging.hpp"
#include "SentinelLogSink.hpp"
#include "ConfigLoader.hpp"
#include "servermodel/RecordingDir.hpp"
#include <filesystem>
#include <unistd.h>
#include <cerrno>

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

    // --require-recording (used by the launchd service): record to the configured
    // directory or exit with EX_TEMPFAIL (75) so launchd retries. Never fall back
    // to the system disk: an unmounted T7 or a missing Full Disk Access grant
    // (TCC denies the access, not the existence check) must not move recording.
    const bool requireRecording = QCoreApplication::arguments().contains(QStringLiteral("--require-recording"));
    if (requireRecording) {
        auto &rc = serverConfig.recording;
        rc.fallbackDir.clear();
        std::error_code ec;
        const bool usable = rc.enabled &&
                            !recording::resolveRecordingDir(rc.dir, rc.fallbackDir).dir.empty() &&
                            std::filesystem::is_directory(rc.dir, ec) && !ec &&
                            ::access(rc.dir.c_str(), R_OK | W_OK | X_OK) == 0;
        if (!usable) {
            sLog_Error("Recording required but " << rc.dir << " is not enabled, mounted or accessible"
                       << " (enabled=" << rc.enabled << " errno=" << errno << "); exiting for retry");
            return 75;
        }
    }

    SentinelServerApp serverApp(serverConfig);
    if (!serverApp.initialize()) {
        sLog_Error("Failed to initialize server application");
        return 1;
    }
    if (requireRecording && !serverApp.recording()) {
        sLog_Error("Recording required but the recorder did not start; exiting for retry");
        return 75;
    }

    sLog_App("Sentinel Server running. Press Ctrl+C to stop.");
    return app.exec();
}

