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
    std::vector<recording::LiveView> views;
    std::vector<heatmap_window::UpdatePtr> updates;
    void SetUp() override {
        QObject::connect(&processor, &DataProcessor::recordingViewNeeded, &processor,
                         [this](const auto& view) { views.push_back(view); });
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
        p.layer = "near";
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

TEST_F(RecordingDataProcessor, LiveAdvancesAndStaleGenerationCannotOverwriteIt) {
    processor.setRecordingCapability(true);
    events(180);
    auto history = page(requests.back());
    processor.onRecordingHistoryReceived(history);
    ASSERT_EQ(views.size(), 1);
    EXPECT_EQ(views.back().generation, history.bandGeneration);
    EXPECT_EQ(views.back().band.tick, history.bandTick);
    auto live = history;
    live.status = "complete";
    live.requestId.clear();
    live.columns[0].bucketStartMs += 60'000;
    live.columns[0].intensity.fill('\x55');
    processor.onRecordingLiveReceived(live);
    ASSERT_FALSE(updates.empty());
    EXPECT_EQ(updates.back()->windowEndMs, live.columns[0].bucketStartMs);
    EXPECT_EQ(updates.back()->liveBucketMs, live.columns[0].bucketStartMs);
    EXPECT_TRUE(updates.back()->pinnedToLive);
    auto count = updates.size();
    auto stale = live;
    --stale.bandGeneration;
    stale.columns[0].bucketStartMs += 60'000;
    processor.onRecordingLiveReceived(stale);
    EXPECT_EQ(updates.size(), count);
    // A queued history reply must neither rewind the edge nor replace live values.
    auto lateHistory = page(requests.back());
    lateHistory.columns[0].bucketStartMs = live.columns[0].bucketStartMs;
    lateHistory.columns[0].intensity.fill('\x11');
    processor.onRecordingHistoryReceived(lateHistory);
    EXPECT_GE(updates.back()->windowEndMs, live.columns[0].bucketStartMs);
    for (size_t i = count; i < updates.size(); ++i)
        for (const auto& write : updates[i]->writes)
            if (write.bucketStartMs == live.columns[0].bucketStartMs)
                EXPECT_EQ(write.intensity, live.columns[0].intensity);
}

TEST_F(RecordingDataProcessor, RebandAndReconnectRegisterFreshConfirmedViews) {
    processor.setRecordingCapability(true);
    events(180);
    processor.onRecordingHistoryReceived(page(requests.back()));
    ASSERT_EQ(views.size(), 1);
    const auto generation = views.back().generation;
    processor.setHeatmapViewport(6'000'000, 12'000'000, false, 20000, 20100, 1000, 500);
    events(180);
    EXPECT_EQ(views.size(), 1); // wait for authoritative native grid before registering
    processor.onRecordingHistoryReceived(page(requests.back()));
    ASSERT_EQ(views.size(), 2);
    EXPECT_GT(views.back().generation, generation);
    const auto beforeReconnect = views.back().generation;
    processor.setRecordingConnected(false);
    processor.setRecordingConnected(true);
    processor.setRecordingCapability(true);
    events(180);
    processor.onRecordingHistoryReceived(page(requests.back()));
    ASSERT_EQ(views.size(), 3);
    EXPECT_GT(views.back().generation, beforeReconnect);
}

TEST_F(RecordingDataProcessor, HistoryCanFinalizeMissedLiveCommit) {
    processor.setRecordingCapability(true);
    events(180);
    auto history = page(requests.back());
    processor.onRecordingHistoryReceived(history);
    auto live = history;
    live.status = "complete";
    live.columns[0].bucketStartMs += 60'000;
    live.columns[0].observedMs = 30000;
    live.columns[0].flags = recording::kProvisional;
    live.columns[0].intensity.fill('\x55');
    processor.onRecordingLiveReceived(live);
    auto final = page(requests.back());
    final.columns[0].bucketStartMs = live.columns[0].bucketStartMs;
    final.columns[0].observedMs = 60000;
    final.columns[0].intensity.fill('\x33');
    processor.onRecordingHistoryReceived(final);
    ASSERT_FALSE(updates.empty());
    const auto& writes = updates.back()->writes;
    const auto found = std::find_if(writes.begin(), writes.end(), [&](const auto& w) {
        return w.bucketStartMs == live.columns[0].bucketStartMs;
    });
    ASSERT_NE(found, writes.end());
    EXPECT_EQ(found->intensity, final.columns[0].intensity);
}

TEST_F(RecordingDataProcessor, ViewRejectionRetriesWithBackoffAndDropsStaleErrors) {
    processor.setRecordingCapability(true);
    events(180);
    processor.onRecordingHistoryReceived(page(requests.back()));
    ASSERT_EQ(views.size(), 1);
    const auto generation = views.back().generation;
    processor.onRecordingViewError("BTC-USD", generation - 1, "capacity", "stale", 1000);
    events(1050);
    EXPECT_EQ(views.size(), 1);
    processor.onRecordingViewError("BTC-USD", generation, "capacity", "full", 1000);
    events(1100);
    ASSERT_EQ(views.size(), 2);
    EXPECT_EQ(views.back().generation, generation);
    processor.onRecordingViewError("BTC-USD", generation, "incompatible_grid", "changed", 1000);
    events(1100);
    EXPECT_EQ(views.size(), 2);
    events(1100);
    EXPECT_EQ(views.size(), 3);
    processor.onRecordingViewError("BTC-USD", generation, "unavailable", "stopped", 1000);
    processor.setRecordingConnected(false);
    events(100);
    EXPECT_EQ(views.size(), 3);
}

TEST_F(RecordingDataProcessor, LostFinalIsFetchedAfterLiveMovesToNextBucket) {
    processor.setRecordingCapability(true);
    events(180);
    auto history = page(requests.back());
    history.status = "complete";
    history.exhausted = true;
    history.scannedStartMs = history.oldestAvailableMs = 60000;
    processor.onRecordingHistoryReceived(history);
    ASSERT_EQ(requests.size(), 1);
    auto live = history;
    live.columns[0].bucketStartMs = 12'060'000;
    live.columns[0].observedMs = 59000;
    live.columns[0].flags = recording::kProvisional;
    processor.onRecordingLiveReceived(live);
    // Closing payload was coalesced by transport congestion. Next live bucket
    // must initiate an explicit history repair, even though the slot is cached.
    live.columns[0].bucketStartMs = 12'120'000;
    live.columns[0].observedMs = 1000;
    processor.onRecordingLiveReceived(live);
    events(2200);
    ASSERT_EQ(requests.size(), 2);
    EXPECT_EQ(requests.back().endTimeMs, 12'060'000);
    EXPECT_EQ(requests.back().count, 1);
    auto final = page(requests.back());
    final.columns[0].bucketStartMs = 12'060'000;
    final.columns[0].observedMs = 60000;
    final.columns[0].flags = 0;
    final.columns[0].intensity.fill('\x31');
    final.scannedStartMs = 12'060'000;
    final.scannedEndMs = 12'120'000;
    processor.onRecordingHistoryReceived(final);
    const auto& writes = updates.back()->writes;
    const auto found = std::find_if(writes.begin(), writes.end(), [&](const auto& w) {
        return w.bucketStartMs == 12'060'000;
    });
    ASSERT_NE(found, writes.end());
    EXPECT_EQ(found->intensity, final.columns[0].intensity);
    events(2200);
    EXPECT_EQ(requests.size(), 2); // repair completed; no paging/retry loop
}

TEST_F(RecordingDataProcessor, UnrepairableBucketStopsAfterThreeAttemptsAndNextBucketRepairs) {
    processor.setRecordingCapability(true);
    events(180);
    auto history = page(requests.back());
    history.status = "complete";
    history.exhausted = true;
    history.scannedStartMs = history.oldestAvailableMs = 60000;
    processor.onRecordingHistoryReceived(history);
    auto live = history;
    live.columns[0].observedMs = 59000;
    live.columns[0].flags = recording::kProvisional;
    for (const auto bucket : {12'060'000, 12'120'000, 12'180'000}) {
        live.columns[0].bucketStartMs = bucket;
        processor.onRecordingLiveReceived(live);
    }
    for (int attempt = 0; attempt < 3; ++attempt) {
        events(attempt == 2 ? 4400 : 2300);
        ASSERT_EQ(requests.size(), static_cast<size_t>(attempt + 2));
        EXPECT_EQ(requests.back().endTimeMs, 12'060'000);
        EXPECT_EQ(requests.back().count, 1);
        auto response = page(requests.back());
        response.columns.clear();
        if (attempt == 0) {
            response.status = "complete"; // no column was ever committed
            response.scannedStartMs = 12'060'000;
            response.scannedEndMs = 12'120'000;
        } else if (attempt == 1) {
            response.status = "budget"; // must not bypass per-bucket attempts via 250ms retry
            response.scannedStartMs = response.scannedEndMs = 0;
        } else response.status = "io_error";
        processor.onRecordingHistoryReceived(response);
    }
    events(8400);
    ASSERT_EQ(requests.size(), 5);
    EXPECT_EQ(requests.back().endTimeMs, 12'120'000);
    auto final = page(requests.back());
    final.status = "complete";
    final.columns[0].bucketStartMs = 12'120'000;
    final.columns[0].observedMs = 60000;
    processor.onRecordingHistoryReceived(final);
    processor.setRecordingConnected(false);
}

TEST_F(RecordingDataProcessor, FinalRepairCancelsASupersededHistoryBudgetTimer) {
    processor.setRecordingCapability(true);
    events(180);
    auto history = page(requests.back());
    processor.onRecordingHistoryReceived(history); // starts an ordinary continuation
    ASSERT_EQ(requests.size(), 2);
    auto live = history;
    live.status = "complete";
    live.columns[0].observedMs = 59000;
    live.columns[0].flags = recording::kProvisional;
    live.columns[0].bucketStartMs = 12'060'000;
    processor.onRecordingLiveReceived(live);
    live.columns[0].bucketStartMs += 60000;
    processor.onRecordingLiveReceived(live);
    auto budget = page(requests.back());
    budget.columns.clear();
    budget.scannedStartMs = budget.scannedEndMs = 0;
    processor.onRecordingHistoryReceived(budget); // schedules 250ms retry
    // Deliver the final-repair timeout in that 250ms gap, without wall-clock races.
    QTimer* finalTimer = nullptr;
    for (auto* timer : processor.findChildren<QTimer*>())
        if (timer->isActive() && timer->interval() == 2000) finalTimer = timer;
    ASSERT_NE(finalTimer, nullptr);
    ASSERT_TRUE(QMetaObject::invokeMethod(finalTimer, "timeout", Qt::DirectConnection));
    ASSERT_EQ(requests.size(), 3);
    ASSERT_EQ(requests.back().count, 1);
    auto failed = page(requests.back());
    failed.status = "io_error";
    processor.onRecordingHistoryReceived(failed);
    events(350);
    EXPECT_EQ(requests.size(), 3); // stale 250ms timer cannot bypass repair backoff
    processor.setRecordingConnected(false);
}
