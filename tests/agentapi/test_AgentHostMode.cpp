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
    EXPECT_FALSE(AgentHostMode::embeddedQmlOnly());
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
    EXPECT_TRUE(AgentHostMode::embeddedQmlOnly());
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
