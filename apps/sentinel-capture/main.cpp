#include <QCoreApplication>
#include "capture/CaptureSession.hpp"
#include "SentinelLogging.hpp"
#include "SentinelLogSink.hpp"

int main(int argc, char** argv) {
    sentinel::logging::installLogSink("sentinel-capture", argc, argv);
    QCoreApplication app(argc, argv);
    try {
        return sentinel::capture::runApplication(app);
    } catch (const std::exception& e) {
        sLog_Error("Capture failed: " << e.what());
        return 1;
    }
}
