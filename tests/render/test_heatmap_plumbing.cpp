#include "render/heatmap/HeatmapDataService.hpp"
#include "render/heatmap/HeatmapSettingsStore.hpp"
#include "lab/LabData.hpp"
#include "protocol/SentinelStreamClient.hpp"
#include "protocol/SentinelStreamClientTransport.hpp"
#include "ConfigLoader.hpp"
#include "../servermodel/FakeChunkTransport.hpp"
#include <QCoreApplication>
#include <QEvent>
#include <QTemporaryDir>
#include <QFile>
#include <QJsonArray>
#include <QDir>
#include <QPointer>
#include <stdexcept>
#include <gtest/gtest.h>
#include <limits>

namespace {
using namespace heatmap;
class HeatmapPlumbing : public testing::Test {
protected:
    int argc = 1;
    char name[5] = "test";
    char *argv[2] = {name, nullptr};
    QCoreApplication app{argc, argv};
    void drain(HeatmapDataService &service) {
        // Cross-thread barriers plus explicit MetaCall drains, no timed waits.
        for (int i = 0; i < 4; ++i) service.onData([] { QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall); });
    }
    protocol::chunkwire::Availability availability() {
        protocol::chunkwire::Availability a;
        a.symbol = "BTC-USD";
        a.chunkWireVersion = kChunkWireVersion;
        protocol::chunkwire::SourceInfo source;
        source.id = kChunkSources.front().id;
        source.latestGrid = protocol::chunkwire::GridInfo{};
        source.latestGrid->priceScale = 100;
        const int64_t start = recording::kHmc2MinMs + 50 * kDayMs;
        source.levels.push_back({kMinuteMs, kHourMs, start + 3 * kHourMs, start, start + 3 * kHourMs - kMinuteMs});
        a.sources.push_back(source);
        return a;
    }
};
TEST_F(HeatmapPlumbing, AttachBeforeConnectSeesConnectionAndAvailabilityWithNoCharts) {
    SentinelStreamClient client("127.0.0.1", "1", "");
    HeatmapDataService service([&](QObject *) { return new protocol::SentinelStreamClientTransport(client); });
    EXPECT_EQ(service.controllerCount(), 0);
    EXPECT_FALSE(service.connected());
    emit client.connected(); // exactly the signals delivered on a real connect/subscribe
    emit client.heatmapAvailabilityReceived(availability());
    drain(service);
    ASSERT_TRUE(service.connected());
    ASSERT_TRUE(service.availability("BTC-USD"));
    EXPECT_EQ(service.availability("BTC-USD")->sources.front().levels.front().levelMs, kMinuteMs);
    service.onData([&] { EXPECT_TRUE(service.fetcher()->availability("BTC-USD")); });
    emit client.disconnected();
    drain(service);
    EXPECT_FALSE(service.connected());
    EXPECT_FALSE(service.availability("BTC-USD"));
}
TEST_F(HeatmapPlumbing, LateAttachMissesConnectedSoOrderingIsNecessary) {
    SentinelStreamClient client("127.0.0.1", "1", "");
    emit client.connected();
    emit client.heatmapAvailabilityReceived(availability());
    HeatmapDataService service([&](QObject *) { return new protocol::SentinelStreamClientTransport(client); });
    drain(service);
    EXPECT_FALSE(service.connected());
    EXPECT_FALSE(service.availability("BTC-USD"));
}
TEST_F(HeatmapPlumbing, ViewBeforeAvailabilityReplansAndControllersShareBudgets) {
    FakeChunkTransport *transport = nullptr;
    HeatmapBudgets budgets{32ull << 20, 16ull << 20, 64ull << 20, 8ull << 20};
    HeatmapDataService service([&](QObject *) { return transport = new FakeChunkTransport; }, budgets);
    auto *first = service.createController(budgets.gpuPerChart);
    auto *second = service.createController(budgets.gpuPerChart);
    const auto a = availability();
    const auto start = a.sources.front().levels.front().oldestMs;
    service.onData([&] {
        // This models serverConfigUpdated/setTimeframe before availability.
        first->setView("BTC-USD", kMinuteMs, start, start + kHourMs);
        EXPECT_TRUE(transport->requests.empty());
        transport->goOnline();
        transport->push(ChunkAvailability{a});
    });
    drain(service);
    service.onData([&] {
        service.fetcher()->pump();
        EXPECT_FALSE(transport->requests.empty());
        EXPECT_EQ(service.cache()->maxBytes(), budgets.spanSources);
        EXPECT_EQ(service.cache()->cpuCeiling(), budgets.cpuCeiling);
    });
    service.refreshStats();
    EXPECT_EQ(service.stats().store.maxBytes, budgets.decodedChunks);
    EXPECT_GT(service.stats().fetcher.requests, 0);
    EXPECT_EQ(service.controllerCount(), 2);
    service.destroyController(first);
    service.destroyController(second);
    EXPECT_EQ(service.controllerCount(), 0);
}
TEST_F(HeatmapPlumbing, LabServerAndLocalConstructThroughTheSameService) {
    QTemporaryDir dir;
    lab::LabData::configure(dir.path().toStdString(), 0);
    lab::LabData::configureServer(lab::LabData::Server{"127.0.0.1", "1", ""});
    auto &server = lab::LabData::instance();
    EXPECT_NE(server.connection(), lab::LabData::Connection::Local);
    auto *controller = server.createController(8ull << 20);
    EXPECT_EQ(server.service().controllerCount(), 1);
    EXPECT_EQ(controller->thread(), server.service().thread());
    EXPECT_EQ(server.fetcher(), server.service().fetcher());
    server.destroyController(controller);
    const auto before = lab::LabData::lateConnectionCallbacksForTest();
    lab::LabData::queueConnectedOnShutdownForTest(true);
    lab::LabData::configureServer(std::nullopt);
    lab::LabData::queueConnectedOnShutdownForTest(false);
    EXPECT_EQ(lab::LabData::lateConnectionCallbacksForTest(), before);
    auto &local = lab::LabData::instance();
    EXPECT_EQ(local.connection(), lab::LabData::Connection::Local);
    EXPECT_EQ(&local.store(), &local.service().store());
    lab::LabData::configure({}, 0);
}
TEST_F(HeatmapPlumbing, SettingsRoundTripWorkspaceAndSharedTickMemory) {
    QTemporaryDir dir;
    QSettings ini(dir.filePath("test.ini"), QSettings::IniFormat);
    HeatmapSettingsStore store(ini);
    auto s = chartDefaults({});
    ASSERT_TRUE(applySettingsPatch(s, {{"renderer", "gpu"}, {"tickMode", "manual"}, {"manualTick", 250},
        {"minRowPx", 3.5}, {"hysteresis", 0.4}, {"crossfadeMs", 0}, {"showBandEdges", true},
        {"palettePreset", "Custom"}, {"bidGradient", QJsonArray{QJsonObject{{"position", 0}, {"color", "#112233"}}, QJsonObject{{"position", 1}, {"color", "#aabbccdd"}}}},
        {"sensitivityMin", 100}, {"sensitivityMax", 1000}, {"opacity", 0.6},
        {"gpuCapBytes", 64 * 1048576}, {"uploadBudgetBytes", 2 * 1048576}, {"prefetchTiles", 3},
        {"liveMinIntervalMs", 750}, {"showTelemetry", true}}).isEmpty());
    store.save("main", s);
    store.saveLayout("work", "main", {});
    store.save("main", {});
    EXPECT_EQ(store.restoreLayout("work", "main", {}), s);
    EXPECT_TRUE(store.saveManualTick("BTC-USD", kMinuteMs, 100));
    EXPECT_TRUE(store.saveManualTick("BTC-USD", kHourMs, 2500));
    EXPECT_FALSE(store.saveManualTick("BTC-USD", kMinuteMs, 37));
    ini.sync();
    QSettings reread(dir.filePath("test.ini"), QSettings::IniFormat);
    HeatmapSettingsStore again(reread);
    EXPECT_EQ(again.load("main", {}), s);
    EXPECT_EQ(again.loadManualTicks().get("BTC-USD", kMinuteMs), 100);
    EXPECT_EQ(again.loadManualTicks().get("BTC-USD", kHourMs), 2500);
    EXPECT_TRUE(ini.contains("layouts/work/heatmap/main/renderer"));
    EXPECT_TRUE(ini.contains("heatmap/manualTick/BTC-USD/60000"));
}
TEST_F(HeatmapPlumbing, DefaultsComeFromYamlAndEveryNumericFieldClamps) {
    QTemporaryDir dir;
    QFile yaml(dir.filePath("client.yaml"));
    ASSERT_TRUE(yaml.open(QIODevice::WriteOnly));
    yaml.write("heatmap:\n  renderer: gpu\n  tick_mode: manual\n  manual_tick: 250\n  min_row_px: 4\n  hysteresis: 0.6\n  crossfade_ms: 350\n  show_band_edges: true\n  palette_preset: Fire\n  bid_gradient: [{position: 0, color: \"#123456\"}, {position: 1, color: \"#abcdef\"}]\n  opacity: 0.7\n  sensitivity_min: 1\n  sensitivity_max: 100\n  gpu_cap_bytes: 67108864\n  upload_budget_bytes: 2097152\n  prefetch_tiles: 3\n  live_min_interval_ms: 800\n  show_telemetry: true\n  decoded_chunk_bytes: 33554432\n  span_source_bytes: 16777216\n  cpu_ceiling_bytes: 67108864\n");
    yaml.close();
    ClientConfig config;
    ASSERT_TRUE(ConfigLoader::loadClientConfig(yaml.fileName().toStdString(), &config));
    QSettings ini(dir.filePath("test.ini"), QSettings::IniFormat);
    HeatmapSettingsStore store(ini);
    auto s = store.load("main", config.heatmap);
    EXPECT_EQ(s.renderer, "gpu"); EXPECT_EQ(s.tickMode, TickMode::Manual); EXPECT_EQ(s.manualTick, 250);
    EXPECT_EQ(s.minRowPx, 4); EXPECT_EQ(s.hysteresis, 0.6); EXPECT_EQ(s.crossfadeMs, 350);
    EXPECT_TRUE(s.showBandEdges); EXPECT_EQ(s.palettePreset, "Fire"); EXPECT_EQ(s.opacity, 0.7);
    EXPECT_EQ(s.sensitivityMin, 1); EXPECT_EQ(s.sensitivityMax, 100); EXPECT_EQ(s.gpuCapBytes, 64ull << 20);
    EXPECT_EQ(s.uploadBudgetBytes, 2ull << 20); EXPECT_EQ(s.prefetchTiles, 3); EXPECT_EQ(s.liveMinIntervalMs, 800);
    EXPECT_TRUE(s.showTelemetry); EXPECT_EQ(store.loadBudgets(config.heatmap).cpuCeiling, 64ull << 20);
    EXPECT_EQ(s.bidGradient.front().color, "#123456"); EXPECT_EQ(s.bidGradient.back().color, "#abcdef");
    EXPECT_EQ(chartDefaults(ClientHeatmapConfig{}).renderer, "legacy");
    ASSERT_TRUE(applySettingsPatch(s, {{"manualTick", 37}, {"minRowPx", -1}, {"hysteresis", 7},
        {"crossfadeMs", -10}, {"sensitivityMin", -5}, {"sensitivityMax", -6}, {"opacity", 2},
        {"gpuCapBytes", -1}, {"uploadBudgetBytes", 999999999}, {"prefetchTiles", 99}, {"liveMinIntervalMs", 0}}).isEmpty());
    EXPECT_EQ(s.manualTick, 50); EXPECT_EQ(s.minRowPx, 0.5); EXPECT_EQ(s.hysteresis, 0.9);
    EXPECT_EQ(s.crossfadeMs, 0); EXPECT_EQ(s.sensitivityMin, 1e-9); EXPECT_GT(s.sensitivityMax, s.sensitivityMin);
    EXPECT_EQ(s.opacity, 1); EXPECT_EQ(s.gpuCapBytes, 1ull << 20); EXPECT_EQ(s.uploadBudgetBytes, s.gpuCapBytes);
    EXPECT_EQ(s.prefetchTiles, 16); EXPECT_EQ(s.liveMinIntervalMs, 100);
    const auto before = s;
    EXPECT_FALSE(applySettingsPatch(s, {{"opacity", "wrong"}, {"renderer", "gpu"}}).isEmpty());
    EXPECT_EQ(s, before);
    s.opacity = std::numeric_limits<double>::quiet_NaN();
    s.minRowPx = std::numeric_limits<double>::infinity();
    clampSettings(s);
    EXPECT_EQ(s.opacity, 1); EXPECT_EQ(s.minRowPx, 2);
    ini.setValue("heatmap/main/opacity", "bad");
    ini.setValue("heatmap/main/renderer", "unknown");
    EXPECT_EQ(store.load("main", config.heatmap).opacity, 0.7);
    EXPECT_EQ(store.load("main", config.heatmap).renderer, "gpu");
    ini.setValue("heatmap/budgets/cpuCeiling", 1);
    EXPECT_TRUE(store.loadBudgets(config.heatmap).valid());
}
TEST_F(HeatmapPlumbing, LastSessionNeverOverwritesLiveSettingsOrSnapshotsOverrides) {
    QTemporaryDir dir;
    QSettings ini(dir.filePath("test.ini"), QSettings::IniFormat);
    HeatmapSettingsStore store(ini);
    auto current = chartDefaults({});
    store.save("main", current);
    store.saveLayout("work", "main", {});
    // A snapshot left by an older binary, before a live POST and a crash.
    ini.setValue("layouts/_last_session/heatmap/main/opacity", 0.1);
    ASSERT_TRUE(store.applyChartPatch("main", current, {{"opacity", 0.7}}, true, {}, "BTC-USD", kMinuteMs).isEmpty());
    current = store.load("main", {}); // fresh process after an unclean shutdown
    store.restoreLayoutInto("_last_session", "main", current, {});
    EXPECT_EQ(current.opacity, 0.7);
    EXPECT_EQ(store.load("main", {}).opacity, 0.7);
    EXPECT_EQ(store.restoreLayout("_last_session", "main", {}).opacity, 0.7);
    const auto keys = ini.allKeys();
    store.saveLayout("_last_session", "main", {});
    EXPECT_EQ(ini.allKeys(), keys);
    EXPECT_EQ(ini.value("layouts/_last_session/heatmap/main/opacity").toDouble(), 0.1);
    store.restoreLayoutInto("work", "main", current, {});
    EXPECT_EQ(current.opacity, 1); // explicit named workspace still restores
    EXPECT_EQ(store.load("main", {}).opacity, 1);
}
TEST_F(HeatmapPlumbing, ProcessOnlyRendererNeverLeaksIntoOtherPatchesOrWorkspaces) {
    QTemporaryDir dir;
    QSettings ini(dir.filePath("test.ini"), QSettings::IniFormat);
    HeatmapSettingsStore store(ini);
    auto current = store.load("main", {});
    ASSERT_TRUE(store.applyChartPatch("main", current, {{"renderer", "gpu"}}, false, {}, "BTC-USD", kMinuteMs).isEmpty());
    EXPECT_EQ(current.renderer, "gpu");
    EXPECT_TRUE(ini.allKeys().isEmpty());
    // Automatic session restore cannot erase a process override, either.
    store.restoreLayoutInto("_last_session", "main", current, {});
    EXPECT_EQ(current.renderer, "gpu");
    ASSERT_TRUE(store.applyChartPatch("main", current, {{"opacity", 0.4}}, true, {}, "BTC-USD", kMinuteMs).isEmpty());
    EXPECT_EQ(current.renderer, "gpu");
    EXPECT_EQ(store.load("main", {}).renderer, "legacy");
    EXPECT_EQ(store.load("main", {}).opacity, 0.4);
    store.saveLayout("ab", "main", {});
    EXPECT_EQ(ini.value("layouts/ab/heatmap/main/renderer").toString(), "legacy");
    ini.sync();
    QSettings peerIni(dir.filePath("test.ini"), QSettings::IniFormat);
    HeatmapSettingsStore peer(peerIni);
    EXPECT_EQ(peer.load("main", {}).renderer, "legacy");
    // Bad patches are still atomic even when no persistence was requested.
    const auto before = current;
    EXPECT_FALSE(store.applyChartPatch("main", current, {{"renderer", "bad"}}, false, {}, "BTC-USD", kMinuteMs).isEmpty());
    EXPECT_EQ(current, before);
}
TEST_F(HeatmapPlumbing, EnteringManualAlonePreservesTheRememberedSymbolTick) {
    QTemporaryDir dir;
    QSettings ini(dir.filePath("test.ini"), QSettings::IniFormat);
    HeatmapSettingsStore store(ini);
    ASSERT_TRUE(store.saveManualTick("BTC-USD", kMinuteMs, 2500));
    auto current = store.load("main", {});
    ASSERT_TRUE(store.applyChartPatch("main", current, {{"tickMode", "manual"}}, true, {}, "BTC-USD", kMinuteMs).isEmpty());
    EXPECT_EQ(store.loadManualTicks().get("BTC-USD", kMinuteMs), 2500);
    ASSERT_TRUE(store.applyChartPatch("main", current, {{"manualTick", 5000}}, true, {}, "BTC-USD", kMinuteMs).isEmpty());
    EXPECT_EQ(store.loadManualTicks().get("BTC-USD", kMinuteMs), 5000);
    ASSERT_TRUE(store.applyChartPatch("main", current, {{"manualTick", 250}}, false, {}, "BTC-USD", kMinuteMs).isEmpty());
    EXPECT_EQ(current.manualTick, 250);
    EXPECT_EQ(store.loadManualTicks().get("BTC-USD", kMinuteMs), 5000);
}
TEST_F(HeatmapPlumbing, LabBudgetsAreInjectedAndCacheClearsDoNotReadConfiguration) {
    QTemporaryDir dir;
    ASSERT_TRUE(QDir(dir.path()).mkpath("config"));
    QFile yaml(dir.filePath("config/client_config.yaml"));
    ASSERT_TRUE(yaml.open(QIODevice::WriteOnly));
    yaml.write("heatmap:\n  decoded_chunk_bytes: 8388608\n  span_source_bytes: 8388608\n");
    yaml.close();
    struct Cleanup {
        QString cwd = QDir::currentPath();
        ~Cleanup() { lab::LabData::configure({}, 0); QDir::setCurrent(cwd); }
    } cleanup;
    ASSERT_TRUE(QDir::setCurrent(dir.path()));
    const auto loaded = ConfigLoader::getLoadedFiles();
    lab::LabData::configure(dir.path().toStdString(), 0);
    lab::LabData::instance().refreshStats();
    EXPECT_EQ(lab::LabData::instance().stats().store.maxBytes, HeatmapBudgets{}.decodedChunks);
    const HeatmapBudgets budgets{32ull << 20, 16ull << 20, 64ull << 20, 8ull << 20};
    lab::LabData::configure(dir.path().toStdString(), 0, budgets);
    for (int i = 0; i < 3; ++i) {
        auto &data = lab::LabData::instance();
        data.refreshStats();
        EXPECT_EQ(data.stats().store.maxBytes, budgets.decodedChunks);
        data.service().onData([&] { EXPECT_EQ(data.cache()->maxBytes(), budgets.spanSources); });
        data.clearCaches();
    }
    EXPECT_EQ(ConfigLoader::getLoadedFiles(), loaded);
}
TEST_F(HeatmapPlumbing, FactoryFailuresCleanUpOnTheWorkerAndJoinBeforeRethrowing) {
    for (const int failure : {0, 1, 2}) { // factory throw, null return, start throw
        QPointer<QObject> owned;
        QPointer<QThread> thread;
        bool deletedOnWorker = false;
        const auto construct = [&] {
            HeatmapDataService service([&](QObject *context) -> ChunkTransport * {
                thread = QThread::currentThread();
                owned = new QObject(context);
                QObject::connect(owned, &QObject::destroyed, context, [&] {
                    deletedOnWorker = QThread::currentThread() == thread.data();
                });
                if (failure == 0) throw std::runtime_error("factory failed");
                if (failure == 1) return nullptr;
                return new FakeChunkTransport;
            }, {}, [=](ChunkTransport &) { if (failure == 2) throw std::runtime_error("start failed"); });
        };
        EXPECT_THROW(construct(), std::runtime_error);
        EXPECT_TRUE(owned.isNull());
        EXPECT_TRUE(thread.isNull()); // QThread destruction would abort if still running
        EXPECT_TRUE(deletedOnWorker);
    }
}
} // namespace
