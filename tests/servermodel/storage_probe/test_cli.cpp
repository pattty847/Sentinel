#include <gtest/gtest.h>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <nlohmann/json.hpp>

namespace {
auto readResult(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) throw std::runtime_error("missing probe result");
    return nlohmann::json::parse(f.readAll().toStdString());
}
TEST(StorageProbeCli, KilledProcessLeavesMinuteCheckpoint) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto path = dir.filePath("partial.json");
    QProcess child;
    child.start(STORAGE_PROBE_EXECUTABLE, {"--synthetic", "--minutes", "1440", "--out", path});
    ASSERT_TRUE(child.waitForStarted(5000));
    QByteArray progress;
    QElapsedTimer elapsed;
    elapsed.start();
    while (!progress.contains("elapsed_ms=60000") && elapsed.elapsed() < 15000 &&
           child.state() != QProcess::NotRunning) {
        child.waitForReadyRead(100);
        progress += child.readAllStandardError();
    }
    child.kill();
    ASSERT_TRUE(child.waitForFinished(5000));
    ASSERT_TRUE(progress.contains("elapsed_ms=60000")) << progress.toStdString();
    EXPECT_TRUE(progress.contains("events="));
    EXPECT_TRUE(progress.contains("raw_local_l2_bytes="));
    EXPECT_TRUE(progress.contains("twap_60000_bytes="));
    auto r = readResult(path);
    EXPECT_EQ(r["status"], "partial");
    EXPECT_EQ(r["finalized"], false);
    EXPECT_GE(r["measured_seconds"].get<double>(), 60);
    EXPECT_LT(r["measured_seconds"].get<double>(), 86400);
    EXPECT_GT(r["messages"].get<int>(), 0);
    ASSERT_EQ(r["encoders"].size(), 5);
    for (const auto& e : r["encoders"]) EXPECT_GT(e["encoded_bytes"].get<int>(), 0);
}
TEST(StorageProbeCli, CheckpointsDoNotAlterFinalEncoding) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto path = dir.filePath("complete.json");
    QProcess child;
    child.start(STORAGE_PROBE_EXECUTABLE, {"--synthetic", "--minutes", "10", "--out", path});
    ASSERT_TRUE(child.waitForFinished(15000));
    ASSERT_EQ(child.exitCode(), 0) << child.readAllStandardError().toStdString();
    const auto r = readResult(path);
    EXPECT_EQ(r["status"], "complete");
    EXPECT_EQ(r["finalized"], true);
    EXPECT_EQ(r["measured_seconds"], 600.0);
    // f2a0b28 baseline: checkpoints must not flush raw blocks or close TWAP tails.
    const std::array<uint64_t, 5> bytes{300878, 1202572, 359226, 226278, 87990};
    ASSERT_EQ(r["encoders"].size(), bytes.size());
    for (size_t i = 0; i < bytes.size(); ++i) EXPECT_EQ(r["encoders"][i]["encoded_bytes"], bytes[i]);
}
}
