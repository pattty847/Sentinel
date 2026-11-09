#include "servermodel/FlowStore.hpp"
#include "ConfigLoader.hpp"
#include "flow/FlowBarBuilder.hpp"
#include "roller/fixtures/FlowJournal.hpp"
#include "roller/JournalReader.hpp"
#include "trading/MarketEventSource.hpp"
#include <gtest/gtest.h>
#include <QTemporaryDir>
#include <QFile>
#include <future>
#include <fstream>

namespace fs = std::filesystem;
namespace flow = sentinel::flow;
using namespace flow_fixture;
using recording::FlowStore;
namespace {
class FlowStoreTest : public testing::Test {
  protected:
    QTemporaryDir temp;
    fs::path base = temp.path().toStdString(), journal = base / "journal", hmc2 = base / "hmc2", root = base / "flow";
    int64_t through = Epoch + 2 * Hour;
    void SetUp() override {
        ASSERT_TRUE(temp.isValid());
        fs::create_directory(journal);
        fs::create_directory(hmc2);
    }
    FlowStore::Options options() {
        FlowStore::Options o;
        o.root = root;
        o.hmc2Root = hmc2;
        o.journalRoot = journal;
        o.symbols = {"BTC-USD"};
        o.hourThrough = [this](const std::string &) { return through; };
        return o;
    }
    void fixture() {
        Journal j(journal);
        j.snapshot(0);
        j.trades(1000, {trade(800, "1", 100000)});
        j.heartbeat(1500); // every sequence number must be parsed
        j.trades(Minute + 200, {trade(Minute + 100, "2", 100002, "SELL")});
        j.reconnect(2 * Minute);
        j.trades(2 * Minute + 100, {trade(800, "1", 100000), trade(2 * Minute, "3", 100003)}, true);
        j.trades(Hour + 500, {trade(Hour - 100, "4", 100004), trade(Hour + 100, "5", 100005)});
        j.heartbeat(2 * Hour + Minute + 1);
        j.close();
    }
    fs::path firstJournal() {
        sentinel::roller::JournalReader reader(journal, "BTC-USD");
        return reader.files().front().path;
    }
    void changeFingerprint() {
        const auto path = firstJournal();
        fs::last_write_time(path, fs::last_write_time(path) + std::chrono::seconds(2));
    }
    QByteArray bytes(const fs::path &p) {
        QFile f(QString::fromStdString(p.string()));
        if (!f.open(QIODevice::ReadOnly))
            return {};
        return f.readAll();
    }
};
TEST_F(FlowStoreTest, MatchesIndependentJournalTradesAndCodecAfterReconnectAndLateTrade) {
    fixture();
    FlowStore store(options());
    const auto pass = store.runPass();
    ASSERT_EQ(pass.failed, 0);
    ASSERT_EQ(pass.built, 2);
    trading::JournalTradeEventSource source(journal, "BTC-USD", Epoch, Epoch + 2 * Hour);
    flow::FlowBarBuilder oracle(Minute, 1, 100, 100'000'000, 100'000'000);
    while (auto event = source.next())
        if (event->trade) {
            const auto &t = *event->trade;
            oracle.add({t.tradeId, t.timestampMs, t.price, t.qty,
                        t.side == AggressorSide::Buy    ? flow::Side::Buy
                        : t.side == AggressorSide::Sell ? flow::Side::Sell
                                                        : flow::Side::Unknown});
        }
    const auto expected = oracle.bars();
    const auto actual = store.window("BTC-USD", Epoch, Epoch + 2 * Hour);
    ASSERT_FALSE(actual.building);
    ASSERT_EQ(actual.bars.size(), expected.size());
    ASSERT_EQ(expected.size(), 5);
    for (size_t i = 0; i < expected.size(); ++i) {
        const auto wire = flow::encodeFlowBar(expected[i]);
        EXPECT_EQ(flow::encodeFlowBar(flow::decodeFlowBar(flow::encodeFlowBar(actual.bars[i]))), wire);
    }
    EXPECT_EQ(store.runPass().built, 0);
}
TEST_F(FlowStoreTest, FingerprintChangeRefusesStaleDayAndRebuildsWithNewRevisions) {
    fixture();
    FlowStore store(options());
    ASSERT_EQ(store.runPass().built, 2);
    const auto path = store.filePath("BTC-USD", Epoch);
    const auto before = bytes(path);
    const auto old = store.window("BTC-USD", Epoch, Epoch + 2 * Hour);
    changeFingerprint();
    const auto stale = store.window("BTC-USD", Epoch, Epoch + 2 * Hour);
    EXPECT_TRUE(stale.building);
    EXPECT_TRUE(stale.bars.empty());
    EXPECT_EQ(store.runPass().built, 2);
    EXPECT_NE(bytes(path), before);
    const auto fresh = store.window("BTC-USD", Epoch, Epoch + 2 * Hour);
    EXPECT_FALSE(fresh.building);
    EXPECT_EQ(fresh.bars, old.bars);
    ASSERT_EQ(fresh.revisions.size(), old.revisions.size());
    for (size_t i = 0; i < fresh.revisions.size(); ++i)
        EXPECT_GT(fresh.revisions[i], old.revisions[i]);
}
TEST_F(FlowStoreTest, CorruptAndTornDaysAreNeverServedAndRebuildOnRestart) {
    fixture();
    FlowStore store(options());
    ASSERT_EQ(store.runPass().built, 2);
    const auto path = store.filePath("BTC-USD", Epoch);
    for (bool truncate : {false, true}) {
        auto damaged = bytes(path);
        if (truncate)
            damaged.truncate(damaged.size() / 2);
        else
            damaged[damaged.size() - 1] ^= 1;
        QFile f(QString::fromStdString(path.string()));
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        ASSERT_EQ(f.write(damaged), damaged.size());
        f.close();
        const auto refused = store.window("BTC-USD", Epoch, Epoch + 2 * Hour);
        EXPECT_TRUE(refused.building);
        EXPECT_TRUE(refused.bars.empty());
        FlowStore restarted(options());
        EXPECT_EQ(restarted.runPass().built, 2);
        EXPECT_FALSE(restarted.window("BTC-USD", Epoch, Epoch + 2 * Hour).building);
    }
}
TEST_F(FlowStoreTest, MissingHoursAndUnsealedHourReturnBuildingAndAreNotWritten) {
    fixture();
    through = Epoch + Hour - 1;
    FlowStore store(options());
    EXPECT_TRUE(store.window("BTC-USD", Epoch, Epoch + 2 * Hour).building);
    EXPECT_EQ(store.runPass().built, 0);
    EXPECT_FALSE(fs::exists(store.filePath("BTC-USD", Epoch)));
    through = Epoch + Hour;
    EXPECT_EQ(store.runPass().built, 1);
    EXPECT_FALSE(store.window("BTC-USD", Epoch, Epoch + Hour).building);
    EXPECT_TRUE(store.window("BTC-USD", Epoch + Hour, Epoch + 2 * Hour).building);
    const auto data = bytes(store.filePath("BTC-USD", Epoch));
    const auto day = flow::decodeFlowDay({reinterpret_cast<const uint8_t *>(data.constData()), size_t(data.size())});
    for (const auto &b : day.bars)
        EXPECT_LT(b.startMs, Epoch + Hour);
    through = Epoch + 2 * Hour;
    EXPECT_EQ(store.runPass().built, 1);
    FlowStore restarted(options());
    EXPECT_EQ(restarted.runPass().built, 0);
    EXPECT_FALSE(restarted.window("BTC-USD", Epoch, Epoch + 2 * Hour).building);
}
TEST_F(FlowStoreTest, EmptySealedHoursRemainCompleteAcrossRestart) {
    Journal j(journal);
    j.snapshot(0);
    j.heartbeat(3 * Hour + Minute);
    j.close();
    through = Epoch + 3 * Hour;
    FlowStore store(options());
    EXPECT_EQ(store.runPass().built, 3);
    FlowStore restarted(options());
    EXPECT_EQ(restarted.runPass().built, 0);
    const auto reply = restarted.window("BTC-USD", Epoch, Epoch + 3 * Hour);
    EXPECT_FALSE(reply.building);
    EXPECT_TRUE(reply.bars.empty());
}
TEST_F(FlowStoreTest, RefusesOverlapsAndSymlinksWithEitherSource) {
    for (const auto &forbidden : {hmc2, hmc2 / "flow", base, journal, journal / "flow"}) {
        auto o = options();
        o.root = forbidden;
        EXPECT_THROW(FlowStore{std::move(o)}, std::invalid_argument);
    }
    const auto link = base / "alias";
    fs::create_directory_symlink(hmc2, link);
    auto o = options();
    o.root = link / "flow";
    EXPECT_THROW(FlowStore{std::move(o)}, std::invalid_argument);
    EXPECT_EQ(recording::flowRootFor("", hmc2, journal), fs::weakly_canonical(fs::path(hmc2.string() + "-flow")));
    fixture();
    fs::create_directory(root);
    fs::create_directory_symlink(hmc2, root / "BTC-USD");
    FlowStore store(options());
    EXPECT_GT(store.runPass().failed, 0);
    EXPECT_TRUE(fs::is_empty(hmc2));
}
TEST_F(FlowStoreTest, BuildsNewestHourFirstAndStopJoinsDuringReplay) {
    fixture();
    auto o = options();
    std::promise<void> entered;
    auto ready = entered.get_future();
    std::atomic<int> records{0};
    o.afterRecordForTest = [&](std::stop_token stop) {
        if (++records != 2)
            return; // Past daily grid discovery, inside hour replay.
        entered.set_value();
        std::mutex mutex;
        std::condition_variable_any cv;
        std::unique_lock lock(mutex);
        cv.wait(lock, stop, [] { return false; });
    };
    FlowStore store(o);
    store.start();
    ASSERT_EQ(ready.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    const auto start = std::chrono::steady_clock::now();
    store.stop();
    store.stop();
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(2));
    EXPECT_FALSE(fs::exists(store.filePath("BTC-USD", Epoch)));
    o.afterRecordForTest = {};
    int commits = 0;
    o.beforeCommitForTest = [&](const fs::path &) {
        if (++commits == 2)
            throw std::runtime_error("simulated interruption");
    };
    FlowStore partial(o);
    const auto pass = partial.runPass();
    EXPECT_EQ(pass.built, 1);
    EXPECT_EQ(pass.failed, 1);
    EXPECT_FALSE(partial.window("BTC-USD", Epoch + Hour, Epoch + 2 * Hour).building);
    EXPECT_TRUE(partial.window("BTC-USD", Epoch, Epoch + Hour).building);
    FlowStore restarted(options());
    EXPECT_EQ(restarted.runPass().built, 1);
}
TEST_F(FlowStoreTest, MismatchedCompletionSidecarIsRefusedAndRebuilt) {
    fixture();
    FlowStore store(options());
    ASSERT_EQ(store.runPass().built, 2);
    auto path = store.filePath("BTC-USD", Epoch);
    path += ".state";
    QFile f(QString::fromStdString(path.string()));
    ASSERT_TRUE(f.open(QIODevice::ReadWrite));
    ASSERT_TRUE(f.seek(4));
    ASSERT_EQ(f.write("\xff", 1), 1);
    f.close();
    EXPECT_TRUE(store.window("BTC-USD", Epoch, Epoch + Hour).bars.empty());
    EXPECT_EQ(store.runPass().built, 2);
}
TEST_F(FlowStoreTest, NewJournalSegmentExtendsDayWithoutRebuildingUnchangedHours) {
    fixture();
    FlowStore store(options());
    ASSERT_EQ(store.runPass().built, 2);
    const auto before = store.window("BTC-USD", Epoch, Epoch + 2 * Hour);
    Journal next(journal);
    next.snapshot(2 * Hour + 2 * Minute);
    next.trades(2 * Hour + 3 * Minute, {trade(2 * Hour + 3 * Minute, "6", 100006)});
    next.heartbeat(3 * Hour + Minute + 1);
    next.close();
    through = Epoch + 3 * Hour;
    EXPECT_TRUE(store.window("BTC-USD", Epoch, Epoch + 3 * Hour).building);
    EXPECT_EQ(store.runPass().built, 1);
    const auto after = store.window("BTC-USD", Epoch, Epoch + 3 * Hour);
    EXPECT_FALSE(after.building);
    EXPECT_EQ(after.bars.size(), before.bars.size() + 1);
    ASSERT_FALSE(after.bars.empty());
    EXPECT_EQ(after.bars.back().startMs, Epoch + 2 * Hour + 3 * Minute);
}
TEST_F(FlowStoreTest, LoadsFlowDirectoryConfigAndKeepsEmptyDefault) {
    ServerConfig cfg;
    EXPECT_TRUE(cfg.recording.flowDir.empty());
    const auto path = base / "server.yaml";
    {
        std::ofstream f(path);
        f << "server:\n  recording:\n    flow_dir: " << root.string() << "\n";
    }
    ASSERT_TRUE(ConfigLoader::loadServerConfig(path.string(), &cfg));
    EXPECT_EQ(cfg.recording.flowDir, root.string());
}
TEST_F(FlowStoreTest, ChangedJournalDuringReplayDiscardsBuild) {
    fixture();
    auto o = options();
    int records = 0;
    o.afterRecordForTest = [&](std::stop_token) {
        if (++records == 2)
            changeFingerprint();
    };
    FlowStore raced(o);
    const auto pass = raced.runPass();
    EXPECT_EQ(pass.failed, 0);
    EXPECT_EQ(pass.built, 0);
    EXPECT_FALSE(fs::exists(raced.filePath("BTC-USD", Epoch)));
    FlowStore retry(options());
    EXPECT_EQ(retry.runPass().built, 2);
    EXPECT_FALSE(retry.window("BTC-USD", Epoch, Epoch + 2 * Hour).building);
}
TEST_F(FlowStoreTest, InterruptedSourceRebuildResumesWithSameRevisionGeneration) {
    fixture();
    FlowStore original(options());
    ASSERT_EQ(original.runPass().built, 2);
    const auto old = original.window("BTC-USD", Epoch, Epoch + 2 * Hour);
    changeFingerprint();
    auto o = options();
    int commits = 0;
    o.beforeCommitForTest = [&](const fs::path &) {
        if (++commits == 2)
            throw std::runtime_error("simulated rebuild interruption");
    };
    FlowStore interrupted(o);
    EXPECT_EQ(interrupted.runPass().built, 1);
    FlowStore resumed(options());
    EXPECT_EQ(resumed.runPass().built, 1);
    const auto fresh = resumed.window("BTC-USD", Epoch, Epoch + 2 * Hour);
    ASSERT_EQ(fresh.revisions.size(), old.revisions.size());
    for (size_t i = 0; i < fresh.revisions.size(); ++i)
        EXPECT_EQ(fresh.revisions[i], old.revisions[i] + 1);
}
TEST_F(FlowStoreTest, ActiveTailAppendsDuringBuildAndServeDoNotStarveSealedHours) {
    through = Epoch + Hour;
    Journal live(journal);
    live.snapshot(0);
    live.trades(1000, {trade(800, "1", 100000)});
    live.heartbeat(Hour + Minute + 1);
    live.writer.flush();
    auto o = options();
    int records = 0;
    o.afterRecordForTest = [&](std::stop_token) {
        if (++records == 2) {
            live.heartbeat(Hour + 2 * Minute);
            live.writer.flush();
        }
    };
    FlowStore store(o);
    ASSERT_EQ(store.runPass().built, 1);
    live.heartbeat(Hour + 3 * Minute);
    live.writer.flush();
    const auto reply = store.window("BTC-USD", Epoch, Epoch + Hour);
    EXPECT_FALSE(reply.building);
    ASSERT_EQ(reply.bars.size(), 1);
    EXPECT_EQ(reply.bars[0].count, 1);
    FlowStore restart(options());
    EXPECT_EQ(restart.runPass().built, 0);
    live.close();
    EXPECT_FALSE(restart.window("BTC-USD", Epoch, Epoch + Hour).building);
}
TEST_F(FlowStoreTest, OpenJournalMustProveReceiveTailBeforePublishing) {
    through = Epoch + Hour;
    Journal live(journal);
    live.snapshot(0);
    live.trades(1000, {trade(800, "1", 100000)});
    live.heartbeat(Hour + 1000);
    live.writer.flush();
    FlowStore store(options());
    EXPECT_EQ(store.runPass().built, 0);
    EXPECT_TRUE(store.window("BTC-USD", Epoch, Epoch + Hour).building);
    EXPECT_FALSE(fs::exists(store.filePath("BTC-USD", Epoch)));
    live.trades(Hour + 2000, {trade(Hour - 100, "2", 100001)});
    live.heartbeat(Hour + Minute + 1);
    live.writer.flush();
    ASSERT_EQ(store.runPass().built, 1);
    const auto reply = store.window("BTC-USD", Epoch, Epoch + Hour);
    EXPECT_FALSE(reply.building);
    EXPECT_EQ(reply.bars.size(), 2);
    live.close();
}
} // namespace
