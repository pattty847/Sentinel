#include <gtest/gtest.h>
#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include <QTemporaryFile>
#include "ConfigLoader.hpp"
#include "render/DataProcessor.hpp"

namespace {
void events(int ms) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}
class RecordingDataProcessor : public testing::Test {
protected:
    int argc = 1;
    char name[5] = "test";
    char* argv[2] = {name, nullptr};
    QCoreApplication app{argc, argv};
    DataProcessor processor;
    std::vector<protocol::recordingwire::Request> requests;
    std::vector<heatmap_window::UpdatePtr> updates;
    void SetUp() override {
        QObject::connect(&processor, &DataProcessor::recordingHistoryFetchNeeded, &processor,
                         [this](const auto& request) { requests.push_back(request); });
        QObject::connect(&processor, &DataProcessor::heatmapWindowUpdated, &processor,
                         [this](auto update) { updates.push_back(std::move(update)); });
        processor.setHeatmapGridDimensions(1024, 2048);
        processor.setActiveSymbol("BTC-USD");
        processor.setServerTimeframe(60'000);
        processor.setRecordingConfig(true, 2, 0.75);
        processor.setRecordingConnected(true);
        processor.setHeatmapViewport(6'000'000, 12'000'000, false, 10000, 10100, 1000, 500);
    }
    SentinelStreamClient::RecordingHistoryPage page(const protocol::recordingwire::Request& request) {
        SentinelStreamClient::RecordingHistoryPage p;
        p.symbol = "BTC-USD";
        p.requestId = QString::fromStdString(request.requestId);
        p.bandGeneration = request.bandGeneration;
        p.timeframeMs = request.timeframeMs;
        p.requestEndMs = request.endTimeMs;
        p.status = "budget";
        p.valueEncoding = "absolute_log_size";
        p.bandLo = 8000;
        p.bandTick = 2;
        p.bandRows = 2048;
        p.sizeFloor = 1e-6;
        p.codesPerOctave = 819;
        p.latestAvailableMs = 12'000'000;
        p.scannedStartMs = 11'940'000;
        p.scannedEndMs = 12'060'000;
        p.nextEndMs = 11'880'000;
        SentinelStreamClient::HeatmapHistoryColumn c;
        c.bucketStartMs = 12'000'000;
        c.minPrice = p.bandLo;
        c.maxPrice = p.bandLo + 2048 * p.bandTick;
        c.tickSize = p.bandTick;
        c.intensity = QByteArray(4096, 0);
        c.liquidity = QByteArray(4096, 0);
        c.validity = QByteArray(256, '\xff');
        p.columns.push_back(c);
        return p;
    }
};
}

TEST_F(RecordingDataProcessor, CapabilityGatesRequestsAndLegacySlicesStayOut) {
    events(180);
    EXPECT_TRUE(requests.empty());
    processor.setRecordingCapability(true);
    events(180);
    ASSERT_EQ(requests.size(), 1);
    EXPECT_GE(requests[0].priceMin, 0);
    EXPECT_FALSE(requests[0].displayTick.has_value());
    EXPECT_EQ(requests[0].rows, 2048);
    HeatmapSlice live;
    live.symbol = "BTC-USD";
    live.timeframeMs = 60'000;
    live.bucketStartMs = 12'060'000;
    live.gridWidth = 1024;
    live.gridHeight = 2048;
    live.minPrice = 8000;
    live.maxPrice = 12096;
    live.tickSize = 2;
    live.format = "u16";
    live.column = QByteArray(4096, 0);
    processor.onHeatmapSliceReceived(live);
    EXPECT_TRUE(updates.empty());
}

TEST_F(RecordingDataProcessor, PagingPinsAuthoritativeBandAndPriceOnlyRebandRejectsOldReply) {
    processor.setRecordingCapability(true);
    events(180);
    ASSERT_EQ(requests.size(), 1);
    auto first = page(requests.back());
    processor.onRecordingHistoryReceived(first);
    ASSERT_EQ(requests.size(), 2);
    EXPECT_EQ(requests[1].endTimeMs, first.nextEndMs);
    ASSERT_TRUE(requests[1].displayTick.has_value());
    EXPECT_DOUBLE_EQ(*requests[1].displayTick, first.bandTick);
    EXPECT_DOUBLE_EQ(requests[1].priceMin, first.bandLo);
    EXPECT_NE(requests[1].requestId, requests[0].requestId);
    const auto old = page(requests[1]);
    // Same time buckets, price-only exit from margin must invalidate the projection.
    processor.setHeatmapViewport(6'000'000, 12'000'000, false, 20000, 20100, 1000, 500);
    events(180);
    ASSERT_EQ(requests.size(), 3);
    EXPECT_GT(requests[2].bandGeneration, old.bandGeneration);
    ASSERT_FALSE(updates.empty());
    EXPECT_TRUE(updates.back()->full);
    const auto count = updates.size();
    processor.onRecordingHistoryReceived(old);
    EXPECT_EQ(updates.size(), count);
    processor.setRecordingConnected(false);
    processor.onRecordingHistoryReceived(page(requests.back()));
    EXPECT_EQ(updates.size(), count);
}

TEST_F(RecordingDataProcessor, BudgetWithoutProgressRetriesSameBoundaryWithNewId) {
    processor.setRecordingCapability(true);
    events(180);
    ASSERT_EQ(requests.size(), 1);
    auto budget = page(requests.back());
    budget.columns.clear();
    budget.scannedStartMs = budget.scannedEndMs = 0;
    budget.bandTick = 0; // discovery can precede band selection
    processor.onRecordingHistoryReceived(budget);
    events(280);
    ASSERT_EQ(requests.size(), 2);
    EXPECT_EQ(requests[1].endTimeMs, requests[0].endTimeMs);
    EXPECT_NE(requests[1].requestId, requests[0].requestId);
    EXPECT_TRUE(updates.empty());
}

TEST_F(RecordingDataProcessor, DefaultLegacyPayloadBytesStayUnchanged) {
    processor.setRecordingConfig(false, 2, 0.75);
    processor.setRecordingCapability(true);
    HeatmapSlice live;
    live.symbol = "BTC-USD";
    live.timeframeMs = 60'000;
    live.bucketStartMs = 12'000'000;
    live.gridWidth = 1024;
    live.gridHeight = 2048;
    live.minPrice = 8000;
    live.maxPrice = 12096;
    live.tickSize = 2;
    live.format = "u16";
    live.column = QByteArray(4096, '\x23');
    processor.onHeatmapSliceReceived(live);
    ASSERT_EQ(updates.size(), 1);
    EXPECT_EQ(updates[0]->valueEncoding, heatmap_window::ValueEncoding::LegacyIntensity);
    const auto& writes = updates[0]->writes;
    const auto found = std::find_if(writes.begin(), writes.end(), [&](const auto& write) {
        return write.bucketStartMs == live.bucketStartMs;
    });
    ASSERT_NE(found, writes.end());
    EXPECT_EQ(found->intensity, live.column);
    EXPECT_TRUE(found->validity.isEmpty());
    EXPECT_TRUE(requests.empty());
}

TEST(RecordingClientConfig, SourceDefaultsAndParsing) {
    EXPECT_EQ(ClientConfig{}.heatmap.source, "legacy");
    for (const auto* source : {"legacy", "recording", "typo"}) {
        QTemporaryFile file;
        ASSERT_TRUE(file.open());
        file.write(QByteArray("heatmap:\n  source: ") + source + "\n");
        file.flush();
        ClientConfig config;
        ASSERT_TRUE(ConfigLoader::loadClientConfig(file.fileName().toStdString(), &config));
        EXPECT_EQ(config.heatmap.source, std::string(source) == "recording" ? "recording" : "legacy");
    }
}
