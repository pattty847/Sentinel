#include <QCoreApplication>
#include <QDir>
#include "SentinelServerApp.hpp"
#include "SentinelLogging.hpp"
#include "SentinelLogSink.hpp"
#include "ConfigLoader.hpp"

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
    
    SentinelServerApp serverApp(serverConfig);
    if (!serverApp.initialize()) {
        sLog_Error("Failed to initialize server application");
        return 1;
    }

    sLog_App("Sentinel Server running. Press Ctrl+C to stop.");
    return app.exec();
}

