// Lab outputs (screenshots, frame sequences, bench JSON) must never be written
// inside the recording root, however the path reaches it: an alias of the root,
// a linked ancestor with a missing descendant, or an output directory that is
// itself a link into the root. Links are symlinks, or junctions on Windows.
#include "lab/LabSources.hpp"
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <gtest/gtest.h>
#include <filesystem>
#include <string>

namespace {
namespace fs = std::filesystem;

// A directory link at `link` to `target`: a symlink, or on Windows a junction
// (no privilege needed). False when neither can be created here.
bool makeDirectoryLink(const fs::path &link, const fs::path &target) {
    std::error_code ec;
    fs::create_directory_symlink(target, link, ec);
    if (!ec) return true;
#ifdef _WIN32
    QProcess mklink; // mklink rejects forward slashes
    mklink.start(QStringLiteral("cmd.exe"), {QStringLiteral("/c"), QStringLiteral("mklink"), QStringLiteral("/J"),
                                             QString::fromStdWString(fs::path(link).make_preferred().wstring()),
                                             QString::fromStdWString(fs::path(target).make_preferred().wstring())});
    return mklink.waitForFinished(10'000) && mklink.exitCode() == 0 && fs::exists(link);
#else
    return false;
#endif
}

QString q(const fs::path &p) { return QString::fromStdWString(p.wstring()); }

class LabOutputGuard : public testing::Test {
protected:
    QTemporaryDir dir;
    fs::path base, root;
    void SetUp() override {
        ASSERT_TRUE(dir.isValid());
        base = fs::path(dir.path().toStdWString());
        root = base / "recording";
        fs::create_directories(root / "BTC-USD");
        setRoot(root);
    }
    void TearDown() override { qunsetenv("SENTINEL_RECORDING_ROOT"); }
    static void setRoot(const fs::path &p) { qputenv("SENTINEL_RECORDING_ROOT", q(p).toUtf8()); }
    static bool allowed(const fs::path &p) { return lab::labOutputAllowed(q(p)); }
};

TEST_F(LabOutputGuard, PlainPathsInsideAreRefusedAndSiblingsAllowed) {
    EXPECT_FALSE(allowed(root / "shot.png"));
    EXPECT_FALSE(allowed(root / "BTC-USD" / "new" / "shot.png"));
    EXPECT_FALSE(allowed(root));
    EXPECT_TRUE(allowed(base / "out" / "shot.png"));
    EXPECT_TRUE(allowed(base / "recording-other" / "shot.png")) << "a name prefix is not containment";
}

TEST_F(LabOutputGuard, RootGivenThroughAnAliasStillProtectsTheRealDirectory) {
    const auto alias = base / "alias";
    if (!makeDirectoryLink(alias, root)) GTEST_SKIP() << "cannot create a directory link here";
    setRoot(alias);
    EXPECT_FALSE(allowed(root / "shot.png"));
    EXPECT_FALSE(allowed(alias / "shot.png"));
}

TEST_F(LabOutputGuard, LinkedAncestorWithMissingDescendantsIsRefused) {
    const auto link = base / "link";
    if (!makeDirectoryLink(link, root)) GTEST_SKIP() << "cannot create a directory link here";
    EXPECT_FALSE(allowed(link / "new" / "deeper" / "shot.png"));
}

TEST_F(LabOutputGuard, OutputDirectoryThatIsALinkIntoTheRootIsRefused) {
    const auto outdir = base / "frames";
    if (!makeDirectoryLink(outdir, root / "BTC-USD")) GTEST_SKIP() << "cannot create a directory link here";
    EXPECT_FALSE(allowed(outdir));
    EXPECT_FALSE(allowed(outdir / "tick-change-000ms.png"));
}

TEST_F(LabOutputGuard, UnresolvableLinkFailsClosed) {
    const auto gone = base / "gone";
    fs::create_directories(gone);
    const auto link = base / "dangling";
    if (!makeDirectoryLink(link, gone)) GTEST_SKIP() << "cannot create a directory link here";
    fs::remove(gone);
    QString why;
    EXPECT_FALSE(lab::labOutputAllowed(q(link / "shot.png"), &why));
    EXPECT_FALSE(why.isEmpty());
}

// The lab's own writers go through the guard before the run and before the
// write: --b1-bench opens its JSON WriteOnly, --window-screenshot saves a PNG.
QByteArray readAll(const fs::path &p) {
    QFile f(q(p));
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}
int runLab(const QStringList &args, const fs::path &root, QByteArray *output) {
    QProcess lab;
    auto env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("SENTINEL_RECORDING_ROOT"), q(root));
    env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
    lab.setProcessEnvironment(env);
    lab.setProcessChannelMode(QProcess::MergedChannels);
    lab.start(QStringLiteral(SENTINEL_LAB_EXECUTABLE), args);
    if (!lab.waitForFinished(90'000)) {
        lab.kill();
        lab.waitForFinished(5'000);
    }
    if (output) *output = lab.readAll();
    return lab.exitStatus() == QProcess::NormalExit ? lab.exitCode() : -1;
}

TEST_F(LabOutputGuard, B1BenchRefusesAJsonPathInsideTheRootAndLeavesTheFile) {
    const auto target = root / "BTC-USD" / "keep.json";
    { QFile f(q(target)); ASSERT_TRUE(f.open(QIODevice::WriteOnly)); f.write("recording data"); }
    QByteArray output;
    EXPECT_EQ(runLab({QStringLiteral("--b1-bench"), q(target), QStringLiteral("--b1-quick"),
                      QStringLiteral("--b1-modes"), QStringLiteral("hybrid")}, root, &output), 2)
        << output.toStdString();
    EXPECT_EQ(readAll(target), QByteArray("recording data"));
    EXPECT_TRUE(output.contains("recording root")) << output.toStdString();
}

TEST_F(LabOutputGuard, WindowScreenshotRefusesAPathInsideTheRoot) {
    const auto target = root / "window.png";
    QByteArray output;
    EXPECT_EQ(runLab({QStringLiteral("--synthetic"), QStringLiteral("20000"), QStringLiteral("--window-screenshot"),
                      q(target)}, root, &output), 2)
        << output.toStdString();
    EXPECT_FALSE(fs::exists(target));
    EXPECT_TRUE(output.contains("recording root")) << output.toStdString();
}

// "link/.." must not be cleaned lexically before the link is resolved: with
// link -> recording/BTC-USD, "link/../result.png" is recording/result.png on
// POSIX. Any ".." component is refused, checked through a real writer.
TEST_F(LabOutputGuard, DotDotAfterALinkIsRefusedByTheScreenshotWriter) {
    const auto link = base / "link";
    if (!makeDirectoryLink(link, root / "BTC-USD")) GTEST_SKIP() << "cannot create a directory link here";
    const QString requested = q(link) + QStringLiteral("/../result.png");
    EXPECT_FALSE(lab::labOutputAllowed(requested));
    QByteArray output;
    EXPECT_EQ(runLab({QStringLiteral("--synthetic"), QStringLiteral("20000"), QStringLiteral("--screenshot"), requested},
                     root, &output), 2)
        << output.toStdString();
    EXPECT_TRUE(output.contains("..")) << output.toStdString();
    EXPECT_FALSE(fs::exists(root / "result.png"));
    EXPECT_FALSE(fs::exists(base / "result.png"));
}

TEST_F(LabOutputGuard, PathsOutsideTheConfiguredRootAreAllowedWithoutTheOverride) {
    qunsetenv("SENTINEL_RECORDING_ROOT"); // the config default (if any) is not this temp directory
    EXPECT_TRUE(allowed(base / "anywhere.png"));
}
} // namespace
