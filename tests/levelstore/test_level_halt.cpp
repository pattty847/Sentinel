#include "LevelTestSupport.hpp"
#include "RestartJournal.hpp"
#include "roller/Roller.hpp"
#include "levelstore/HaltRecord.hpp"
#include "roller/AnchorStore.hpp"

#include <gtest/gtest.h>
#include <fstream>
#include <map>
using namespace sentinel;
using namespace levelstore_test;
using nlohmann::json;
namespace {
std::map<std::string, std::string> tree(const fs::path &root, const char *ext) {
    std::map<std::string, std::string> result;
    if (fs::exists(root))
        for (const auto &f : fs::recursive_directory_iterator(root))
            if (f.is_regular_file() && (std::string(ext).empty() || f.path().extension() == ext)) {
                std::ifstream in(f.path(), std::ios::binary);
                result[fs::relative(f.path(), root).string()] = {std::istreambuf_iterator<char>(in), {}};
            }
    return result;
}
json checkpoint(const fs::path &root) {
    std::ifstream in(root / "hmc/BTC-USD/roller.json");
    json j;
    in >> j;
    return j;
}
roller::RollOptions options(const fs::path &raw, const fs::path &root) {
    roller::RollOptions o{raw, root / "hmc", "BTC-USD", RestartJournal::Epoch, RestartJournal::Epoch + 90 * 60000};
    o.levelLogRoot = root / "log";
    return o;
}
} // namespace
TEST(LevelHalt, DurableHaltAndFreshRootRebuildMatchEveryFile) {
    TempDir raw, baseline, resumed, hmconly, rebuilt;
    RestartJournal journal(raw.path);
    // The halted root contains a preceding UTC day. Recovery must not resume it.
    journal.at(-1);
    auto previous = options(raw.path, resumed.path);
    previous.fromMs = RestartJournal::Epoch - 60000;
    previous.toMs = RestartJournal::Epoch;
    roller::roll(previous);
    auto priorHmc = previous;
    priorHmc.outputRoot = hmconly.path / "hmc";
    priorHmc.levelLogRoot.clear();
    roller::roll(priorHmc);
    journal.grow(30);
    auto normal = options(raw.path, baseline.path);
    normal.levelRepresentationVersionForTest = 2;
    roller::roll(normal); // The never-halted control has an incremental history.
    auto paused = options(raw.path, resumed.path);
    auto first = roller::roll(paused);
    const auto before = checkpoint(resumed.path);
    ASSERT_FALSE(first.at("cHalted"));
    ASSERT_TRUE(before.at("days").at(std::to_string(RestartJournal::Epoch)).contains("level"));
    journal.grow(45);
    roller::roll(normal);
    paused.levelScaleCapForTest = 7;
    const auto stopped = roller::roll(paused);
    ASSERT_TRUE(stopped.at("cHalted"));
    const auto halt = readHalt(paused.levelLogRoot, "BTC-USD");
    ASSERT_TRUE(halt);
    RecordProperty("durable_halt_record", halt->dump());
    for (const auto *key :
         {"representationVersion", "symbol", "file", "run", "block", "record", "sequence", "priceIncrement",
          "sizeIncrement", "priceDigits", "sizeDigits", "priceText", "sizeText", "reason", "committedCheckpoint"})
        EXPECT_TRUE(halt->contains(key)) << key;
    EXPECT_EQ(halt->at("committedCheckpoint"), before);
    EXPECT_FALSE(fs::path(halt->at("file").get<std::string>()).is_absolute());
    EXPECT_EQ(checkpoint(resumed.path).at("days").at(std::to_string(RestartJournal::Epoch)).at("level"),
              before.at("days").at(std::to_string(RestartJournal::Epoch)).at("level"));
    const auto frozen = tree(paused.levelLogRoot, ".lcl");
    const auto stoppedHmc = tree(resumed.path, ".hmc2");
    const auto stoppedBoundary = checkpoint(resumed.path);
    journal.grow(92); // New RAWL2 arrives after C halted, before the same-version restart.
    roller::roll(normal);
    int cCreated = 0;
    paused.onLevelRecorder = [&](auto *c) {
        if (c)
            ++cCreated;
    };
    EXPECT_TRUE(roller::roll(paused).at("cHalted"));
    EXPECT_EQ(cCreated, 0);
    EXPECT_NE(tree(resumed.path, ".hmc2"), stoppedHmc);
    EXPECT_NE(checkpoint(resumed.path), stoppedBoundary);
    EXPECT_EQ(checkpoint(resumed.path).at("days").at(std::to_string(RestartJournal::Epoch)).at("level"),
              before.at("days").at(std::to_string(RestartJournal::Epoch)).at("level"));
    EXPECT_EQ(tree(paused.levelLogRoot, ".lcl"), frozen);
    EXPECT_EQ(readHalt(paused.levelLogRoot, "BTC-USD"), halt);
    auto h = options(raw.path, hmconly.path);
    h.levelLogRoot.clear();
    roller::roll(h);
    EXPECT_EQ(tree(resumed.path, ".hmc2"), tree(hmconly.path, ".hmc2"));
    const auto haltedTree = tree(resumed.path, "");
    paused.levelScaleCapForTest = UINT32_MAX;
    paused.levelRepresentationVersionForTest = 2;
    EXPECT_TRUE(roller::roll(paused).at("cHalted"));
    EXPECT_EQ(tree(resumed.path, ""), haltedTree);
    TempDir movedRaw;
    fs::copy(raw.path, movedRaw.path, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
    auto fixed = options(movedRaw.path, rebuilt.path);
    fixed.levelRepresentationVersionForTest = 2;
    EXPECT_FALSE(roller::roll(fixed).at("cHalted"));
    EXPECT_EQ(fixed.fromMs % 86400000, 0); // Rebuild starts at the affected UTC day start.
    EXPECT_EQ(tree(rebuilt.path, ""), tree(baseline.path, ""));
    EXPECT_EQ(tree(resumed.path, ""), haltedTree);
    EXPECT_EQ(readHalt(paused.levelLogRoot, "BTC-USD"), halt);
    RestartJournal other(raw.path, true, "ETH-USD");
    other.grow(92);
    auto isolated = options(raw.path, resumed.path);
    isolated.product = "ETH-USD";
    auto control = options(raw.path, baseline.path);
    control.product = "ETH-USD";
    EXPECT_FALSE(roller::roll(isolated).at("cHalted"));
    EXPECT_FALSE(roller::roll(control).at("cHalted"));
    EXPECT_EQ(tree(resumed.path / "hmc/ETH-USD", ""), tree(baseline.path / "hmc/ETH-USD", ""));
    EXPECT_EQ(tree(resumed.path / "log/ETH-USD", ""), tree(baseline.path / "log/ETH-USD", ""));
}

TEST(LevelHalt, OversizedRecordIsRejectedBeforeParsing) {
    TempDir t;
    fs::create_directories(t.path / "BTC-USD");
    const auto p = t.path / "BTC-USD/halt.json";
    std::ofstream(p).close();
    fs::resize_file(p, kMaxHaltBytes + 1);
    try { (void)readHalt(t.path, "BTC-USD"); FAIL(); }
    catch (const std::runtime_error &e) { EXPECT_EQ(std::string(e.what()), "halt record exceeds 64 MiB bound"); }
}
