#include <gtest/gtest.h>
#include <QCoreApplication>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QTimer>
#include <QTemporaryFile>
#include "ConfigLoader.hpp"
#include "render/DataProcessor.hpp"
#include "render/HeatmapStreamService.hpp"
#include "render/GridViewState.hpp"

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
    int rangeResets = 0;
    void SetUp() override {
        QObject::connect(&processor, &DataProcessor::heatmapRangeReset, &processor,
                         [this](auto...) { ++rangeResets; });
        QObject::connect(&processor, &DataProcessor::recordingViewNeeded, &processor,
                         [this](const auto& view) { views.push_back(view); });
        QObject::connect(&processor, &DataProcessor::recordingHistoryFetchNeeded, &processor,
                         [this](const auto& request) { requests.push_back(request); });
        QObject::connect(&processor, &DataProcessor::heatmapWindowUpdated, &processor,
                         [this](auto update) { updates.push_back(std::move(update)); });
        processor.setHeatmapGridDimensions(1024, 2048);
        processor.setActiveSymbol("BTC-USD");
        processor.setServerTimeframe(60'000);
        processor.setRecordingConfig(true, 2);
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
    EXPECT_EQ(requests[0].count, 116);
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
    EXPECT_EQ(rangeResets, 0);
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

TEST_F(RecordingDataProcessor, VisibleFirstPagePublishesBeforeOlderBackfill) {
    processor.setRecordingCapability(true);
    events(180);
    ASSERT_EQ(requests.size(), 1);
    EXPECT_EQ(requests[0].count, 116);
    auto first = page(requests[0]);
    const auto sample = first.columns.front();
    first.columns.clear();
    for (int64_t bucket = 6'000'000; bucket < 12'000'000; bucket += 60'000) {
        auto column = sample;
        column.bucketStartMs = bucket;
        first.columns.push_back(std::move(column));
    }
    first.scannedStartMs = 6'000'000;
    first.scannedEndMs = 12'060'000;
    first.nextEndMs = 5'940'000;
    processor.onRecordingHistoryReceived(first);
    ASSERT_FALSE(updates.empty());
    EXPECT_TRUE(updates.back()->full);
    ASSERT_EQ(requests.size(), 2);
    EXPECT_EQ(requests[1].endTimeMs, 5'940'000);
    EXPECT_GT(requests[1].count, requests[0].count);
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
    processor.setRecordingConfig(false, 2);
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

TEST(RecordingStartup, WaitsForLiveMidBeforeFirstViewportRequest) {
    HeatmapStreamService service;
    service.init(1024, 2048, 60'000, 2);
    service.setRecordingMode(true);
    service.setInitialPricePct(5);
    GridViewState view;
    view.setViewport(6'000'000, 12'000'000, 62'000, 103'000);
    const auto version = view.getViewportVersion();
    EXPECT_FALSE(service.recordingViewportReady(&view));
    service.setLiveBook(83'186, 83'188, &view);
    EXPECT_TRUE(service.recordingViewportReady(&view));
    EXPECT_GT(view.getViewportVersion(), version);
    EXPECT_DOUBLE_EQ((view.getMinPrice() + view.getMaxPrice()) * 0.5, 83'187);
    EXPECT_DOUBLE_EQ(view.getMaxPrice() - view.getMinPrice(), 2'050);
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
    auto awaitRequests = [&](size_t count, int deadlineMs) {
        QElapsedTimer elapsed;
        elapsed.start();
        while (requests.size() < count && elapsed.elapsed() < deadlineMs) events(20);
        return requests.size() == count;
    };
    for (int attempt = 0; attempt < 3; ++attempt) {
        // Qt's coarse 2s polling can fire before a backoff expires and defer
        // the request to the next poll. Assert behavior, not an exact wakeup.
        ASSERT_TRUE(awaitRequests(static_cast<size_t>(attempt + 2), attempt == 2 ? 7500 : 5500));
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
    ASSERT_TRUE(awaitRequests(5, 11500));
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


TEST_F(RecordingDataProcessor, RebandsKeepPublishedPictureUntilLatestViewIsCovered) {
    processor.setRecordingCapability(true);
    events(180);
    ASSERT_EQ(requests.size(), 1);
    auto original = page(requests.back());
    original.status = "complete";
    original.exhausted = true;
    original.scannedStartMs = original.oldestAvailableMs = 60'000;
    original.columns[0].intensity.fill('\x11');
    processor.onRecordingHistoryReceived(original);
    ASSERT_EQ(updates.size(), 1);
    const auto displayed = updates.back();

    auto bandPage = [&](const auto& request, double lo) {
        auto p = page(request);
        p.bandLo = lo;
        p.columns[0].minPrice = lo;
        p.columns[0].maxPrice = lo + p.bandRows * p.bandTick;
        p.columns[0].intensity.fill('\x22');
        return p;
    };
    processor.setHeatmapViewport(6'000'000, 12'000'000, false, 20000, 20100, 1000, 500);
    events(180);
    ASSERT_EQ(requests.size(), 2);
    EXPECT_EQ(updates.size(), 1); // no provisional-band blank update
    processor.onRecordingHistoryReceived(bandPage(requests.back(), 18000));
    EXPECT_EQ(updates.size(), 1); // first page is insufficient
    ASSERT_EQ(requests.size(), 3); // continuations still run while publication is held
    const auto stale = bandPage(requests.back(), 18000);

    processor.setHeatmapViewport(6'000'000, 12'000'000, false, 30000, 30100, 1000, 500);
    events(180);
    ASSERT_EQ(requests.size(), 4);
    const auto latestGeneration = requests.back().bandGeneration;
    processor.onRecordingHistoryReceived(stale);
    EXPECT_EQ(updates.size(), 1);
    processor.onRecordingHistoryReceived(bandPage(requests.back(), 28000));
    EXPECT_EQ(updates.size(), 1);
    ASSERT_EQ(requests.size(), 5);
    auto rest = bandPage(requests.back(), 28000);
    rest.columns.clear(); // known gaps are sufficient evidence for the rest
    rest.scannedStartMs = 6'000'000;
    rest.scannedEndMs = 11'940'000;
    rest.nextEndMs = 5'940'000;
    processor.onRecordingHistoryReceived(rest);
    ASSERT_EQ(updates.size(), 2);
    EXPECT_TRUE(updates.back()->full);
    EXPECT_EQ(updates.back()->bandGeneration, latestGeneration);
    EXPECT_DOUBLE_EQ(updates.back()->band.minPrice, 28000);
    ASSERT_EQ(updates.back()->writes.size(), 1024);
    EXPECT_EQ(displayed->bandGeneration, original.bandGeneration);
    EXPECT_DOUBLE_EQ(displayed->band.minPrice, original.bandLo);
    const auto& oldWrites = displayed->writes;
    auto oldColumn = std::find_if(oldWrites.begin(), oldWrites.end(), [](const auto& w) {
        return w.bucketStartMs == 12'000'000;
    });
    ASSERT_NE(oldColumn, oldWrites.end());
    EXPECT_EQ(oldColumn->intensity, QByteArray(4096, '\x11'));
    processor.setRecordingConnected(false);
}

TEST_F(RecordingDataProcessor, MutingCancelsPendingBandsAndRejectsRepliesThenResumesRecording) {
    processor.setRecordingCapability(true); // band timer pending
    processor.setHeatmapEnabled(false);
    processor.setHeatmapViewport(6'000'000, 12'000'000, false, 11000, 11100, 1000, 500);
    processor.refreshRecordingHistory();
    events(180);
    EXPECT_TRUE(requests.empty());
    processor.setHeatmapEnabled(true);
    events(180);
    ASSERT_EQ(requests.size(), 1);
    const auto stale = page(requests.back());
    processor.setHeatmapEnabled(false);
    const auto count = updates.size();
    processor.onRecordingHistoryReceived(stale);
    EXPECT_EQ(updates.size(), count);
    processor.setHeatmapViewport(6'000'000, 12'000'000, false, 12000, 12100, 1000, 500);
    events(180);
    EXPECT_EQ(requests.size(), 1);
    processor.setHeatmapEnabled(true);
    events(180);
    ASSERT_EQ(requests.size(), 2);
    EXPECT_GT(requests.back().bandGeneration, stale.bandGeneration);
    EXPECT_GT(requests.back().priceMin, requests.front().priceMin);
}

TEST_F(RecordingDataProcessor, MutingLegacySlicesAlsoStopsFetchesAndResumesWithoutChangingMode) {
    processor.setRecordingConfig(false, 2);
    HeatmapSlice live;
    live.symbol = "BTC-USD"; live.timeframeMs = 60'000; live.bucketStartMs = 12'000'000;
    live.gridWidth = 1024; live.gridHeight = 2048; live.minPrice = 8000; live.maxPrice = 12096;
    live.tickSize = 2; live.format = "u16"; live.column = QByteArray(4096, 0);
    processor.onHeatmapSliceReceived(live);
    ASSERT_FALSE(updates.empty());
    processor.setHeatmapEnabled(false);
    const auto count = updates.size();
    int fetches = 0;
    QObject::connect(&processor, &DataProcessor::heatmapHistoryFetchNeeded, &processor, [&](auto...) { ++fetches; });
    live.bucketStartMs += 60'000;
    processor.onHeatmapSliceReceived(live);
    EXPECT_EQ(updates.size(), count);
    processor.setHeatmapViewport(5'000'000, 11'000'000, false, 8000, 12096, 1000, 500);
    EXPECT_EQ(fetches, 0);
    processor.setHeatmapEnabled(true);
    EXPECT_GT(fetches, 0);
    live.bucketStartMs = 10'980'000;
    const auto resumed = updates.size();
    processor.onHeatmapSliceReceived(live);
    EXPECT_GT(updates.size(), resumed);
}
