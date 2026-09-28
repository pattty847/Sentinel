#pragma once

#include <QString>

// Per-run log file for every Qt message (qDebug, qCDebug, sLog_*, Qt's own
// categories). Each app calls installLogSink() first thing in main().
//
// Line format:
//   2026-09-27 14:03:22.417 D render  QSGRenderThread DataProcessor.cpp:90 | message
//   <local time>            <level D/I/W/E/F> <category> <thread> <file:line> | <message>
//
// Files: <dir>/<app>-YYYYMMDD-HHMMSS-<pid>.log, plus a <app>-latest.log symlink
// (POSIX). The file starts with '#' header lines: version, pid, exe, cwd, args
// and the SENTINEL_/QT_/QSG_ environment.
//
// Environment:
//   SENTINEL_LOG_DIR     directory (default ~/Library/Logs/Sentinel on macOS,
//                        <GenericDataLocation>/Sentinel/logs elsewhere)
//   SENTINEL_LOG_FILE=0  no file, stderr only
//   SENTINEL_LOG_KEEP    runs kept per app (default 20)
//   SENTINEL_LOG_STDERR=0  no stderr echo
//
// Warnings and above flush at once; lower levels flush within 250 ms.
namespace sentinel::logging {

struct SinkOptions {
    QString appName;
    QString dir;
    int keep = 20;
    bool writeFile = true;
    bool echoStderr = true;
};

// Options for appName with the environment overrides above applied.
SinkOptions sinkOptionsFromEnv(const QString& appName);

// Installs the Qt message handler. Returns the log file path, or an empty
// string if the file is disabled or could not be opened (stderr still works).
// Later calls return the first call's path.
QString installLogSink(const SinkOptions& options, int argc = 0, char** argv = nullptr);

inline QString installLogSink(const char* appName, int argc = 0, char** argv = nullptr) {
    return installLogSink(sinkOptionsFromEnv(QString::fromUtf8(appName)), argc, argv);
}

// Path of the current log file, or empty.
QString logFilePath();

// Writes buffered lines to disk now.
void flushLogSink();

} // namespace sentinel::logging
