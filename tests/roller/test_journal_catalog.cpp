// A JournalReader catalog lives for the life of a roller product worker and of a
// FlowStore symbol, over thousands of RAWL2 files. It must keep only the header
// fields its readers use, not each file's parsed header.
#include "heap_in_use.hpp"
#include "roller/JournalReader.hpp"
#include <gtest/gtest.h>
#include <QTemporaryDir>

using namespace sentinel;
using namespace sentinel::roller;
using nlohmann::json;
namespace fs = std::filesystem;
namespace {
constexpr int64_t Epoch = 1'798'761'600'000; // 2027-01-01 UTC
constexpr int64_t HourMs = 3'600'000;
// About the size and shape of a real Coinbase product response (~70 fields).
json largeProductMetadata(const std::string& product) {
    json m = {{"product_id", product}, {"quote_increment", "0.01"}, {"base_increment", "0.00000001"}};
    for (int i = 0; i < 60; ++i)
        m["descriptive_field_" + std::to_string(i)] = "a value longer than the small-string buffer " + std::to_string(i);
    m["spot_margin_details"] = {{"base_currency_id", "BTC"}, {"instrument_id", "BTC_USDC"},
                                {"max_leverage", "10"}, {"min_trade_amount", "0.00000001"},
                                {"quote_currency_id", "USDC"}, {"tick_size", "0.01"}};
    return m;
}
// One run, one file per hour from firstHour (the writer rotates on the hour).
void writeRun(const fs::path& root, const json& metadata, int firstHour, int hours) {
    capture::WriterConfig c;
    c.root = QString::fromStdString(root.string());
    c.fsyncBlocks = 0;
    capture::Writer w(c, metadata);
    for (int h = firstHour; h < firstHour + hours; ++h) {
        const int64_t ms = Epoch + h * HourMs;
        w.append({capture::Kind::Frame, {ms * 1'000'000, (h + 1) * HourMs * 1'000'000}, 1,
                  R"({"channel":"heartbeats"})"});
    }
    w.close();
}
} // namespace

TEST(JournalCatalog, RetainedHeapPerFileStaysSmall) {
    if (!test::heapInUse()) GTEST_SKIP() << "no heap accounting on this platform";
    QTemporaryDir temp(QStringLiteral(ROLLER_TEST_ROOT "/catalog-XXXXXX"));
    ASSERT_TRUE(temp.isValid());
    const fs::path root = temp.path().toStdString();
    const json metadata = {{"product_metadata", largeProductMetadata("BTC-USD")}};
    writeRun(root, metadata, 0, 40);
    writeRun(root, metadata, 40, 40);
    { JournalReader warm(root, "BTC-USD"); } // one-time allocations (statics, locale) excluded

    const auto before = *test::heapInUse();
    JournalReader catalog(root, "BTC-USD");
    const auto after = *test::heapInUse();
    ASSERT_EQ(catalog.files().size(), 80u);
    const auto perFile = (int64_t(after.bytes) - int64_t(before.bytes)) / 80;
    // A parsed header of this size costs ~10 KiB and ~150 allocations per file.
    EXPECT_LT(perFile, 1024) << "catalog keeps " << perFile << " heap bytes per file";

    // A cursor over the catalog (FlowStore builds, roller catch-up) shares it.
    const auto beforeCopy = *test::heapInUse();
    JournalReader cursor(catalog, std::nullopt);
    const auto afterCopy = *test::heapInUse();
    EXPECT_LT(int64_t(afterCopy.bytes) - int64_t(beforeCopy.bytes), 4096)
        << "a cursor copies the catalog";
    EXPECT_EQ(&cursor.files(), &catalog.files());
}

// Every header value a reader uses equals the file's own header; files with the
// same metadata share one object instead of each keeping a parsed copy.
TEST(JournalCatalog, KeepsEveryHeaderValueReadersUseAndSharesMetadata) {
    QTemporaryDir temp(QStringLiteral(ROLLER_TEST_ROOT "/catalog-XXXXXX"));
    ASSERT_TRUE(temp.isValid());
    const fs::path root = temp.path().toStdString();
    auto metadataA = largeProductMetadata("BTC-USD");
    auto metadataB = metadataA;
    metadataB["price"] = "62880.9"; // a later run fetched different values
    writeRun(root, {{"product_metadata", metadataA}}, 0, 3);
    writeRun(root, {{"product_metadata", metadataB}}, 3, 2);
    const std::string kalshi = "KXTEST-1.5";
    const json instrument = {{"price_min", "0.0000"}, {"price_max", "1.0000"}, {"size_increment", "0.01"},
        {"price_ranges", json::array({{{"start", "0.0000"}, {"end", "1.0000"}, {"step", "0.0100"}}})}};
    {
        capture::WriterConfig c;
        c.root = QString::fromStdString(root.string());
        c.symbol = kalshi;
        c.fsyncBlocks = 0;
        capture::Writer w(c, {{"venue", "kalshi"}, {"product_metadata", {{"product_id", kalshi}}},
                              {"instrument", instrument}});
        for (int h = 0; h < 2; ++h)
            w.append({capture::Kind::CaptureStarted, {(Epoch + h * HourMs) * 1'000'000, (h + 1) * HourMs * 1'000'000},
                      1, "{}"});
        w.close();
    }
    for (const auto& product : {std::string("BTC-USD"), kalshi}) {
        JournalReader catalog(root, product);
        const auto& files = catalog.files();
        ASSERT_EQ(files.size(), product == kalshi ? 2u : 5u);
        for (const auto& f : files) {
            const auto h = capture::readHeader(QString::fromStdString(f.path.string()));
            EXPECT_EQ(f.run, h.at("run_id").get<std::string>());
            EXPECT_EQ(f.segment, h.at("segment").get<uint64_t>());
            EXPECT_EQ(f.firstBlock, h.at("first_block_ordinal").get<uint64_t>());
            EXPECT_EQ(f.runStartedNs, h.at("run_started_system_ns").get<int64_t>());
            EXPECT_EQ(f.openedNs, h.at("opened_system_ns").get<int64_t>());
            EXPECT_EQ(f.formatVersion, h.at("format_version").get<int>());
            EXPECT_EQ(f.venue, venueFromHeader(h));
            ASSERT_TRUE(f.metadata && f.instrument);
            EXPECT_EQ(f.metadata->dump(), h.at("product_metadata").dump());
            EXPECT_EQ(f.instrument->dump(), h.value("instrument", json()).dump());
            // Records carry the same header values as before.
            JournalReader one(catalog, JournalPos{product, f.run, f.firstBlock, 0});
            JournalRecord r;
            ASSERT_TRUE(one.next(r));
            EXPECT_EQ(r.pos.run, f.run);
            EXPECT_EQ(r.metadata, h.at("product_metadata"));
            EXPECT_EQ(r.version, h.at("format_version").get<int>());
            EXPECT_EQ(r.venue, venueFromHeader(h));
            EXPECT_EQ(r.instrument, h.value("instrument", json()));
        }
        // One metadata and one instrument object per distinct value, not per file.
        for (const auto& f : files) {
            EXPECT_EQ(f.instrument, files.front().instrument);
            for (const auto& g : files)
                EXPECT_EQ(f.metadata == g.metadata, f.run == g.run) << f.path << ' ' << g.path;
        }
    }
    JournalReader btc(root, "BTC-USD");
    EXPECT_EQ(*btc.files().front().metadata, metadataA);
    EXPECT_EQ(*btc.files().back().metadata, metadataB);
    EXPECT_TRUE(btc.files().front().instrument->is_null());
    EXPECT_EQ(*JournalReader(root, kalshi).files().front().instrument, instrument);
}
