#include "config/AgentHostMode.hpp"

#include <QDir>
#include <QDirIterator>
#include <QRegularExpression>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QTemporaryDir>
#include <gtest/gtest.h>

// --agent-host: the GUI run by scripts/dev/gui-host.py for sandboxed agents. It must not capture
// screen pixels, must keep screenshots and general QSettings in its session, and must not
// load code (QML) from agent-writable paths. These are the policy pieces that need no window.

class AgentHostModeTest : public ::testing::Test {
protected:
    void SetUp() override { AgentHostMode::resetForTests(); }
    void TearDown() override { AgentHostMode::resetForTests(); }
};

TEST_F(AgentHostModeTest, InactiveByDefaultAndAllowsEveryTarget) {
    EXPECT_FALSE(AgentHostMode::active());
    EXPECT_TRUE(AgentHostMode::screenshotTargetAllowed("main"));
    EXPECT_TRUE(AgentHostMode::screenshotTargetAllowed("anything"));
    EXPECT_TRUE(AgentHostMode::tradingAllowed());
    EXPECT_TRUE(AgentHostMode::symbolAllowed("ANYTHING-USD"));
}

TEST_F(AgentHostModeTest, ActiveModeRefusesScreenPixelTargets) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QString error;
    ASSERT_TRUE(AgentHostMode::activate(dir.path(), {}, &error)) << qPrintable(error);
    EXPECT_TRUE(AgentHostMode::active());
    // "main" grabs screen pixels (FM-120); the legacy /screenshot route does not validate target
    // strings, so unknown and empty names must be refused too, not only "main".
    for (const char* bad : {"main", "", "MAIN", "window", "screen", "../main", "settings:../x", "settings:"})
        EXPECT_FALSE(AgentHostMode::screenshotTargetAllowed(bad)) << bad;
    for (const char* ok : {"heatmap", "lab", "telemetry", "toolbar", "settings", "settings:Tick", "settings:TPO"})
        EXPECT_TRUE(AgentHostMode::screenshotTargetAllowed(ok)) << ok;
}

TEST_F(AgentHostModeTest, ActiveModeBlocksTradingAndRestrictsSymbolsToTheAllowlist) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QString error;
    ASSERT_TRUE(AgentHostMode::activate(dir.path(), {}, &error)) << qPrintable(error);
    // /api/v1/input can drive the chart's TP/SL controls into the server's trading session, and a
    // symbol change makes the recorder subscribe upstream: both are off limits for an agent-run GUI.
    EXPECT_FALSE(AgentHostMode::tradingAllowed());
    EXPECT_FALSE(AgentHostMode::symbolAllowed("BTC-USD")) << "no allowlist means no symbol changes";
    AgentHostMode::setSymbolAllowlist({"BTC-USD", "ETH-USD"});
    EXPECT_TRUE(AgentHostMode::symbolAllowed("BTC-USD"));
    EXPECT_TRUE(AgentHostMode::symbolAllowed("ETH-USD"));
    for (const char* bad : {"btc-usd", "BTC-USD ", "SOL-USD", "", "BTC-USD,ETH-USD", "BTC", "ETH-USD\n"})
        EXPECT_FALSE(AgentHostMode::symbolAllowed(bad)) << bad;
    AgentHostMode::resetForTests();
    EXPECT_TRUE(AgentHostMode::tradingAllowed());
    EXPECT_TRUE(AgentHostMode::symbolAllowed("SOL-USD"));
}

TEST_F(AgentHostModeTest, ScreenshotDirIsFixedInsideTheSessionDirAndCreated) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QString error;
    ASSERT_TRUE(AgentHostMode::activate(dir.path(), {}, &error)) << qPrintable(error);
    const QString canonical = QFileInfo(dir.path()).canonicalFilePath();
    EXPECT_EQ(AgentHostMode::screenshotDir(), canonical + "/screenshots");
    EXPECT_TRUE(QFileInfo(AgentHostMode::screenshotDir()).isDir());
}

TEST_F(AgentHostModeTest, SettingsAreIsolatedIntoTheSessionDirNotTheOwnersDomain) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QString error;
    ASSERT_TRUE(AgentHostMode::activate(dir.path(), {}, &error)) << qPrintable(error);
    // The GUI constructs QSettings("Sentinel", "SentinelTerminal") with the process default format.
    {
        QSettings settings(QSettings::defaultFormat(), QSettings::UserScope, "Sentinel", "AgentHostModeTestDomain");
        settings.setValue("probe", 42);
        settings.sync();
        EXPECT_EQ(settings.format(), QSettings::IniFormat);
        EXPECT_TRUE(settings.fileName().startsWith(QFileInfo(dir.path()).canonicalFilePath() + "/settings/"))
            << qPrintable(settings.fileName());
    }
    EXPECT_FALSE(QFile::exists(QDir::homePath() + "/Library/Preferences/com.sentinel.AgentHostModeTestDomain.plist"));
}

TEST_F(AgentHostModeTest, OnlyDockProfileSurvivesDifferentSessionDirectories) {
    QTemporaryDir dir;
    const QString profile = dir.path() + "/profile/docks.ini";
    QString error;
    ASSERT_TRUE(AgentHostMode::activate(dir.path() + "/first", {}, &error, profile)) << qPrintable(error);
    {
        QSettings settings(QSettings::defaultFormat(), QSettings::UserScope, "Sentinel", "AgentHostProfileTest");
        settings.setValue("heatmap/changed", true);
        settings.sync();
    }
    {
        QSettings docks(profile, QSettings::IniFormat);
        docks.setValue("agentApi/docks/visible", QVariantMap{{"heatmap", true}});
        docks.sync();
    }
    AgentHostMode::resetForTests();
    ASSERT_TRUE(AgentHostMode::activate(dir.path() + "/second", {}, &error, profile)) << qPrintable(error);
    QSettings settings(QSettings::defaultFormat(), QSettings::UserScope, "Sentinel", "AgentHostProfileTest");
    EXPECT_FALSE(settings.contains("heatmap/changed"));
    EXPECT_TRUE(settings.fileName().startsWith(QFileInfo(dir.path()).canonicalFilePath() + "/second/settings/"))
        << qPrintable(settings.fileName());
    QSettings docks(profile, QSettings::IniFormat);
    EXPECT_TRUE(docks.value("agentApi/docks/visible").toMap().value("heatmap").toBool());
    EXPECT_NE(AgentHostMode::screenshotDir(), dir.path() + "/first/screenshots");
}

TEST_F(AgentHostModeTest, ProfileDirGetsSameForbiddenRootChecksAsSessionDir) {
    QTemporaryDir dir;
    ASSERT_TRUE(QDir().mkpath(dir.path() + "/repo"));
    QString error;
    EXPECT_FALSE(AgentHostMode::activate(dir.path() + "/session", {dir.path() + "/repo"}, &error,
                                          dir.path() + "/repo/docks.ini"));
    EXPECT_FALSE(QFileInfo::exists(dir.path() + "/session"));
    EXPECT_FALSE(AgentHostMode::activate(dir.path() + "/session", {}, &error, "/Volumes/T7/profile/docks.ini"));
    EXPECT_FALSE(AgentHostMode::activate(dir.path() + "/session", {}, &error,
                                         QDir::homePath() + "/Library/Preferences/docks.ini"));
}

TEST_F(AgentHostModeTest, DockPersistencePolicyHonorsHostAndOwnerFlag) {
    EXPECT_FALSE(AgentHostMode::dockChangesPersist(false));
    EXPECT_TRUE(AgentHostMode::dockChangesPersist(true));
    QTemporaryDir dir;
    QString error;
    ASSERT_TRUE(AgentHostMode::activate(dir.path(), {}, &error)) << qPrintable(error);
    EXPECT_TRUE(AgentHostMode::dockChangesPersist(false));
    EXPECT_TRUE(AgentHostMode::dockChangesPersist(true));
}

TEST(AgentHostModeSources, DockRouteUsesHostAwarePersistencePolicy) {
    QFile file(QString(SENTINEL_SOURCE_DIR) + "/libs/gui/MainWindowGpu.cpp");
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    const QString source = QString::fromUtf8(file.readAll());
    EXPECT_TRUE(source.contains("AgentHostMode::dockChangesPersist(body.persistDocks)"));
}

TEST_F(AgentHostModeTest, RefusesDirectoriesThatAreNotSafeAndLeavesModeInactive) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QString error;
    EXPECT_FALSE(AgentHostMode::activate("relative/dir", {}, &error));
    EXPECT_FALSE(error.isEmpty());
    EXPECT_FALSE(AgentHostMode::activate("", {}, &error));
    EXPECT_FALSE(AgentHostMode::activate("/Volumes/T7/anything", {}, &error)) << "recording drive and agent worktrees";
    EXPECT_FALSE(AgentHostMode::activate("/Volumes", {}, &error));
    // Inside a forbidden root (the repo / the build tree): agents can write there.
    const QString inside = dir.path() + "/repo/session";
    EXPECT_FALSE(AgentHostMode::activate(inside, {dir.path() + "/repo"}, &error));
    ASSERT_TRUE(QDir().mkpath(dir.path() + "/repo"));
    EXPECT_FALSE(AgentHostMode::activate(inside, {dir.path() + "/repo"}, &error));
    EXPECT_FALSE(AgentHostMode::activate(dir.path() + "/repo", {dir.path() + "/repo"}, &error)) << "the root itself";
    EXPECT_FALSE(AgentHostMode::active());
}

TEST_F(AgentHostModeTest, ARefusedDirectoryIsNeverCreated) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(QDir().mkpath(dir.path() + "/repo"));
    QString error;
    EXPECT_FALSE(AgentHostMode::activate(dir.path() + "/repo/new/deeper", {dir.path() + "/repo"}, &error));
    EXPECT_FALSE(QFileInfo::exists(dir.path() + "/repo/new")) << "refusal must not leave directories in the repo";
    ASSERT_TRUE(QFile::link(dir.path() + "/repo", dir.path() + "/link"));
    EXPECT_FALSE(AgentHostMode::activate(dir.path() + "/link/via/symlink", {dir.path() + "/repo"}, &error));
    EXPECT_FALSE(QFileInfo::exists(dir.path() + "/repo/via"));
}

TEST_F(AgentHostModeTest, ANewNestedDirectoryOutsideTheForbiddenRootsIsCreatedAndCanonical) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QString error;
    ASSERT_TRUE(AgentHostMode::activate(dir.path() + "/a/b/c", {}, &error)) << qPrintable(error);
    EXPECT_EQ(AgentHostMode::screenshotDir(), QFileInfo(dir.path()).canonicalFilePath() + "/a/b/c/screenshots");
}

TEST_F(AgentHostModeTest, StartupSymbolIsAnAllowlistedOneOrNoneAtAll) {
    EXPECT_EQ(AgentHostMode::startupSymbol("BTC-USD"), "BTC-USD") << "inactive: the caller's choice stands";
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QString error;
    ASSERT_TRUE(AgentHostMode::activate(dir.path(), {}, &error)) << qPrintable(error);
    // The GUI starts on a hard-coded BTC-USD and on the server's default symbol, then resubscribes it
    // on every reconnect; none of that may reach the recorder for a symbol outside the allowlist.
    EXPECT_EQ(AgentHostMode::startupSymbol("BTC-USD"), "") << "empty allowlist: stay unsubscribed";
    AgentHostMode::setSymbolAllowlist({"ETH-USD", "SOL-USD"});
    EXPECT_EQ(AgentHostMode::startupSymbol("BTC-USD"), "ETH-USD") << "allowlist excludes BTC: first allowed symbol";
    EXPECT_EQ(AgentHostMode::startupSymbol("SOL-USD"), "SOL-USD") << "an allowed preference stands";
    AgentHostMode::setSymbolAllowlist({"BTC-USD"});
    EXPECT_EQ(AgentHostMode::startupSymbol("BTC-USD"), "BTC-USD");
}

TEST_F(AgentHostModeTest, SymlinkIntoAForbiddenRootIsResolvedBeforeTheCheck) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(QDir().mkpath(dir.path() + "/repo"));
    ASSERT_TRUE(QFile::link(dir.path() + "/repo", dir.path() + "/innocent"));
    QString error;
    EXPECT_FALSE(AgentHostMode::activate(dir.path() + "/innocent", {dir.path() + "/repo"}, &error));
    EXPECT_FALSE(AgentHostMode::active());
}

// Regression guard: QSettings("org", "app") hardcodes the NATIVE format, which --agent-host cannot
// redirect (setDefaultFormat only reaches the one-argument constructor). A new one in the GUI would
// quietly write the owner's preferences plist from an agent run. Use
// QSettings(QSettings::defaultFormat(), QSettings::UserScope, "org", "app"). The standalone lab app
// (libs/gui/lab) is not run under --agent-host and is exempt.
TEST(AgentHostModeSources, NoNativeFormatQSettingsInTheGui) {
    const QRegularExpression nativeCtor(
        R"(QSettings\s*\w*\s*[({]\s*(QStringLiteral\()?"[^"]*"\)?\s*,\s*(QStringLiteral\()?")");
    const QRegularExpression lineComment(R"(//.*$)");
    QStringList offenders;
    for (const QString& sub : {QStringLiteral("libs/gui"), QStringLiteral("apps/sentinel_gui")}) {
        QDirIterator it(QString(SENTINEL_SOURCE_DIR) + "/" + sub, {"*.cpp", "*.hpp", "*.h"}, QDir::Files,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString path = it.next();
            if (path.contains("/libs/gui/lab/")) continue;
            QFile file(path);
            ASSERT_TRUE(file.open(QIODevice::ReadOnly)) << qPrintable(path);
            int line = 0;
            while (!file.atEnd()) {
                QString text = QString::fromUtf8(file.readLine());
                ++line;
                text.remove(lineComment);
                if (nativeCtor.match(text).hasMatch()) offenders << path + ":" + QString::number(line);
            }
        }
    }
    EXPECT_TRUE(offenders.isEmpty()) << "native-format QSettings (see comment): " << qPrintable(offenders.join(", "));
}

// Regression guard: an agent-run GUI (--agent-host) must not send a TradeCommand or an algo start/stop,
// and no outbound request may name a symbol outside the allowlist (a subscribe makes the recorder
// subscribe upstream). Every such message leaves the GUI through RemoteGridDataSource -> the stream
// client, so the guard lives there. This fails if (a) a second place talks to the stream client's
// trade/algo senders, (b) a sender in the data source loses its guard or the guard comes after the send,
// or (c) a new outbound method appears that nobody has classified.
TEST(AgentHostModeSources, EveryOutboundRequestInTheDataSourceIsGuarded) {
    const QSet<QString> guarded{"subscribe", "requestHeatmapHistory", "registerRecordingView",
                                "requestRecordingHeatmapHistory", "requestFootprintHistory",
                                "requestCandleHistory", "requestTpoHistory", "sendTradeCommand", "sendAlgoCommand"};
    // Connection management, releases, cancels and the local generation lookup:
    // they name no new symbol to the server or carry no request.
    const QSet<QString> exempt{"connectToServer", "unsubscribe", "releaseRecordingView", "cancelTpoHistory",
                               "setCandleDeliveryGeneration", "bookDeliveryGeneration"};
    QFile file(QString(SENTINEL_SOURCE_DIR) + "/libs/gui/datasources/RemoteGridDataSource.cpp");
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    const QString text = QString::fromUtf8(file.readAll());

    const QRegularExpression definition(R"(^[\w:<>&\* ]*\bRemoteGridDataSource::(\w+)\s*\()",
                                        QRegularExpression::MultilineOption);
    const QRegularExpression send(R"(\bm_client\s*\.\s*(\w+)\s*\()");
    QList<QRegularExpressionMatch> defs;
    for (auto it = definition.globalMatch(text); it.hasNext();) defs << it.next();
    ASSERT_GT(defs.size(), 20) << "the definition scan found too little";

    QSet<QString> seen;
    for (int i = 0; i < defs.size(); ++i) {
        const int begin = defs[i].capturedStart();
        const int end = i + 1 < defs.size() ? defs[i + 1].capturedStart() : text.size();
        const QString body = text.mid(begin, end - begin);
        for (auto it = send.globalMatch(body); it.hasNext();) {
            const auto m = it.next();
            const QString name = m.captured(1);
            if (guarded.contains(name)) {
                seen << name;
                // Either the policy directly (trade/algo) or the symbol helper that wraps it.
                const int guard = body.indexOf(QRegularExpression(R"(AgentHostMode::|\bsymbolPermitted\s*\()"));
                EXPECT_TRUE(guard >= 0 && guard < m.capturedStart())
                    << qPrintable(defs[i].captured(1)) << " sends " << qPrintable(name) << " without the agent-host guard before it";
            } else if (!exempt.contains(name)) {
                ADD_FAILURE() << qPrintable(defs[i].captured(1)) << " sends m_client." << qPrintable(name)
                              << ": classify it as guarded or exempt in this test (does it name a symbol or trade?)";
            }
        }
    }
    EXPECT_EQ(seen, guarded) << "every guarded sender must actually be found, or the scan is vacuous";
    // The helper the symbol guards call must itself consult the allowlist.
    const int helper = text.indexOf("bool symbolPermitted(");
    ASSERT_GE(helper, 0) << "symbolPermitted helper not found";
    const int helperEnd = text.indexOf("\n}\n", helper);
    EXPECT_TRUE(text.mid(helper, helperEnd - helper).contains("AgentHostMode::symbolAllowed"))
        << "symbolPermitted must call AgentHostMode::symbolAllowed";

    // Nobody else may reach the trade/algo senders; callers go through m_dataSource->.
    const QRegularExpression tradeSend(R"((\w+)\s*(\.|->)\s*(sendTradeCommand|sendAlgoCommand)\s*\()");
    QStringList offenders;
    for (const QString& sub : {QStringLiteral("libs/gui"), QStringLiteral("apps/sentinel_gui")}) {
        QDirIterator it(QString(SENTINEL_SOURCE_DIR) + "/" + sub, {"*.cpp", "*.hpp", "*.h"}, QDir::Files,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString path = it.next();
            QFile f(path);
            ASSERT_TRUE(f.open(QIODevice::ReadOnly)) << qPrintable(path);
            const QString t = QString::fromUtf8(f.readAll());
            for (auto m = tradeSend.globalMatch(t); m.hasNext();) {
                const QString receiver = m.next().captured(1);
                const bool viaDataSource = receiver == "m_dataSource";
                const bool theGuardedSender = receiver == "m_client" && path.endsWith("/datasources/RemoteGridDataSource.cpp");
                if (!viaDataSource && !theGuardedSender) offenders << path + " (" + receiver + ")";
            }
        }
    }
    EXPECT_TRUE(offenders.isEmpty()) << "trade/algo commands must go through m_dataSource->: " << qPrintable(offenders.join(", "));
}
