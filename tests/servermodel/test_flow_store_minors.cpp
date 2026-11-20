#include "servermodel/FlowStore.hpp"
#include "roller/JournalReader.hpp"
#include "roller/fixtures/FlowJournal.hpp"
#include <gtest/gtest.h>
#include <QTemporaryDir>

namespace fs = std::filesystem;
using namespace flow_fixture;

TEST(FlowStoreMinors, FirstJournalWithinReceiveTailLeavesPreviousHourUnrecorded) {
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
    int64_t through = 0;
    options.hourThrough = [&](const std::string&) { return through; };
    options.nowMs = [] { return Epoch + 2 * Hour; };
    recording::FlowStore store(options);
    ASSERT_EQ(store.runPass().failed, 0u);
    const auto from = Epoch - 24 * Hour + 3 * Hour;
    EXPECT_EQ(store.window("BTC-USD", from, from + Hour).ranges.front().status, "unrecorded");
    EXPECT_EQ(store.buildWindow("BTC-USD", from, from + Hour).ranges.front().status,
              "unrecorded");
    through = Epoch;
    ASSERT_EQ(store.runPass().built, 20u);
    for (const auto leading : {Epoch - 24 * Hour, from}) {
        EXPECT_EQ(store.buildWindow("BTC-USD", leading, leading + Hour).ranges.front().status,
                  "unrecorded");
    }
}

TEST(FlowStoreMinors, JournalCatalogHonorsCancellationDuringScan) {
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    const fs::path journal = fs::path(temp.path().toStdString()) / "journal";
    fs::create_directory(journal);
    for (int run = 0; run < 2; ++run) {
        Journal fixture(journal);
        fixture.snapshot(run * Hour);
        fixture.close();
    }
    size_t files = 0;
    for (const auto& entry : fs::recursive_directory_iterator(journal))
        files += entry.is_regular_file() && entry.path().extension() == ".rawl2";
    ASSERT_GE(files, 2u);
    std::stop_source stop;
    int scanned = 0;
    auto afterHeader = [&] {
        if (++scanned == 1) stop.request_stop();
    };
    EXPECT_THROW((sentinel::roller::JournalReader(journal, "BTC-USD", std::nullopt,
                                                  stop.get_token(), afterHeader)), std::runtime_error);
    EXPECT_EQ(scanned, 1);
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
