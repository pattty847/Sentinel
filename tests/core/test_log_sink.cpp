#include "SentinelLogSink.hpp"
#include "SentinelLogging.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QTemporaryDir>

#include <gtest/gtest.h>

namespace sl = sentinel::logging;

namespace {

QStringList readLines(const QString& path) {
    sl::flushLogSink();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    return QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
}

QString findLine(const QStringList& lines, const QString& needle) {
    for (const QString& line : lines) {
        if (line.contains(needle)) return line;
    }
    return {};
}

} // namespace

TEST(LogProbeFilter, NamesPrefixesAndAll) {
    EXPECT_FALSE(sl::probeFilterMatches(QString(), "tpo.ingest"));
    EXPECT_TRUE(sl::probeFilterMatches(QStringLiteral("tpo"), "tpo.ingest"));
    EXPECT_TRUE(sl::probeFilterMatches(QStringLiteral("TPO.Ingest"), "tpo.ingest"));
    EXPECT_TRUE(sl::probeFilterMatches(QStringLiteral("heatmap, tpo.ingest"), "tpo.ingest"));
    EXPECT_TRUE(sl::probeFilterMatches(QStringLiteral("all"), "anything.at.all"));
    EXPECT_FALSE(sl::probeFilterMatches(QStringLiteral("tp"), "tpo.ingest"));
    EXPECT_FALSE(sl::probeFilterMatches(QStringLiteral("tpo.ingest.rows"), "tpo.ingest"));
}

// One test installs the process-wide sink; the rest of this binary logs into it.
TEST(LogSink, WritesHeaderAndFormattedLinesAndPrunesOldRuns) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    QDir dir(tmp.path());
    for (int i = 0; i < 5; ++i) {
        QFile old(dir.filePath(QStringLiteral("sinktest-20000101-00000%1-1.log").arg(i)));
        ASSERT_TRUE(old.open(QIODevice::WriteOnly));
    }
    QFile other(dir.filePath(QStringLiteral("otherapp-20000101-000000-1.log")));
    ASSERT_TRUE(other.open(QIODevice::WriteOnly));

    qputenv("SENTINEL_PROBES", "tpo");
    QLoggingCategory::setFilterRules(QStringLiteral("sentinel.*=true"));

    sl::SinkOptions options;
    options.appName = QStringLiteral("sinktest");
    options.dir = tmp.path();
    options.keep = 3;
    options.echoStderr = false;
    char arg0[] = "test_log_sink";
    char* argv[] = {arg0};
    const QString path = sl::installLogSink(options, 1, argv);
    ASSERT_FALSE(path.isEmpty());
    EXPECT_EQ(sl::logFilePath(), path);
    EXPECT_EQ(sl::installLogSink(options), path);

    const QStringList runs = dir.entryList({QStringLiteral("sinktest-2*.log")}, QDir::Files, QDir::Name);
    ASSERT_EQ(runs.size(), 3);
    EXPECT_EQ(runs.last(), QFileInfo(path).fileName());
    EXPECT_TRUE(dir.exists(QStringLiteral("otherapp-20000101-000000-1.log")));
#ifndef _WIN32
    EXPECT_EQ(QFileInfo(dir.filePath(QStringLiteral("sinktest-latest.log"))).canonicalFilePath(),
              QFileInfo(path).canonicalFilePath());
#endif

    sLog_Render("render line" << 42);
    sLog_Warning("warn line");
    sLog_Probe("tpo.ingest", "rows=" << 7 << "span=" << std::string("[3..9]"));
    sLog_Probe("heatmap.window", "should not print");

    const QStringList lines = readLines(path);
    ASSERT_FALSE(lines.isEmpty());
    EXPECT_TRUE(lines.first().startsWith(QStringLiteral("# Sentinel log: app=sinktest")));
    EXPECT_FALSE(findLine(lines, QStringLiteral("env SENTINEL_PROBES=tpo")).isEmpty());

    const QString render = findLine(lines, QStringLiteral("render line 42"));
    ASSERT_FALSE(render.isEmpty());
    EXPECT_TRUE(render.contains(QStringLiteral(" D render  main test_log_sink.cpp:")));
    EXPECT_TRUE(render.endsWith(QStringLiteral("| render line 42")));

    const QString warn = findLine(lines, QStringLiteral("warn line"));
    EXPECT_TRUE(warn.contains(QStringLiteral(" W app ")));

    const QString probe = findLine(lines, QStringLiteral("[tpo.ingest]"));
    EXPECT_TRUE(probe.endsWith(QStringLiteral("| [tpo.ingest] rows= 7 span= [3..9]")));
    EXPECT_TRUE(probe.contains(QStringLiteral(" D probe ")));
    EXPECT_TRUE(findLine(lines, QStringLiteral("should not print")).isEmpty());
}
