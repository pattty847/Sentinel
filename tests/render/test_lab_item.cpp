// Slice S5c: the lab chart on the production path (LocalChunkTransport ->
// ChunkFetcher -> HeatmapSourceController on the heatmap-data thread ->
// HeatmapTileNode), over a synthetic HMC2 recording written to a temp dir (the
// real recording is never touched). GPU cases use the selected QRhi backend and
// skip with the reason when it cannot create one.
#include "lab/LabData.hpp"
#include "lab/LabItem.hpp"
#include "lab/OffscreenQuick.hpp"
#include "lab/RhiBackend.hpp"
#include "servermodel/Hmc2Store.hpp"
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <gtest/gtest.h>
#include <cmath>
#include <iostream>
#include <set>
#include <thread>

namespace {
using heatmap::kHourMs;
using heatmap::kMinuteMs;
constexpr int64_t minute = kMinuteMs;
constexpr int64_t epoch = recording::kHmc2MinMs + 50 * heatmap::kDayMs;

// Minute i of layer "deep" ($10 rows, a wide book) or "near" ($1 rows, a band).
recording::Hmc2Record syntheticMinute(const std::string &layer, int64_t i) {
    const bool deep = layer == "deep";
    recording::Hmc2Record r;
    r.header = {"BTC-USD", layer, minute, 100, deep ? 1000 : 100, {}, deep ? 77u : 78u};
    r.bucketStartMs = epoch + i * minute;
    r.observedMs = uint32_t(minute);
    const int64_t mid = (100'000 + (i % 30) * 10) * 100 / r.header.rowTickUnits;
    const int64_t half = deep ? 40 : 200;
    r.bidRowLo = r.askRowLo = mid - half;
    r.bidRowHi = r.askRowHi = mid + half - 1;
    r.midOpen = r.midClose = r.midMin = r.midMax = double(mid) * double(r.header.rowTickUnits) / 100;
    for (int64_t row = mid - half; row < mid + half; ++row) {
        const bool ask = row >= mid;
        const auto code = recording::encodeSize(0.01 * double(1 + (row * 7 + i) % 53), r.header.sizeScale);
        r.entries.push_back({row, ask, code, code, r.observedMs});
    }
    return r;
}
void writeRecording(const QTemporaryDir &dir, int64_t minutes) {
    recording::Hmc2Store writer(dir.path().toStdString());
    for (int64_t i = 0; i < minutes; ++i)
        for (const char *layer : {"deep", "near"}) writer.append(syntheticMinute(layer, i));
}

// Renders frames (asking every item to redraw: offscreen rendering emits no
// frameSwapped) until done() holds, or `ms` pass.
template <class Done>
bool pump(lab::OffscreenQuick &scene, const std::vector<lab::LabItem *> &items, int ms, Done done, QString *error) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        for (auto *item : items) item->update();
        if (scene.renderFrame(error).isNull()) return false;
        if (done()) return true;
    }
    return false;
}
bool settle(lab::OffscreenQuick &scene, const std::vector<lab::LabItem *> &items, QString *error) {
    int streak = 0;
    const bool ok = pump(scene, items, 30'000, [&] {
        streak = std::all_of(items.begin(), items.end(), [](lab::LabItem *i) { return i->settled(); }) ? streak + 1 : 0;
        return streak >= 3;
    }, error);
    if (!ok && error && error->isEmpty()) *error = QStringLiteral("did not settle: ") + items.front()->status();
    return ok;
}
double metric(const lab::LabItem *item, const char *name) { return item->metrics().value(name).toDouble(); }
std::set<std::string> visibleBuilds(const lab::LabItem *item) {
    std::set<std::string> out;
    if (const auto set = item->snapshot())
        for (const auto &span : set->spans)
            if (span.rank.tier == heatmap::SpanTier::Visible)
                for (const auto &source : span.sources)
                    if (source.build)
                        for (const auto &g : source.build->key.generations)
                            out.insert(source.source + "/" + std::to_string(g.startMs) + "/" + std::to_string(g.generation));
    return out;
}

class LabItemTest : public testing::Test {
protected:
    QTemporaryDir dir;
    void SetUp() override {
        if (const QString why = lab::gpuUnavailableReason(); !why.isEmpty()) GTEST_SKIP() << "GPU case skipped: " << why.toStdString();
        ASSERT_TRUE(dir.isValid());
        writeRecording(dir, 4 * 60);
        lab::LabData::configure(dir.path().toStdString(), 0);
    }
    void TearDown() override { lab::LabData::configure({}, 0); }
    lab::LabItem *chart(lab::OffscreenQuick &scene, int tfMinutes, QPointF at = {}) {
        auto *item = new lab::LabItem(scene.window()->contentItem());
        item->setPosition(at);
        item->setSize(QSizeF(640, 240));
        item->setTimeframeMinutes(tfMinutes);
        item->loadReal(24);
        return item;
    }
};

TEST_F(LabItemTest, SettlesOnTheProductionPathWithBothSources) {
    lab::OffscreenQuick scene;
    QString error;
    ASSERT_TRUE(scene.create(QSize(640, 240), &error)) << error.toStdString();
    auto *item = chart(scene, 1);
    ASSERT_TRUE(settle(scene, {item}, &error)) << error.toStdString();
    const auto m = item->metrics();
    EXPECT_GT(m.value("residentSources").toDouble(), 0);
    EXPECT_GT(m.value("sourcesUploaded").toDouble(), 0);
    EXPECT_EQ(m.value("errors").toDouble(), 0);
    EXPECT_EQ(m.value("missingDraws").toDouble(), 0);
    EXPECT_GT(m.value("tick").toDouble(), 0);
    // Both sources are planned for every span (the coarse one bins first).
    const auto set = item->snapshot();
    ASSERT_TRUE(set);
    std::set<std::string> sources;
    for (const auto &span : set->spans)
        for (const auto &source : span.sources) sources.insert(source.source);
    EXPECT_EQ(sources.size(), 2u);
    delete item;
    ASSERT_FALSE(scene.renderFrame(&error).isNull());
}

// Plan section 4: the previous timeframe's spans stay resident (recent-tf), so
// switching back needs no request and no build.
TEST_F(LabItemTest, ReturnsToARecentTimeframeWithoutRequestsOrBuilds) {
    lab::OffscreenQuick scene;
    QString error;
    ASSERT_TRUE(scene.create(QSize(640, 240), &error)) << error.toStdString();
    auto *item = chart(scene, 1);
    ASSERT_TRUE(settle(scene, {item}, &error)) << error.toStdString();
    item->setTimeframeMinutes(5);
    ASSERT_TRUE(settle(scene, {item}, &error)) << error.toStdString();
    lab::LabData::instance().refreshStats();
    const double fetched = metric(item, "fetchedChunks"), builds = metric(item, "spanBuilds");
    item->setTimeframeMinutes(1);
    int frames = 0;
    ASSERT_TRUE(pump(scene, {item}, 10'000, [&] { ++frames; return item->settled(); }, &error)) << error.toStdString();
    lab::LabData::instance().refreshStats();
    EXPECT_EQ(metric(item, "fetchedChunks"), fetched) << "no chunk requested";
    EXPECT_EQ(metric(item, "spanBuilds"), builds) << "no span rebuilt";
    EXPECT_LE(frames, 4) << "the switch back draws within a few frames";
    delete item;
    ASSERT_FALSE(scene.renderFrame(&error).isNull());
}

// FM-104 on the production path: the chart moves to a new scene (a new QRhi);
// the node reports the loss, the controller rebuilds the released images and
// the chart settles again.
TEST_F(LabItemTest, ComesBackAfterTheQRhiIsRecreated) {
    auto *item = new lab::LabItem;
    item->setSize(QSizeF(640, 240));
    item->setTimeframeMinutes(1);
    QString error;
    auto first = std::make_unique<lab::OffscreenQuick>();
    ASSERT_TRUE(first->create(QSize(640, 240), &error)) << error.toStdString();
    item->setParentItem(first->window()->contentItem());
    item->loadReal(24);
    ASSERT_TRUE(settle(*first, {item}, &error)) << error.toStdString();
    const double uploads = metric(item, "sourcesUploaded");
    item->setParentItem(nullptr);
    ASSERT_FALSE(first->renderFrame(&error).isNull());
    first.reset();
    lab::OffscreenQuick second;
    ASSERT_TRUE(second.create(QSize(640, 240), &error)) << error.toStdString();
    item->setParentItem(second.window()->contentItem());
    ASSERT_TRUE(settle(second, {item}, &error)) << error.toStdString();
    EXPECT_GT(metric(item, "sourcesUploaded"), uploads) << "the released images came back and were uploaded again";
    EXPECT_GT(metric(item, "residentSources"), 0);
    EXPECT_EQ(metric(item, "errors"), 0);
    item->setParentItem(nullptr);
    ASSERT_FALSE(second.renderFrame(&error).isNull());
    delete item;
}

// A revision stored once reaches every chart that uses the chunk.
TEST_F(LabItemTest, ARevisionRebuildsEveryChart) {
    lab::OffscreenQuick scene;
    QString error;
    ASSERT_TRUE(scene.create(QSize(640, 480), &error)) << error.toStdString();
    std::vector<lab::LabItem *> items{chart(scene, 1), chart(scene, 5, {0, 240})};
    ASSERT_TRUE(settle(scene, items, &error)) << error.toStdString();
    const auto a = visibleBuilds(items[0]), b = visibleBuilds(items[1]);
    ASSERT_FALSE(lab::LabData::instance().reviseNewestChunks().empty());
    EXPECT_TRUE(pump(scene, items, 10'000, [&] {
        return visibleBuilds(items[0]) != a && visibleBuilds(items[1]) != b && items[0]->settled() && items[1]->settled();
    }, &error)) << "both charts rebuilt the spans of the revised chunk " << error.toStdString();
    for (auto *item : items) delete item;
    ASSERT_FALSE(scene.renderFrame(&error).isNull());
}

// Spec rules 1, 2 and 9: Shift+wheel scales price only (macOS delivers it as a
// horizontal delta); wheel zoom obeys the clamps.
TEST_F(LabItemTest, ShiftWheelScalesPriceOnlyWithinTheClamps) {
    lab::OffscreenQuick scene;
    QString error;
    ASSERT_TRUE(scene.create(QSize(1000, 500), &error)) << error.toStdString();
    auto *item = new lab::LabItem(scene.window()->contentItem());
    item->setSize(QSizeF(1000, 500));
    item->loadReal(24);
    ASSERT_TRUE(settle(scene, {item}, &error)) << error.toStdString();
    auto span = [&](const char *key) { return item->metrics().value(key).toDouble(); };
    const double time0 = span("timeSpanMin"), price0 = span("priceSpan");
    item->wheelZoom(120, 0, true, 0.5, 0.5); // Shift+wheel as macOS sends it
    EXPECT_DOUBLE_EQ(span("timeSpanMin"), time0) << "price only";
    EXPECT_NEAR(span("priceSpan"), price0 * std::exp(-0.12), 1e-6 * price0);
    const double price1 = span("priceSpan");
    item->wheelZoom(0, 0, true, 0.5, 0.5);
    item->wheelZoom(120, 0, false, 0.5, 0.5); // unshifted horizontal scroll is not a zoom
    EXPECT_DOUBLE_EQ(span("priceSpan"), price1);
    item->wheelZoom(0, 120, true, 0.5, 0.5); // a mouse that keeps the vertical delta
    EXPECT_LT(span("priceSpan"), price1);
    EXPECT_DOUBLE_EQ(span("timeSpanMin"), time0);
    // Manual $10: zoom-out stops at one row per pixel (500 px -> $5000).
    item->setManualTick(10);
    for (int i = 0; i < 80; ++i) item->wheelZoom(-120, 0, true, 0.5, 0.5);
    EXPECT_NEAR(span("priceSpan"), 5000, 1e-6);
    // Plain wheel zoom-out stops at one column per pixel (1000 px at 1 m).
    for (int i = 0; i < 80; ++i) item->wheelZoom(0, -120, false, 0.5, 0.5);
    EXPECT_LE(span("timeSpanMin"), 1000 + 1e-9);
    EXPECT_NEAR(span("priceSpan"), 5000, 1e-6) << "Manual price clamp holds for plain wheel too";
    item->setManualMode(false); // Auto re-ticks instead of clamping
    for (int i = 0; i < 10; ++i) item->wheelZoom(-120, 0, true, 0.5, 0.5);
    EXPECT_GT(span("priceSpan"), 5000);
    delete item;
    ASSERT_FALSE(scene.renderFrame(&error).isNull());
}
// S6a CPU parity: synthetic HMC2 -> local transport -> shared service -> two
// controllers. Runs even when the sandbox has no Metal device/window server.
TEST(LabDataService, LocalControllersShareRecordedBuildsWithoutAGpu) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    writeRecording(dir, 4 * 60);
    lab::LabData::configure(dir.path().toStdString(), epoch + 4 * kHourMs);
    struct Cleanup { ~Cleanup() { lab::LabData::configure({}, 0); } } cleanup;
    auto &data = lab::LabData::instance();
    auto &service = data.service();
    auto *a = data.createController(320ull << 20);
    auto *b = data.createController(320ull << 20);
    service.onData([&] {
        a->setView(lab::kSymbol, minute, epoch + kHourMs, epoch + 2 * kHourMs);
        b->setView(lab::kSymbol, minute, epoch + kHourMs, epoch + 2 * kHourMs);
    });
    auto builds = [](const auto *controller) {
        std::set<const heatmap::SpanSourceBuild *> out;
        if (const auto set = controller->latestSnapshot()) {
            for (const auto &span : set->spans) {
                if (span.rank.tier != heatmap::SpanTier::Visible) continue;
                for (const auto &source : span.sources) {
                    if (!source.build) return std::set<const heatmap::SpanSourceBuild *>{};
                    out.insert(source.build.get());
                }
            }
        }
        return out;
    };
    QElapsedTimer deadline;
    deadline.start();
    while ((builds(a).empty() || builds(b).empty()) && deadline.elapsed() < 5000) {
        QCoreApplication::processEvents();
        service.onData([] { QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall); });
        std::this_thread::yield();
    }
    ASSERT_FALSE(builds(a).empty());
    EXPECT_EQ(builds(a), builds(b));
    ASSERT_TRUE(data.availability());
    EXPECT_EQ(data.availability()->sources.size(), 2);
    data.refreshStats();
    EXPECT_GT(data.stats().fetcher.bodies, 0);
    EXPECT_GT(data.stats().store.bytes, 0);
    data.destroyController(a);
    data.destroyController(b);
    EXPECT_EQ(service.controllerCount(), 0);
}

// S5L-c review fix 2: the server-mode data path shuts down with a connection
// completion queued behind it (the stream client's connected() emitted inside
// the teardown turn, before the client is deleted). Its delivery is dropped with
// the client: it never runs against a deleted client (no GPU needed; the
// server address is unreachable, so nothing else connects).
TEST(LabDataServer, AConnectionCompletionQueuedBehindShutdownNeverRuns) {
    lab::LabData::configureServer(lab::LabData::Server{"127.0.0.1", "1", ""});
    auto &data = lab::LabData::instance();
    ASSERT_NE(data.connection(), lab::LabData::Connection::Local);
    const int before = lab::LabData::lateConnectionCallbacksForTest();
    lab::LabData::queueConnectedOnShutdownForTest(true);
    lab::LabData::configureServer(std::nullopt); // destroys the instance: shutdown on the data thread
    lab::LabData::queueConnectedOnShutdownForTest(false);
    EXPECT_EQ(lab::LabData::lateConnectionCallbacksForTest(), before)
        << "a connection callback ran after the teardown had started";
    EXPECT_EQ(lab::LabData::instance().connection(), lab::LabData::Connection::Local); // local again
    lab::LabData::configure({}, 0);
}
} // namespace

int main(int argc, char **argv) {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    lab::selectQuickSceneGraph(); // before any QQuickWindow: Qt fixes the backend at the first one
    ::testing::InitGoogleTest(&argc, argv);
    std::cout << "[sentinel] " << lab::describeRhi().toStdString() << std::endl;
    return RUN_ALL_TESTS();
}
