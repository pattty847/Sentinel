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
// screen pixels, must keep every file it writes inside the host's session directory, and must not
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

// Regression guard: an agent-run GUI (--agent-host) must never send a TradeCommand. Every trade path
// (dock buttons, shortcuts, the chart's TP/SL controls that /api/v1/input can reach) ends in
// IGridDataSource::sendTradeCommand -> RemoteGridDataSource -> SentinelStreamClient. The guard sits in
// RemoteGridDataSource; this fails if a second place starts talking to the stream client directly, or
// if the guard is removed or moved after the send.
TEST(AgentHostModeSources, TradeCommandsHaveOneChokePointAndItIsGuarded) {
    const QRegularExpression directSend(R"(\bm_client\s*\.\s*sendTradeCommand\s*\()");
    const QRegularExpression anyClientSend(R"((\w+)\s*(\.|->)\s*sendTradeCommand\s*\()");
    QStringList viaInterface, directSenders;
    QString chokeFile;
    for (const QString& sub : {QStringLiteral("libs/gui"), QStringLiteral("apps/sentinel_gui")}) {
        QDirIterator it(QString(SENTINEL_SOURCE_DIR) + "/" + sub, {"*.cpp", "*.hpp", "*.h"}, QDir::Files,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString path = it.next();
            QFile file(path);
            ASSERT_TRUE(file.open(QIODevice::ReadOnly)) << qPrintable(path);
            const QString text = QString::fromUtf8(file.readAll());
            if (directSend.match(text).hasMatch()) {
                directSenders << path;
                chokeFile = path;
            }
            auto matches = anyClientSend.globalMatch(text);
            while (matches.hasNext()) {
                const QString receiver = matches.next().captured(1);
                if (receiver != "m_dataSource" && receiver != "m_client") viaInterface << path + " (" + receiver + ")";
            }
        }
    }
    ASSERT_EQ(directSenders.size(), 1) << "exactly one file may call the stream client's sendTradeCommand: "
                                       << qPrintable(directSenders.join(", "));
    EXPECT_TRUE(directSenders.first().endsWith("/datasources/RemoteGridDataSource.cpp")) << qPrintable(directSenders.first());
    EXPECT_TRUE(viaInterface.isEmpty()) << "trade commands must go through m_dataSource->sendTradeCommand: "
                                        << qPrintable(viaInterface.join(", "));
    QFile choke(chokeFile);
    ASSERT_TRUE(choke.open(QIODevice::ReadOnly));
    const QString text = QString::fromUtf8(choke.readAll());
    const int guard = text.indexOf("AgentHostMode::tradingAllowed()");
    const int send = text.indexOf(QRegularExpression(R"(m_client\s*\.\s*sendTradeCommand\s*\()"));
    EXPECT_GE(guard, 0) << "the agent-host guard is missing";
    EXPECT_LT(guard, send) << "the guard must come before the send";
}
