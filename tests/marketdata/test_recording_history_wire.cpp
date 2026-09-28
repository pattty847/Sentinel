#include <gtest/gtest.h>
#include "protocol/RecordingHistoryWire.hpp"
#include "protocol/SentinelStreamClientParseHelpers.hpp"
#include <future>
#include <barrier>

using namespace protocol::recordingwire;

TEST(RecordingHistoryWire, RequestFieldsAndPageLimit) {
    nlohmann::json msg = {{"symbol", "BTC-USD"}, {"timeframe_ms", 300000},
                          {"end_time", 1230000}, {"count", 1024}, {"rows", 4096},
                          {"price_min", 90000.0}, {"price_max", 90100.0},
                          {"display_tick", 10.0}, {"request_id", "req-1"},
                          {"band_generation", 7u}};
    const auto q = parseRequest(msg);
    ASSERT_TRUE(q);
    EXPECT_EQ(q->symbol, "BTC-USD");
    EXPECT_EQ(q->timeframeMs, 300000);
    EXPECT_EQ(q->endTimeMs, 1230000);
    EXPECT_EQ(q->count, 2'000'000 / 4096);
    EXPECT_EQ(q->rows, 4096);
    EXPECT_DOUBLE_EQ(*q->displayTick, 10.0);
    EXPECT_EQ(q->requestId, "req-1");
    EXPECT_EQ(q->bandGeneration, 7u);
    msg["price_max"] = msg["price_min"];
    EXPECT_FALSE(parseRequest(msg));
}

TEST(RecordingHistoryWire, ChunkPadsTopRowsAndValidity) {
    Request q;
    q.symbol = "BTC-USD"; q.timeframeMs = 60000; q.rows = 5; q.count = 1;
    q.requestId = "r-2"; q.bandGeneration = 3;
    recording::BuildResult page;
    page.band = {100.0, 10.0, 3};
    page.layer = "deep";
    page.sizeScale = {1e-6, 819.0};
    page.scannedStartMs = 60'000; page.scannedEndMs = 120'000;
    page.nextEnd = 0; page.exhausted = true;
    page.oldestAvailableMs = 60'000; page.latestAvailableMs = 60'000;
    recording::ServedColumn col;
    col.bucketStartMs = 60'000;
    col.cells = {0x1234, 0, 0x8002};
    col.quantities = {2, 0, 3};
    col.validity = {0b00000101};
    col.observedMs = 45'000; col.flags = recording::kPartial;
    page.columns.push_back(col);
    const auto wire = buildChunk(q, page);
    EXPECT_EQ(wire.at("encoding"), "base64");
    EXPECT_EQ(wire.at("value_encoding"), "absolute_log_size");
    EXPECT_EQ(wire.at("request_id"), "r-2");
    EXPECT_EQ(wire.at("band_generation"), 3);
    EXPECT_EQ(wire.at("band_rows"), 5);
    EXPECT_EQ(wire.at("status"), "complete");
    EXPECT_EQ(wire.at("scanned_start"), 60'000);
    EXPECT_EQ(wire.at("latest_available_ms"), 60'000);
    const auto& item = wire.at("columns").at(0);
    EXPECT_EQ(item.at("observed_ms"), 45'000);
    EXPECT_EQ(item.at("flags"), recording::kPartial);
    EXPECT_EQ(QByteArray::fromBase64(QByteArray::fromStdString(item.at("validity").get<std::string>())).toHex(), "14");
    EXPECT_EQ(QByteArray::fromBase64(QByteArray::fromStdString(item.at("column").get<std::string>())).toHex(),
              "00000000341200000280");
}

TEST(RecordingHistoryWire, CoveredPaddingKeepsValidityButClearsValues) {
    Request q;
    q.symbol = "BTC-USD"; q.timeframeMs = 60000; q.rows = 5; q.count = 1;
    q.priceMin = 100.0; q.priceMax = 130.0; q.displayTick = 10.0;
    const auto request = buildRequest(q);
    EXPECT_GT(request.priceHi, q.priceMax);
    recording::BuildResult page;
    page.band = {100.0, 10.0, 5};
    recording::ServedColumn col;
    col.cells = {11, 22, 33, 44, 55};
    col.quantities = {1, 2, 3, 4, 5};
    col.validity = {0b00011111};
    page.columns.push_back(col);
    emptyPaddingValues(q, page);
    EXPECT_EQ(page.columns[0].cells, (std::vector<uint16_t>{0, 0, 33, 44, 55}));
    const auto wire = buildChunk(q, page);
    const auto& item = wire.at("columns").at(0);
    EXPECT_EQ(QByteArray::fromBase64(QByteArray::fromStdString(item.at("validity").get<std::string>())).toHex(), "1f");
    EXPECT_EQ(QByteArray::fromBase64(QByteArray::fromStdString(item.at("column").get<std::string>())).toHex(),
              "0000000021002c003700");
}

TEST(RecordingHistoryWire, StatusErrorsAndCapabilities) {
    EXPECT_STREQ(statusName(recording::BuildStatus::Budget), "budget");
    EXPECT_STREQ(statusName(recording::BuildStatus::Cancelled), "cancelled");
    EXPECT_STREQ(statusName(recording::BuildStatus::InvalidRequest), "invalid_request");
    EXPECT_STREQ(statusName(recording::BuildStatus::IncompatibleGrid), "incompatible_grid");
    EXPECT_STREQ(statusName(recording::BuildStatus::IoError), "io_error");
    const auto err = error("BTC-USD", "queue full", "r-3", 9);
    EXPECT_EQ(err.at("request_id"), "r-3");
    EXPECT_EQ(err.at("band_generation"), 9);
    ServerConfig cfg;
    cfg.heatmap.timeframesMs = {1000, 60000, 300000, 3600000, 7200000, 5400000};
    const auto capabilityOff = capability(cfg, false);
    EXPECT_FALSE(capabilityOff.at("available").get<bool>());
    EXPECT_EQ(capabilityOff.at("timeframes_ms"), nlohmann::json::array({60000, 300000, 3600000, 7200000}));
    const auto parsed = protocol::clientparse::parseServerConfig({{"recording", capability(cfg, true)}});
    EXPECT_TRUE(parsed.recording.available);
    EXPECT_TRUE(parsed.wasAdvertised("recording.available"));
    EXPECT_EQ(parsed.recording.layers, (std::vector<std::string>{"near", "deep"}));
    EXPECT_EQ(parsed.recording.timeframesMs.size(), 4u);
    const auto legacy = protocol::clientparse::parseServerConfig({{"heatmap", {{"grid_height", 2048}}}});
    EXPECT_FALSE(legacy.recording.available);
    EXPECT_FALSE(legacy.wasAdvertised("recording.available"));
}

TEST(RecordingHistoryWire, ReaderIsOwnedByItsWorkerThread) {
    const auto root = std::filesystem::temp_directory_path() / "sentinel-reader-owner-test";
    std::barrier ready(3), release(3);
    auto worker = [&] {
        auto* reader = &threadReader(root);
        ready.arrive_and_wait();
        release.arrive_and_wait();
        return reader;
    };
    auto a = std::async(std::launch::async, worker);
    auto b = std::async(std::launch::async, worker);
    ready.arrive_and_wait();
    release.arrive_and_wait();
    EXPECT_NE(a.get(), b.get());
    EXPECT_EQ(&threadReader(root), &threadReader(root));
}
