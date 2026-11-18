// The production tick chooser (HeatmapGpuLayer::chooseTickForView, with the real
// HeatmapSourceController behind it) over the synthetic HMC2 recording: its cost per
// changed view (the DISABLED_ benchmark, run alone through the build queue) and its
// allocation rule on the render path (CONTRIBUTING.md "Hot paths"), for the main
// chart's layer and for the lab's LabItem (its updatePaintNode, called directly: no
// window, so no scene graph renders the node).
//
// Benchmark: test_heatmap_tick_choice --gtest_also_run_disabled_tests
//            --gtest_filter='HeatmapTickChoice.DISABLED_*'
#include "SyntheticHmc2Fixture.hpp"
#include "lab/LabData.hpp"
#include "lab/LabItem.hpp"
#include "render/heatmap/HeatmapGpuLayer.hpp"
#include <QGuiApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <set>
#include <vector>

namespace {
thread_local bool gCount = false;
thread_local size_t gAllocations = 0, gBytes = 0;
} // namespace
void *operator new(std::size_t n) {
    if (gCount) ++gAllocations, gBytes += n;
    if (void *p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void *operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }

namespace {
using namespace synthetic_hmc2;
using heatmap::gpu::ViewWindow;
constexpr int64_t kDays = 3;

struct Scenario {
    const char *name;
    int64_t tfMs;
    ViewWindow start;
    double panPricePerStep; // the time step is tf / 7 (every view is a new one)
};
// 1m: a two-hour window over $600 (beyond the near band) and over $100 (near the
// mid, where more finer candidates are tried before one builds). 15m: 2.25 days.
const Scenario kScenarios[] = {
    {"1m 2h wide", minute, {double(epoch + 24 * kHourMs), double(epoch + 26 * kHourMs), 99'850, 100'450}, 0.37},
    {"1m 2h narrow", minute, {double(epoch + 24 * kHourMs), double(epoch + 26 * kHourMs), 100'050, 100'150}, 0.05},
    {"15m 2.25d", 15 * minute, {double(epoch + 4 * kHourMs), double(epoch + 58 * kHourMs), 99'500, 100'800}, 0.37},
};

std::vector<ViewWindow> panViews(const Scenario &s, int n) {
    std::vector<ViewWindow> out;
    for (int k = 0; k < n; ++k) {
        const double dt = double(s.tfMs) / 7 * k, dp = s.panPricePerStep * k;
        out.push_back({s.start.timeLoMs + dt, s.start.timeHiMs + dt, s.start.priceLo + dp, s.start.priceHi + dp});
    }
    return out;
}
// Price zoom about the centre: out by `factor` per step for n/2 steps, then back in.
std::vector<ViewWindow> zoomViews(const Scenario &s, int n, double factor) {
    std::vector<ViewWindow> out;
    const double centre = (s.start.priceLo + s.start.priceHi) / 2, half = (s.start.priceHi - s.start.priceLo) / 2;
    for (int k = 0; k < n; ++k) {
        const double h = half * std::pow(factor, k < n / 2 ? k : n - k);
        out.push_back({s.start.timeLoMs, s.start.timeHiMs, centre - h, centre + h});
    }
    return out;
}
ViewWindow extent(const std::vector<ViewWindow> &views) {
    ViewWindow e = views.front();
    for (const auto &v : views) {
        e.timeLoMs = std::min(e.timeLoMs, v.timeLoMs), e.timeHiMs = std::max(e.timeHiMs, v.timeHiMs);
        e.priceLo = std::min(e.priceLo, v.priceLo), e.priceHi = std::max(e.priceHi, v.priceHi);
    }
    return e;
}

// A layer on the lab service with every column of `views` loaded and the data path quiet.
class LoadedLayer {
public:
    heatmap::gpu::HeatmapGpuLayer layer;
    LoadedLayer(int64_t tfMs, const std::vector<ViewWindow> &views) : tfMs_(tfMs), extent_(extent(views)) {
        layer.setSymbol("BTC-USD");
        layer.setTimeframeMs(tfMs);
        layer.setSurface(1500, 800, 1);
        layer.setView(extent_, true);
        layer.setService(&lab::LabData::instance().service());
        layer.setActive(true);
    }
    ~LoadedLayer() { layer.setService(nullptr); }
    // Columns of the extent with a present source in the summary the chooser reads.
    size_t presentColumns() const {
        const auto snapshot = layer.snapshot();
        if (!snapshot || snapshot->tfMs != tfMs_) return 0;
        const auto latest = layer.controller() ? layer.controller()->latestResolution() : nullptr;
        const auto &summary = latest && latest->tfMs == tfMs_ ? *latest : snapshot->resolution;
        size_t n = 0;
        for (const auto &c : summary.columns) {
            if (double(c.startMs) < extent_.timeLoMs - tfMs_ || double(c.startMs) >= extent_.timeHiMs) continue;
            for (const auto &source : c.sources)
                if (source.state == heatmap::BucketState::Present) { ++n; break; }
        }
        return n;
    }
    size_t expectedColumns() const {
        const int64_t lo = int64_t(std::floor(extent_.timeLoMs / double(tfMs_))) * tfMs_;
        return size_t((int64_t(std::ceil(extent_.timeHiMs / double(tfMs_))) * tfMs_ - lo) / tfMs_);
    }
    // Loads, then waits until no new snapshot arrived for 500 ms (the tick commits too).
    bool settle() {
        QElapsedTimer total;
        total.start();
        const void *last = nullptr;
        QElapsedTimer quiet;
        quiet.start();
        while (total.elapsed() < 60'000) {
            layer.chooseTickForView(extent_);
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            const void *now = layer.controller() ? layer.controller()->latestSnapshot().get() : nullptr;
            if (now != last) last = now, quiet.restart();
            if (presentColumns() >= expectedColumns() && quiet.elapsed() > 500) return true;
        }
        return false;
    }

private:
    int64_t tfMs_;
    ViewWindow extent_;
};

struct Sample {
    double us = 0;
    size_t allocations = 0, bytes = 0;
};
Sample timedChoice(heatmap::gpu::HeatmapGpuLayer &layer, const ViewWindow &view) {
    gAllocations = gBytes = 0;
    gCount = true;
    const auto t0 = std::chrono::steady_clock::now();
    layer.chooseTickForView(view);
    const auto t1 = std::chrono::steady_clock::now();
    gCount = false;
    return {std::chrono::duration<double, std::micro>(t1 - t0).count(), gAllocations, gBytes};
}
double quantile(std::vector<double> v, double q) {
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, size_t(q * double(v.size() - 1) + 0.5))];
}
void report(const char *scenario, const char *motion, const char *mode, const char *cache,
            const std::vector<Sample> &samples) {
    std::vector<double> us;
    double allocations = 0, bytes = 0;
    size_t maxAllocations = 0;
    for (const auto &s : samples) {
        us.push_back(s.us);
        allocations += double(s.allocations), bytes += double(s.bytes);
        maxAllocations = std::max(maxAllocations, s.allocations);
    }
    const double n = double(samples.size());
    std::printf("TICK_BENCH | %-12s | %-4s | %-6s | %-4s | n=%3zu | allocs/call %8.1f (max %5zu) | bytes/call %9.0f | "
                "median %8.2f us | p99 %8.2f us | max %8.2f us\n",
                scenario, motion, mode, cache, samples.size(), allocations / n, maxAllocations, bytes / n,
                quantile(us, 0.5), quantile(us, 0.99), quantile(us, 1.0));
}
} // namespace

// The render path's rule with the real tick authority: once the scratch has grown,
// a changed view (pan, price zoom inside the hysteresis band) allocates nothing.
// The tick stays put, so no proposal is queued (a tick change posts one event).
TEST(HeatmapTickChoice, ChangedViewsAllocateNothingAfterWarmUp) {
    for (const auto &scenario : kScenarios) {
        auto views = panViews(scenario, 100);
        const auto zoom = zoomViews(scenario, 60, 1.001);
        views.insert(views.end(), zoom.begin(), zoom.end());
        LoadedLayer loaded(scenario.tfMs, views);
        ASSERT_TRUE(loaded.settle()) << scenario.name;
        for (const auto &v : views) loaded.layer.chooseTickForView(v); // warm-up: the scratch grows
        QCoreApplication::processEvents(QEventLoop::AllEvents, 200);   // the startup tick commits
        const int64_t tick = loaded.layer.tickUnits();
        ASSERT_GT(tick, 0) << scenario.name;
        size_t allocations = 0, bytes = 0;
        for (const auto &v : views) {
            const auto sample = timedChoice(loaded.layer, v);
            allocations += sample.allocations, bytes += sample.bytes;
            ASSERT_EQ(loaded.layer.predictTickUnits(v), tick) << scenario.name;
        }
        EXPECT_EQ(allocations, 0u) << scenario.name << ": " << bytes << " bytes over " << views.size() << " views";
        std::printf("%s: %zu allocations over %zu changed views, tick %lld\n", scenario.name, allocations,
                    views.size(), (long long)tick);
    }
}

// The lab chart chooses its tick in LabItem::updatePaintNode (GUI thread blocked):
// after warm-up a changed view allocates nothing there either. The first call
// creates the node; later calls only refill its frame.
class LabProbe : public lab::LabItem {
public:
    QSGNode *paint(QSGNode *old) { return updatePaintNode(old, nullptr); }
};
TEST(HeatmapTickChoice, LabItemChangedViewsAllocateNothingAfterWarmUp) {
    const auto &scenario = kScenarios[0];
    auto views = panViews(scenario, 100);
    const auto zoom = zoomViews(scenario, 60, 1.001);
    views.insert(views.end(), zoom.begin(), zoom.end());
    LabProbe item;
    item.setSize({1500, 800});
    item.setInitialView(extent(views));
    item.loadReal(int(kDays * 24));
    QElapsedTimer total;
    total.start();
    QSGNode *node = nullptr;
    auto loaded = [&] {
        const auto snapshot = item.snapshot();
        size_t n = 0;
        if (snapshot && snapshot->tfMs == scenario.tfMs)
            for (const auto &c : snapshot->resolution.columns)
                if (double(c.startMs) >= extent(views).timeLoMs - double(minute) &&
                    double(c.startMs) < extent(views).timeHiMs)
                    for (const auto &source : c.sources)
                        if (source.state == heatmap::BucketState::Present) { ++n; break; }
        const auto e = extent(views);
        return n >= size_t(std::ceil(e.timeHiMs / double(minute)) - std::floor(e.timeLoMs / double(minute)));
    };
    while (!(loaded() && item.tickUnits() > 0) && total.elapsed() < 60'000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        node = item.paint(node);
    }
    ASSERT_TRUE(loaded() && item.tickUnits() > 0);
    for (const auto &v : views) item.setView(v), node = item.paint(node); // warm-up: the scratch grows
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    const int64_t tick = item.tickUnits();
    size_t allocations = 0, bytes = 0;
    for (const auto &v : views) {
        item.setView(v); // posts the view to the controller (allocates; not the render path)
        gAllocations = gBytes = 0;
        gCount = true;
        node = item.paint(node);
        gCount = false;
        allocations += gAllocations, bytes += gBytes;
        ASSERT_EQ(item.tickUnits(), tick);
    }
    EXPECT_EQ(allocations, 0u) << bytes << " bytes over " << views.size() << " views";
    std::printf("lab item: %zu allocations over %zu changed views, tick %lld\n", allocations, views.size(),
                (long long)tick);
    delete node;
}

// Per changed view (miss: the view differs from the last frame's) and per repeated
// view (hit: the TickKey cache), Auto then Manual, for a pan and a price zoom.
TEST(HeatmapTickChoice, DISABLED_ChangedViewCost) {
    constexpr int kViews = 300;
    for (const auto &scenario : kScenarios) {
        const auto pan = panViews(scenario, kViews), zoom = zoomViews(scenario, kViews, 1.01);
        auto all = pan;
        all.insert(all.end(), zoom.begin(), zoom.end());
        LoadedLayer loaded(scenario.tfMs, all);
        ASSERT_TRUE(loaded.settle()) << scenario.name << " columns " << loaded.presentColumns() << "/"
                                     << loaded.expectedColumns();
        std::printf("TICK_BENCH %s: %zu columns loaded, %zu in the first view\n", scenario.name,
                    loaded.presentColumns(),
                    size_t((scenario.start.timeHiMs - scenario.start.timeLoMs) / double(scenario.tfMs)));
        for (const bool manual : {false, true}) {
            if (manual) {
                auto settings = loaded.layer.settings();
                settings.tickMode = heatmap::TickMode::Manual;
                settings.manualTick = 1000;
                loaded.layer.setSettings(settings, true);
                QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            }
            for (const auto *views : {&pan, &zoom}) {
                for (const auto &v : *views) loaded.layer.chooseTickForView(v); // warm pass
                std::vector<Sample> miss, hit;
                std::set<int64_t> ticks;
                for (const auto &v : *views) {
                    miss.push_back(timedChoice(loaded.layer, v));
                    hit.push_back(timedChoice(loaded.layer, v));
                    ticks.insert(loaded.layer.predictTickUnits(v));
                }
                const char *motion = views == &pan ? "pan" : "zoom";
                const char *mode = manual ? "Manual" : "Auto";
                report(scenario.name, motion, mode, "miss", miss);
                report(scenario.name, motion, mode, "hit", hit);
                std::printf("TICK_BENCH   distinct ticks over the %s: %zu\n", motion, ticks.size());
                QCoreApplication::processEvents(QEventLoop::AllEvents, 50); // queued proposals
            }
        }
    }
}

int main(int argc, char **argv) {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
    // The recording outlives the application: LabData shuts down (and its transport
    // stops polling availability) when the application is destroyed.
    QTemporaryDir fixture;
    QGuiApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    writeRecording(fixture, kDays * 24 * 60);
    lab::LabData::configure(fixture.path().toStdString(), epoch + kDays * 24 * kHourMs);
    return RUN_ALL_TESTS();
}
