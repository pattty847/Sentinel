// Footprint numbers on the render thread: time FootprintCellsLayer::submitLabels with its
// labels dirty (every frame formats, measures and places every number) on a dense flow
// snapshot (20 bars x 50 bands, both sides: 2,000 numbers from 0.01 to the thousands).
// Prints microseconds per changed frame and heap allocations per frame. No GPU, no GUI.
// With an argument "heatmap [maxGlyphs]": time HeatmapLabelLayout::layout plus the shared
// ChartTextRenderer submission for a 3840x2160 view of 96x108 cells (four-glyph labels).
#include "chart/core/ChartFrame.hpp"
#include "chart/layers/FootprintCellsLayer.hpp"
#include "config/AgentHostMode.hpp"
#include "render/ChartTextAtlas.hpp"
#include "render/ChartTextRenderer.hpp"
#include "render/heatmap/HeatmapLabelLayout.hpp"
#include <QSGNode>
#include <QTemporaryDir>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <new>
#include <random>
#include <string>
#include <vector>

namespace {
std::atomic<uint64_t> allocations{0};
}
void* operator new(std::size_t size) {
    allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

namespace {
int heatmapBench(const ChartTextAtlas& atlas, size_t maxGlyphs) {
    using namespace heatmap;
    using namespace heatmap::gpu;
    constexpr int64_t minute = 60'000;
    constexpr uint32_t columns = 96, rows = 108;
    auto cells = std::make_shared<LabelCells>();
    cells->key.serial = 1; cells->key.tfMs = minute; cells->key.tickUnits = 100;
    cells->key.firstBucket = 1000; cells->key.firstBin = 100'000; cells->key.columns = columns; cells->key.rows = rows;
    cells->grid = {minute, 0, 1000, columns, 100, 1.0, 100, 100'000, rows};
    cells->cells.resize(size_t(columns) * rows);
    for (auto& cell : cells->cells) {
        cell.word = tiles::cellWord(tiles::kCellValid, 20000);
        cell.value = 1.234; // x $100k: "123k" in both forms, four glyphs
        formatLabelCell(cell, 100'000);
    }
    TimeAxisMapping m;
    m.viewStartMs = 1000.0 * minute; m.viewEndMs = (1000.0 + columns) * minute;
    m.viewMinPrice = 100'000; m.viewMaxPrice = 100'000.0 + rows;
    m.dataStartMs = m.viewStartMs; m.dataEndMs = m.viewEndMs; m.dataMinPrice = m.viewMinPrice; m.dataMaxPrice = m.viewMaxPrice;
    m.appendMs = minute; m.tickSize = 1; m.drawRect = QRectF(0, 0, 3840, 2160);
    m.srcRect = QRectF(0, 0, columns, rows); m.valid = true;
    LabelStyle style;
    style.window = {6000, 24000};
    style.maxGlyphs = maxGlyphs;
    HeatmapLabelLayout layout;
    std::vector<ChartGlyphInstance> glyphs;
    glyphs.reserve(maxGlyphs);
    ChartTextRenderer renderer;
    QSGNode root;
    constexpr int kFrames = 200;
    std::vector<double> micros;
    micros.reserve(kFrames);
    uint64_t allocated = 0;
    for (int i = 0; i < kFrames + 20; ++i) {
        const uint64_t before = allocations.load();
        const auto t0 = std::chrono::steady_clock::now();
        layout.layout(cells, atlas, m, 1.0, style, glyphs);
        renderer.beginFrame(&root, nullptr, atlas);
        renderer.submitGlyphs(glyphs, ChartTextRenderer::Priority::Low);
        renderer.endFrame();
        if (i >= 20) {
            micros.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count());
            allocated += allocations.load() - before;
        }
    }
    std::sort(micros.begin(), micros.end());
    std::cout << "heatmap_labels_bench maxGlyphs=" << maxGlyphs << " glyphs=" << glyphs.size()
              << " labels=" << layout.stats().labels << " rendererAccepted=" << renderer.submittedGlyphs()
              << " dropped=" << renderer.droppedLowGlyphs() << " median_us=" << micros[kFrames / 2]
              << " p90_us=" << micros[kFrames * 9 / 10] << " allocations_per_frame=" << double(allocated) / kFrames << "\n";
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    QTemporaryDir cache;
    QString error;
    if (!AgentHostMode::activate(cache.path(), {}, &error)) { std::cerr << error.toStdString() << "\n"; return 2; }
    ChartTextAtlas atlas;
    ChartTextAtlas::BuildParams params;
    params.fontPath = QStringLiteral(SENTINEL_SOURCE_DIR "/resources/fonts/RobotoMono/RobotoMono-Regular.ttf");
    params.charset = "0123456789.<>kMBT";
    params.fontPx = 64;
    if (!atlas.build(params)) { std::cerr << "atlas build failed\n"; return 2; }
    if (argc > 1 && std::string(argv[1]) == "heatmap") return heatmapBench(atlas, argc > 2 ? std::stoul(argv[2]) : 40000);

    constexpr int kBars = 20, kRows = 50, kLow = 100;
    chart::ChartFrame frame;
    auto& f = frame.context;
    f.surfaceBounds = {0, 0, kBars * 160, kRows * 24 + 10};
    f.itemBounds = f.surfaceBounds;
    f.surfaceDpr = 1;
    f.time.activeTimeframeMs = 1000;
    f.overlays.footprint = true;
    auto& r = f.raster;
    r.valid = true; r.dpr = 1; r.tick = 1; r.tfMs = 1000;
    r.rowPx = 24; r.rowPxF = 24; r.colPx = 160; r.colPxF = 160;
    r.drawnStartMs = 0; r.drawnEndMs = kBars * 1000; r.drawnMinPrice = kLow; r.drawnMaxPrice = kLow + kRows;
    r.topF = (kLow + kRows) * 24.0; r.topRowIndex = int64_t(r.topF); r.heightDev = kRows * 24 + 10; r.widthDev = kBars * 160;

    chart::FootprintCellsLayer layer;
    layer.presentation().enabled = true;
    layer.presentation().dustNotional = 0;
    auto& data = layer.mailbox().back();
    data.valid = true; data.windowState = chart::FlowSnapshot::WindowState::Ready;
    data.timeframeMs = 1000; data.tick = 1;
    data.fromMs = 0; data.toMs = kBars * 1000; data.low = kLow; data.high = kLow + kRows;
    data.windowFromMs = r.drawnStartMs; data.windowToMs = r.drawnEndMs;
    data.windowLow = r.drawnMinPrice; data.windowHigh = r.drawnMaxPrice;
    std::mt19937 rng(11);
    std::uniform_real_distribution<double> exponent(0, 5.5); // 0.01 .. ~3,000 (base scale 100)
    for (int b = 0; b < kBars; ++b) {
        auto& bar = data.bars[size_t(b)];
        bar.startMs = b * 1000; bar.baseScale = 100;
        bar.firstCell = data.cellCount; bar.cellCount = kRows;
        bar.lowRow = kLow; bar.highRow = kLow + kRows;
        int64_t maximum = 0;
        for (int row = 0; row < kRows; ++row) {
            auto& cell = data.cells[size_t(data.cellCount++)];
            cell.row = kLow + row; cell.span = 1;
            cell.bidVol = int64_t(std::pow(10.0, exponent(rng))) + 1;
            cell.askVol = int64_t(std::pow(10.0, exponent(rng))) + 1;
            maximum = std::max({maximum, cell.bidVol, cell.askVol});
        }
        bar.maxSideVol = maximum; bar.maxRowVol = 2 * maximum;
    }
    data.barCount = kBars; data.visibleBarCount = kBars;
    layer.mailbox().post();

    std::unique_ptr<QSGNode> root(layer.createContainer());
    ChartTextRenderer text;
    chart::LayerNodeContext context{nullptr, root.get(), text, atlas, true};
    layer.sync(frame);
    layer.updateNodes(frame, context);
    layer.submitLabels(frame, context, 0);
    if (layer.glyphCount() == 0) { std::cerr << "no numbers drawn\n"; return 1; }
    // Alternate the bottom inset (no number is near the bottom) so every frame re-lays the labels.
    constexpr int kWarm = 50, kFrames = 400;
    for (int i = 0; i < kWarm; ++i) layer.submitLabels(frame, context, (i & 1) * 0.5);
    std::vector<double> micros;
    micros.reserve(kFrames);
    const uint64_t before = allocations.load();
    for (int i = 0; i < kFrames; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        layer.submitLabels(frame, context, (i & 1) * 0.5);
        micros.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count());
    }
    const uint64_t allocated = allocations.load() - before;
    std::sort(micros.begin(), micros.end());
    std::cout << "footprint_labels_bench numbers=" << 2 * kBars * kRows << " glyphs=" << layer.glyphCount()
              << " frames=" << kFrames << " median_us=" << micros[kFrames / 2] << " p90_us=" << micros[kFrames * 9 / 10]
              << " min_us=" << micros.front() << " allocations_per_frame=" << double(allocated) / kFrames << "\n";
    return 0;
}
