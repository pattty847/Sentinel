#include <QCoreApplication>
#include "SentinelServerApp.hpp"
#include "SentinelLogging.hpp"

int main(int argc, char *argv[]) {
    // Set up logging
    qSetMessagePattern("[%{time yyyy-MM-dd h:mm:ss.zzz}] %{type}: %{message}");
    sLog_App("Starting Sentinel Server...");

    QCoreApplication app(argc, argv);
    
    SentinelServerApp serverApp;
    if (!serverApp.initialize()) {
        sLog_Error("Failed to initialize server application");
        return 1;
    }

    sLog_App("Sentinel Server running. Press Ctrl+C to stop.");
    return app.exec();
}

