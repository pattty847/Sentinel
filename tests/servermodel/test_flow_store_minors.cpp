#include "servermodel/FlowStore.hpp"
#include "roller/JournalReader.hpp"
#include "roller/fixtures/FlowJournal.hpp"
#include <gtest/gtest.h>
#include <QTemporaryDir>

namespace fs = std::filesystem;
using namespace flow_fixture;

TEST(FlowStoreMinors, FirstJournalDayKeepsSecondPrefixPartial) {
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    const fs::path base = temp.path().toStdString();
    const auto journal = base / "journal";
    fs::create_directory(journal);
    Journal fixture(journal);
    fixture.snapshot(-20 * Hour + 53 * Minute);
    fixture.heartbeat(Minute + 1);
    fixture.close();
    recording::FlowStore::Options options;
    options.root = base / "flow";
    options.hmc2Root = base / "hmc2";
    options.journalRoot = journal;
    options.symbols = {"BTC-USD"};
    options.hourThrough = [](const std::string&) { return int64_t{0}; };
    options.nowMs = [] { return Epoch + 2 * Hour; };
    recording::FlowStore store(options);
    ASSERT_EQ(store.runPass().failed, 0u);
    const auto from = Epoch - 24 * Hour + 2 * Hour;
    EXPECT_EQ(store.window("BTC-USD", from, from + Hour).ranges.front().status, "unrecorded");
    EXPECT_EQ(store.buildWindow("BTC-USD", from, from + Hour).ranges.front().status, "partial");
    EXPECT_EQ(store.buildWindow("BTC-USD", from - 24 * Hour, from - 23 * Hour).ranges.front().status,
              "unrecorded");
}

TEST(FlowStoreMinors, FirstJournalWithinReceiveTailKeepsPreviousHourBuilding) {
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    const fs::path base = temp.path().toStdString();
    const auto journal = base / "journal";
    fs::create_directory(journal);
    Journal fixture(journal);
    fixture.snapshot(-20 * Hour + 30'000);
    fixture.close();
    recording::FlowStore::Options options;
    options.root = base / "flow";
    options.hmc2Root = base / "hmc2";
    options.journalRoot = journal;
    options.symbols = {"BTC-USD"};
    options.hourThrough = [](const std::string&) { return int64_t{0}; };
    options.nowMs = [] { return Epoch + 2 * Hour; };
    recording::FlowStore store(options);
    ASSERT_EQ(store.runPass().failed, 0u);
    const auto from = Epoch - 24 * Hour + 3 * Hour;
    EXPECT_EQ(store.window("BTC-USD", from, from + Hour).ranges.front().status, "building");
}

TEST(FlowStoreMinors, JournalCatalogHonorsCancellationDuringScan) {
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    const fs::path journal = fs::path(temp.path().toStdString()) / "journal";
    fs::create_directory(journal);
    Journal fixture(journal);
    fixture.snapshot(0);
    fixture.close();
    std::stop_source stop;
    stop.request_stop();
    EXPECT_THROW((sentinel::roller::JournalReader(journal, "BTC-USD", std::nullopt,
                                                  stop.get_token())), std::runtime_error);
}

TEST(FlowStoreMinors, StoppedCatalogDoesNotPublishSourceProof) {
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    const fs::path base = temp.path().toStdString();
    const auto journal = base / "journal";
    fs::create_directory(journal);
    Journal fixture(journal);
    fixture.snapshot(0);
    fixture.close();
    std::stop_source stop;
    recording::FlowStore::Options options;
    options.root = base / "flow";
    options.hmc2Root = base / "hmc2";
    options.journalRoot = journal;
    options.symbols = {"BTC-USD"};
    options.hourThrough = [](const std::string&) { return int64_t{0}; };
    options.nowMs = [] { return Epoch + 2 * Hour; };
    options.afterCatalogConstructForTest = [&] { stop.request_stop(); };
    recording::FlowStore store(options);
    const auto result = store.runPass(stop.get_token());
    EXPECT_TRUE(result.stopped);
    EXPECT_EQ(result.failed, 0u);
    EXPECT_EQ(store.window("BTC-USD", Epoch - 24 * Hour, Epoch - 23 * Hour).ranges.front().status,
              "building");
}
