#include "SentinelLogSink.hpp"

#include "SentinelLogging.hpp"
#include "Version.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QSysInfo>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(__APPLE__) || defined(__linux__)
#include <pthread.h>
#endif

namespace sentinel::logging {
namespace {

struct Sink {
    std::FILE* file = nullptr;
    QString path;
    bool echoStderr = true;
    std::thread::id mainThread;

    std::mutex flushMutex;
    std::condition_variable flushCv;
    bool stopping = false;
    std::thread flusher;
};

// Leaked on purpose: static destructors may still log after exit handlers run.
std::atomic<Sink*> g_sink{nullptr};
std::mutex g_installMutex;

char levelChar(QtMsgType type) {
    switch (type) {
        case QtDebugMsg:    return 'D';
        case QtInfoMsg:     return 'I';
        case QtWarningMsg:  return 'W';
        case QtCriticalMsg: return 'E';
        case QtFatalMsg:    return 'F';
    }
    return '?';
}

void appendTimestamp(std::string& out) {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const std::time_t secs = system_clock::to_time_t(now);
    const int ms = static_cast<int>(duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000);

    thread_local std::time_t cachedSecs = -1;
    thread_local char cached[24] = {};
    if (secs != cachedSecs) {
        std::tm tm{};
#if defined(_WIN32)
        localtime_s(&tm, &secs);
#else
        localtime_r(&secs, &tm);
#endif
        std::strftime(cached, sizeof(cached), "%Y-%m-%d %H:%M:%S", &tm);
        cachedSecs = secs;
    }
    char msBuf[8];
    std::snprintf(msBuf, sizeof(msBuf), ".%03d", ms);
    out += cached;
    out += msBuf;
}

std::string currentThreadName(const Sink& sink) {
    if (std::this_thread::get_id() == sink.mainThread) return "main";
#if defined(__APPLE__) || defined(__linux__)
    char buf[64] = {};
    if (pthread_getname_np(pthread_self(), buf, sizeof(buf)) == 0 && buf[0] != '\0') {
        std::string name(buf);
        std::replace(name.begin(), name.end(), ' ', '_');
        return name;
    }
#endif
    char idBuf[32];
    std::snprintf(idBuf, sizeof(idBuf), "t%zx",
                  std::hash<std::thread::id>{}(std::this_thread::get_id()) & 0xffffff);
    return idBuf;
}

const char* categoryLabel(const char* category) {
    if (!category || category[0] == '\0') return "default";
    constexpr const char kPrefix[] = "sentinel.";
    if (std::strncmp(category, kPrefix, sizeof(kPrefix) - 1) == 0) {
        return category + sizeof(kPrefix) - 1;
    }
    return category;
}

const char* fileBaseName(const char* file) {
    const char* base = file;
    for (const char* p = file; *p; ++p) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }
    return base;
}

void messageHandler(QtMsgType type, const QMessageLogContext& ctx, const QString& msg) {
    Sink* sink = g_sink.load(std::memory_order_acquire);
    if (!sink) return;

    thread_local std::string line;
    thread_local std::string threadName = currentThreadName(*sink);
    line.clear();

    appendTimestamp(line);
    line += ' ';
    line += levelChar(type);
    line += ' ';
    const char* category = categoryLabel(ctx.category);
    line += category;
    for (std::size_t n = std::strlen(category); n < 7; ++n) line += ' ';
    line += ' ';
    line += threadName;
    line += ' ';
    if (ctx.file && ctx.file[0] != '\0') {
        line += fileBaseName(ctx.file);
        line += ':';
        line += std::to_string(ctx.line);
        line += ' ';
    }
    line += "| ";
    const QByteArray utf8 = msg.toUtf8();
    for (char c : utf8) {
        line += c;
        if (c == '\n') line += "    ";
    }
    line += '\n';

    if (sink->file) {
        std::fwrite(line.data(), 1, line.size(), sink->file);
        if (type != QtDebugMsg && type != QtInfoMsg) std::fflush(sink->file);
    }
    if (sink->echoStderr || !sink->file) {
        std::fwrite(line.data(), 1, line.size(), stderr);
    }
}

void stopFlusher() {
    Sink* sink = g_sink.load(std::memory_order_acquire);
    if (!sink) return;
    {
        std::lock_guard<std::mutex> lock(sink->flushMutex);
        sink->stopping = true;
    }
    sink->flushCv.notify_all();
    if (sink->flusher.joinable()) sink->flusher.join();
    if (sink->file) std::fflush(sink->file);
}

QString defaultLogDir() {
#if defined(Q_OS_MACOS)
    return QDir::homePath() + QStringLiteral("/Library/Logs/Sentinel");
#else
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
        + QStringLiteral("/Sentinel/logs");
#endif
}

std::FILE* openAppend(const QString& path) {
#if defined(_WIN32)
    return _wfopen(reinterpret_cast<const wchar_t*>(path.utf16()), L"ab");
#else
    return std::fopen(QFile::encodeName(path).constData(), "ab");
#endif
}

void pruneOldRuns(QDir dir, const QString& appName, int keep) {
    const QString latest = appName + QStringLiteral("-latest.log");
    QStringList runs = dir.entryList({appName + QStringLiteral("-*.log")}, QDir::Files, QDir::Name);
    runs.removeAll(latest);
    // Names start with a sortable timestamp; drop the oldest first.
    for (int i = 0; i + keep < runs.size(); ++i) {
        dir.remove(runs[i]);
    }
}

void updateLatestLink(const QDir& dir, const QString& appName, const QString& fileName) {
#if defined(_WIN32)
    Q_UNUSED(dir); Q_UNUSED(appName); Q_UNUSED(fileName);
#else
    namespace fs = std::filesystem;
    const fs::path link = fs::path(QFile::encodeName(dir.filePath(appName + QStringLiteral("-latest.log"))).toStdString());
    std::error_code ec;
    fs::remove(link, ec);
    fs::create_symlink(QFile::encodeName(fileName).toStdString(), link, ec);
#endif
}

void writeHeader(std::FILE* file, const SinkOptions& options, int argc, char** argv) {
    QStringList lines;
    lines << QStringLiteral("Sentinel log: app=%1 version=%2 pid=%3")
                 .arg(options.appName,
                      QString::fromStdString(Sentinel::getVersionString()))
                 .arg(QCoreApplication::applicationPid());
    lines << QStringLiteral("started=%1").arg(QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
    if (argc > 0 && argv && argv[0]) {
        const QFileInfo exe(QString::fromLocal8Bit(argv[0]));
        lines << QStringLiteral("exe=%1 built=%2")
                     .arg(exe.absoluteFilePath(),
                          exe.lastModified().toString(Qt::ISODate));
        QStringList args;
        for (int i = 1; i < argc; ++i) args << QString::fromLocal8Bit(argv[i]);
        lines << QStringLiteral("args=%1").arg(args.join(QLatin1Char(' ')));
    }
    lines << QStringLiteral("cwd=%1").arg(QDir::currentPath());
    lines << QStringLiteral("qt=%1 os=%2 %3").arg(QString::fromLatin1(qVersion()),
                                                  QSysInfo::prettyProductName(),
                                                  QSysInfo::currentCpuArchitecture());

    static const QRegularExpression kSecret(QStringLiteral("KEY|SECRET|TOKEN|PASS|JWT"),
                                            QRegularExpression::CaseInsensitiveOption);
    const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    QStringList keys = env.keys();
    keys.sort();
    for (const QString& key : keys) {
        if (!key.startsWith(QStringLiteral("SENTINEL_")) && !key.startsWith(QStringLiteral("QT_")) &&
            !key.startsWith(QStringLiteral("QSG_"))) {
            continue;
        }
        const QString value = kSecret.match(key).hasMatch() ? QStringLiteral("<redacted>") : env.value(key);
        lines << QStringLiteral("env %1=%2").arg(key, value);
    }
    lines << QStringLiteral("format: <time> <level D/I/W/E/F> <category> <thread> <file:line> | <message>");

    for (const QString& l : lines) {
        const QByteArray utf8 = QStringLiteral("# %1\n").arg(l).toUtf8();
        std::fwrite(utf8.constData(), 1, static_cast<std::size_t>(utf8.size()), file);
    }
    std::fflush(file);
}

} // namespace

SinkOptions sinkOptionsFromEnv(const QString& appName) {
    SinkOptions options;
    options.appName = appName;
    const QString dir = qEnvironmentVariable("SENTINEL_LOG_DIR");
    options.dir = dir.isEmpty() ? defaultLogDir() : dir;
    bool ok = false;
    const int keep = qEnvironmentVariableIntValue("SENTINEL_LOG_KEEP", &ok);
    if (ok && keep > 0) options.keep = keep;
    if (qEnvironmentVariable("SENTINEL_LOG_FILE") == QStringLiteral("0")) options.writeFile = false;
    if (qEnvironmentVariable("SENTINEL_LOG_STDERR") == QStringLiteral("0")) options.echoStderr = false;
    return options;
}

QString installLogSink(const SinkOptions& options, int argc, char** argv) {
    std::lock_guard<std::mutex> lock(g_installMutex);
    if (Sink* existing = g_sink.load()) return existing->path;

    auto* sink = new Sink;
    sink->echoStderr = options.echoStderr;
    sink->mainThread = std::this_thread::get_id();

    if (options.writeFile && !options.appName.isEmpty()) {
        QDir dir(options.dir);
        if (dir.mkpath(QStringLiteral("."))) {
            const QString fileName = QStringLiteral("%1-%2-%3.log")
                .arg(options.appName,
                     QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")))
                .arg(QCoreApplication::applicationPid());
            const QString path = dir.filePath(fileName);
            sink->file = openAppend(path);
            if (sink->file) {
                std::setvbuf(sink->file, nullptr, _IOFBF, 64 * 1024);
                sink->path = path;
                writeHeader(sink->file, options, argc, argv);
                pruneOldRuns(dir, options.appName, std::max(1, options.keep));
                updateLatestLink(dir, options.appName, fileName);
            }
        }
    }

    if (sink->file) {
        sink->flusher = std::thread([sink] {
            std::unique_lock<std::mutex> lk(sink->flushMutex);
            while (!sink->stopping) {
                sink->flushCv.wait_for(lk, std::chrono::milliseconds(250));
                std::fflush(sink->file);
            }
        });
        std::atexit(stopFlusher);
    }

    g_sink.store(sink, std::memory_order_release);
    qInstallMessageHandler(messageHandler);

    if (sink->file) {
        std::fprintf(stderr, "[sentinel] log file: %s\n", QFile::encodeName(sink->path).constData());
    } else if (options.writeFile) {
        std::fprintf(stderr, "[sentinel] could not open a log file in %s; logging to stderr only\n",
                     QFile::encodeName(options.dir).constData());
    }
    return sink->path;
}

QString logFilePath() {
    std::lock_guard<std::mutex> lock(g_installMutex);
    Sink* sink = g_sink.load();
    return sink ? sink->path : QString();
}

void flushLogSink() {
    Sink* sink = g_sink.load(std::memory_order_acquire);
    if (sink && sink->file) std::fflush(sink->file);
}

} // namespace sentinel::logging
