// Slice S5c: the production HeatmapTileNode on the controller's SpanSet, and the
// bin kernel's fill pass. GPU cases run on the selected QRhi backend
// (SENTINEL_RHI_BACKEND; lab::HeadlessRhi / OffscreenQuick) and skip cleanly
// when it cannot create a QRhi with compute: a skipped case is no result.
// Deterministic: fake SpanSets (the "fake controller") or the real controller on
// FakeChunkTransport with a manual build executor; no sleeps except where a
// fade must end.
#include "HeatmapNodeFixtures.hpp"
#include "HeatmapNodeScene.hpp"
#include "protocol/SentinelStreamClient.hpp" // shared-frame metatype
#include "../servermodel/FakeChunkTransport.hpp"
#include "heatmap/TimeComposer.hpp"
#include "lab/OffscreenQuick.hpp"
#include "lab/RhiBackend.hpp"
#include "render/heatmap/HeatmapGpuBinner.hpp"
#include "render/heatmap/HeatmapTileNode.hpp"
#include "render/heatmap/HeatmapPalette.hpp"
#include "render/ChartRaster.hpp"
#include <QCoreApplication>
#include <QFile>
#include <QMatrix4x4>
#include <QEvent>
#include <QGuiApplication>
#include <QQuickItem>
#include <QQuickWindow>
#include <gtest/gtest.h>
#include <rhi/qrhi.h>
#include <rhi/qshader.h>
#include <cstring>
#include <deque>
#include <iostream>
#include <set>
#include <thread>

namespace {
using namespace heatmap;
using namespace heatmap::gpu;
using namespace nodefx;
constexpr int64_t minute = kMinuteMs;

struct Headless : lab::HeadlessRhi {
    Headless() { create(); }
};
tiles::TileGrid tileGridFor(int64_t tile, int64_t tfMs, int64_t tickUnits, int64_t firstBin, uint32_t rows) {
    tiles::TileGrid g;
    g.tfMs = tfMs;
    g.tile = tile;
    g.firstBucket = tiles::tileFirstBucket(tile);
    g.tickUnits = tickUnits;
    g.tick = fromUnits(tickUnits, 100);
    g.firstBin = firstBin;
    g.rows = rows;
    return g;
}
BinGrid toBinGrid(const tiles::TileGrid &g) { return {g.tfMs, g.firstBucket, g.columns, g.tick, g.firstBin, g.rows}; }

// Bins `passes` (first, then fills) resident sources into one buffer per row
// block (as the node does) and reads the cells back, top row first.
std::vector<uint32_t> binPasses(QRhi *rhi, HeatmapGpuBinner &binner, const std::vector<uint64_t> &passes,
                                const BinGrid &grid, bool fill = true) {
    std::vector<uint32_t> out(size_t(grid.columns) * grid.rows);
    for (uint32_t top = 0; top < grid.rows; top += tiles::kTileBlockRows) {
        BinGrid block = grid;
        block.rows = std::min(tiles::kTileBlockRows, grid.rows - top);
        block.firstBin = grid.firstBin + int64_t(grid.rows - top - block.rows);
        const uint64_t bytes = uint64_t(block.columns) * block.rows * 4;
        std::unique_ptr<QRhiBuffer> target(rhi->newBuffer(QRhiBuffer::Static, QRhiBuffer::StorageBuffer,
                                                          quint32((bytes + 15) / 16 * 16)));
        EXPECT_TRUE(target->create());
        QRhiReadbackResult result;
        QRhiCommandBuffer *cb = nullptr;
        QString error;
        EXPECT_EQ(rhi->beginOffscreenFrame(&cb), QRhi::FrameOpSuccess);
        for (size_t p = 0; p < passes.size(); ++p)
            EXPECT_TRUE(binner.binResidentInto(passes[p], cb, block, {}, target.get(), &error, fill && p > 0))
                << error.toStdString();
        auto *updates = rhi->nextResourceUpdateBatch();
        updates->readBackBuffer(target.get(), 0, quint32(bytes), &result);
        cb->resourceUpdate(updates);
        EXPECT_EQ(rhi->endOffscreenFrame(), QRhi::FrameOpSuccess);
        if (result.data.size() != qsizetype(bytes)) { ADD_FAILURE() << "readback size"; return out; }
        std::memcpy(out.data() + size_t(top) * grid.columns, result.data.constData(), bytes);
    }
    return out;
}
uint64_t uploadResident(QRhi *rhi, HeatmapGpuBinner &binner, const std::shared_ptr<const GpuSource> &source) {
    QRhiCommandBuffer *cb = nullptr;
    QString error;
    bool complete = false;
    uint64_t budget = 1ull << 40;
    EXPECT_EQ(rhi->beginOffscreenFrame(&cb), QRhi::FrameOpSuccess);
    EXPECT_TRUE(binner.uploadResident(source, cb, budget, &complete, &error)) << error.toStdString();
    EXPECT_EQ(rhi->endOffscreenFrame(), QRhi::FrameOpSuccess);
    EXPECT_TRUE(complete);
    return source->id;
}

// Direct display-pass readback: upload known packed cells so the floor-path
// checks exercise the production shaders independently of price binning. Cells are
// row-major, top row first (`columns` per row). The fragment shader defaults to the
// production one; the slice A reference (tests/render/shaders) checks the rest path.
class DisplayReadback {
public:
    explicit DisplayReadback(QRhi *rhi) : rhi_(rhi) {}
    bool create(QSize size, uint32_t rows, const HeatmapPalette &palette, uint32_t columns = 1,
                const char *fragment = ":/heatmapgpu/heatmap_display.frag.qsb") {
        size_ = size;
        rows_ = rows;
        columns_ = columns;
        cells_.reset(rhi_->newBuffer(QRhiBuffer::Static, QRhiBuffer::StorageBuffer, rows * columns * 4));
        params_.reset(rhi_->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(Draw)));
        palette_.reset(rhi_->newTexture(QRhiTexture::RGBA8, QSize(kPaletteWidth, 1), 1));
        color_.reset(rhi_->newTexture(QRhiTexture::RGBA8, size, 1,
                                      QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
        sampler_.reset(rhi_->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
                                        QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
        if (!cells_->create() || !params_->create() || !palette_->create() || !color_->create() ||
            !sampler_->create())
            return false;
        QRhiTextureRenderTargetDescription description{QRhiColorAttachment(color_.get())};
        target_.reset(rhi_->newTextureRenderTarget(description));
        pass_.reset(target_->newCompatibleRenderPassDescriptor());
        target_->setRenderPassDescriptor(pass_.get());
        if (!target_->create()) return false;
        const auto fs = QRhiShaderResourceBinding::FragmentStage;
        const auto vs = QRhiShaderResourceBinding::VertexStage;
        bindings_.reset(rhi_->newShaderResourceBindings());
        bindings_->setBindings(
            {QRhiShaderResourceBinding::bufferLoad(0, fs, cells_.get()),
             QRhiShaderResourceBinding::uniformBuffer(1, vs | fs, params_.get()),
             QRhiShaderResourceBinding::sampledTexture(2, fs, palette_.get(), sampler_.get())});
        if (!bindings_->create()) return false;
        auto shader = [](const char *path) {
            QFile file(QString::fromLatin1(path));
            return file.open(QIODevice::ReadOnly) ? QShader::fromSerialized(file.readAll()) : QShader{};
        };
        const QShader frag = shader(fragment);
        if (!frag.isValid()) return false;
        pipeline_.reset(rhi_->newGraphicsPipeline());
        pipeline_->setShaderStages(
            {{QRhiShaderStage::Vertex, shader(":/heatmapgpu/heatmap_display.vert.qsb")},
             {QRhiShaderStage::Fragment, frag}});
        pipeline_->setTopology(QRhiGraphicsPipeline::TriangleStrip);
        pipeline_->setShaderResourceBindings(bindings_.get());
        pipeline_->setRenderPassDescriptor(pass_.get());
        if (!pipeline_->create()) return false;
        texels_ = palette.texels;
        tone_ = palette.tone;
        return true;
    }
    // top/span: the row offset and rows down the image; left/across: the column offset
    // and columns across it. coverageGamma 0: rest (single cell), 2.2: a transition.
    QImage render(const std::vector<uint32_t> &cells, float top, float span, bool clampRows = false,
                  float coverageGamma = 0.0f, float left = 0.0f, float across = 1.0f) {
        if (cells.size() != size_t(rows_) * columns_) return {};
        Draw draw{};
        QMatrix4x4 projection;
        projection.ortho(0.0f, float(size_.width()), float(size_.height()), 0.0f, -1.0f, 1.0f);
        const QMatrix4x4 mvp = rhi_->clipSpaceCorrMatrix() * projection;
        std::memcpy(draw.mvp, mvp.constData(), sizeof(draw.mvp));
        draw.rect[2] = float(size_.width());
        draw.rect[3] = float(size_.height());
        draw.mapping[0] = left;
        draw.mapping[1] = across;
        draw.mapping[2] = top;
        draw.mapping[3] = span;
        draw.dims[0] = columns_;
        draw.dims[1] = rows_;
        draw.dims[2] = clampRows ? 1u : 0u;
        draw.style[1] = 32767.0f;
        draw.style[2] = 1.0f;
        draw.tone[0] = tone_.gamma;
        draw.tone[1] = tone_.contrast;
        draw.tone[2] = tone_.floor;
        draw.tone[3] = coverageGamma;
        QRhiCommandBuffer *cb = nullptr;
        if (rhi_->beginOffscreenFrame(&cb) != QRhi::FrameOpSuccess) return {};
        auto *updates = rhi_->nextResourceUpdateBatch();
        updates->uploadStaticBuffer(cells_.get(), cells.data());
        updates->updateDynamicBuffer(params_.get(), 0, sizeof(draw), &draw);
        const QByteArray bytes(reinterpret_cast<const char *>(texels_.data()), int(texels_.size()));
        updates->uploadTexture(palette_.get(), QRhiTextureUploadDescription({QRhiTextureUploadEntry(
                                                   0, 0, QRhiTextureSubresourceUploadDescription(bytes))}));
        cb->beginPass(target_.get(), QColor(0, 0, 0, 0), {1.0f, 0}, updates);
        cb->setGraphicsPipeline(pipeline_.get());
        cb->setViewport(QRhiViewport(0, 0, float(size_.width()), float(size_.height())));
        cb->setShaderResources(bindings_.get());
        cb->draw(4);
        cb->endPass();
        QRhiReadbackResult result;
        auto *read = rhi_->nextResourceUpdateBatch();
        read->readBackTexture(QRhiReadbackDescription(color_.get()), &result);
        cb->resourceUpdate(read);
        if (rhi_->endOffscreenFrame() != QRhi::FrameOpSuccess ||
            result.data.size() != qsizetype(size_.width()) * size_.height() * 4)
            return {};
        // Keep raw premultiplied bytes for exact RGBA comparisons and light sums.
        const QImage image(reinterpret_cast<const uchar *>(result.data.constData()), size_.width(),
                           size_.height(), QImage::Format_RGBA8888);
        return rhi_->isYUpInFramebuffer() ? image.flipped(Qt::Vertical) : image.copy();
    }

private:
    struct Draw {
        float mvp[16], rect[4], mapping[4];
        uint32_t dims[4];
        float style[4], tone[4];
    };
    static_assert(sizeof(Draw) == 144);
    QRhi *rhi_;
    QSize size_;
    uint32_t rows_ = 0, columns_ = 1;
    PaletteTexels texels_{};
    PaletteTone tone_;
    std::unique_ptr<QRhiBuffer> cells_, params_;
    std::unique_ptr<QRhiTexture> palette_, color_;
    std::unique_ptr<QRhiSampler> sampler_;
    std::unique_ptr<QRhiRenderPassDescriptor> pass_;
    std::unique_ptr<QRhiTextureRenderTarget> target_;
    std::unique_ptr<QRhiShaderResourceBindings> bindings_;
    std::unique_ptr<QRhiGraphicsPipeline> pipeline_;
};
HeatmapPalette solidSidePalette() {
    HeatmapPalette palette;
    palette.tone = {1.0f, 1.0f, 0.0f};
    for (int i = 0; i < kPaletteWidth; ++i) {
        const std::array<uint8_t, 4> rgba =
            i < 256 ? std::array<uint8_t, 4>{32, 128, 240, 255} : std::array<uint8_t, 4>{224, 64, 16, 255};
        std::copy(rgba.begin(), rgba.end(), palette.texels.begin() + i * 4);
    }
    return palette;
}
constexpr uint32_t validCell = 3u << 16;
constexpr uint32_t hotBid = validCell | 32767u;
constexpr uint32_t hotAsk = hotBid | 0x8000u;

// Interior floor-selected data pixels match the CPU palette reference exactly.
TEST(HeatmapDisplayFloor, InteriorDataPixelsMatchTheCpuReference) {
    Headless gpu;
    if (!gpu.rhi) GTEST_SKIP() << gpu.skipReason();
    const auto palette = solidSidePalette();
    DisplayReadback display(gpu.rhi.get());
    ASSERT_TRUE(display.create(QSize(8, 24), 8, palette));
    const std::vector<uint32_t> cells{hotAsk, hotBid, hotAsk, hotBid, hotAsk, hotBid, hotAsk, hotBid};
    int compared = 0;
    for (int offset = 0; offset < 16; ++offset) {
        const float top = -1.0f - float(offset) / 16.0f / 2.4f;
        const auto image = display.render(cells, top, 10.0f);
        ASSERT_FALSE(image.isNull());
        for (int y = 0; y < image.height(); ++y) {
            const double lo = top + double(y) / 2.4, hi = top + double(y + 1) / 2.4;
            const int row = int(std::floor(lo));
            if (row < 0 || row >= 8 || hi >= row + 1.0 - 1e-5 || lo <= row + 1e-5) continue;
            const auto rgba = legacyRecordingColor(uint16_t(cells[row]), {0, 32767}, palette);
            const QColor original(int(std::lround(rgba[0] * 255)), int(std::lround(rgba[1] * 255)),
                                  int(std::lround(rgba[2] * 255)), int(std::lround(rgba[3] * 255)));
            EXPECT_EQ(image.pixelColor(4, y), original) << "offset=" << offset << " y=" << y;
            ++compared;
        }
    }
    EXPECT_GE(compared, 100);
}

// Loading and veil bytes match a full-screen hatch at the same screen pixels,
// including both tones, when the floor-selected row is next to data.
TEST(HeatmapDisplayFloor, LoadingAndVeilHatchesStayByteIdenticalNextToData) {
    Headless gpu;
    if (!gpu.rhi) GTEST_SKIP() << gpu.skipReason();
    DisplayReadback display(gpu.rhi.get());
    ASSERT_TRUE(display.create(QSize(16, 24), 8, solidSidePalette()));
    for (const uint32_t state : {1u, 2u}) {
        for (int offset = 0; offset < 8; ++offset) {
            const float top = -1.0f - float(offset) / 8.0f / 2.4f;
            std::vector<uint32_t> cells(8, state << 16);
            const auto original = display.render(cells, 0.0f, 8.0f);
            ASSERT_FALSE(original.isNull());
            cells[3] = hotBid;
            cells[5] = hotAsk;
            const auto adjacent = display.render(cells, top, 10.0f);
            ASSERT_FALSE(adjacent.isNull());
            int compared = 0;
            std::set<QRgb> tones;
            for (int y = 0; y < adjacent.height(); ++y) {
                const double centre = top + (double(y) + 0.5) / 2.4;
                if (std::abs(centre - std::round(centre)) < 1e-5) continue;
                const int row = int(std::floor(centre));
                if (row < 0 || row >= 8 || row == 3 || row == 5) continue;
                for (int x = 0; x < adjacent.width(); ++x) {
                    EXPECT_EQ(adjacent.pixel(x, y), original.pixel(x, y)) << "state=" << state << " y=" << y;
                    tones.insert(adjacent.pixel(x, y));
                    ++compared;
                }
            }
            EXPECT_GE(compared, 64);
            EXPECT_EQ(tones.size(), 2u);
            const std::set<QRgb> legacyTones = state == 1u
                                                   ? std::set<QRgb>{qRgb(27, 41, 61), qRgb(14, 20, 31)}
                                                   : std::set<QRgb>{qRgb(83, 83, 87), qRgb(62, 62, 67)};
            std::set<QRgb> rgbTones;
            for (const QRgb pixel : tones) {
                rgbTones.insert(pixel | 0xff000000u);
                EXPECT_NEAR(qAlpha(pixel), state == 1u ? 204.0 : 229.5, state == 1u ? 0.0 : 0.5);
            }
            EXPECT_EQ(rgbTones, legacyTones) << "original premultiplied hatch RGB bytes";
        }
    }
}

// ---------------------------------------------------------------- whole-pixel rows
// Plan 2026-10-08 B1/B2: the raster camera (chart_raster::computeRaster) -> its drawn
// window -> the node's draw parameters (mappingFor over one block covering the view,
// as HeatmapTileNode::addBinDraw) -> the production display shader. Rows are drawn
// whole device pixels tall, the same height everywhere and at every drag offset.
struct RasterRows {
    static constexpr double tick = 1.0;
    static constexpr int64_t firstBin = 99'800; // the grid's bottom bin
    static constexpr uint32_t rows = 400;      // bins 99,800 .. 100,199
    QSize size{8, 240};                        // device pixels (dpr 2: 4 x 120 logical)
    chart_raster::RasterInputs view(double rowsPx, double dragDevicePx) const {
        chart_raster::RasterInputs in;
        in.tfMs = double(minute);
        in.tick = tick;
        in.dpr = 2.0;
        in.itemWidthLogical = size.width() / in.dpr;
        in.itemHeightLogical = size.height() / in.dpr;
        in.timeStart = 0;
        in.timeEnd = minute;
        in.maxPrice = 100'050.3; // not on a row edge
        in.minPrice = in.maxPrice - size.height() * tick / rowsPx;
        in.anchorFracY = 0.3;
        in.dragLogicalPx = QPointF(0, dragDevicePx / in.dpr);
        return in;
    }
    // The cells top row first (fy = 0 is bin firstBin + rows - 1).
    static std::vector<uint32_t> cells(const std::function<uint32_t(int64_t bin)> &cellOf) {
        std::vector<uint32_t> out(rows);
        for (uint32_t i = 0; i < rows; ++i) out[i] = cellOf(firstBin + int64_t(rows) - 1 - int64_t(i));
        return out;
    }
    static QImage draw(DisplayReadback &display, const std::vector<uint32_t> &cells, const chart_raster::RasterCamera &cam) {
        const BinGrid grid{minute, 0, 1, tick, firstBin, rows};
        const auto mapping = mappingFor(grid, {cam.drawnStartMs, cam.drawnEndMs, cam.drawnMinPrice, cam.drawnMaxPrice});
        return display.render(cells, mapping.priceOffset, mapping.priceSpan);
    }
    // Lengths of the colour runs in pixel column x that touch neither image edge.
    static std::vector<std::pair<QRgb, int>> interiorRuns(const QImage &image, int x) {
        std::vector<std::pair<QRgb, int>> runs;
        int start = 0;
        for (int y = 1; y <= image.height(); ++y) {
            if (y < image.height() && image.pixel(x, y) == image.pixel(x, start)) continue;
            if (start > 0 && y < image.height()) runs.push_back({image.pixel(x, start), y - start});
            start = y;
        }
        return runs;
    }
};

// B1: one hot row in an otherwise empty grid at today's Auto regime (2.4 px per row),
// 300 sub-pixel drag offsets: the hot row is always P = 2 device pixels tall, never 3.
TEST(HeatmapDisplayRaster, AnIsolatedRowKeepsItsHeightAtEveryDragOffset) {
    Headless gpu;
    if (!gpu.rhi) GTEST_SKIP() << "GPU case skipped: " << gpu.skipReason();
    RasterRows fx;
    DisplayReadback display(gpu.rhi.get());
    ASSERT_TRUE(display.create(fx.size, RasterRows::rows, solidSidePalette()));
    const int64_t hotBin = 100'000;
    const auto cells = RasterRows::cells([&](int64_t bin) { return bin == hotBin ? hotBid : 0u; });
    chart_raster::RasterStep prev{};
    int drawn = 0;
    std::set<int> heights;
    for (int i = 0; i < 300; ++i) {
        const auto cam = chart_raster::computeRaster(fx.view(2.4, -0.3 * i), prev);
        ASSERT_TRUE(cam.valid);
        ASSERT_EQ(cam.rowPx, 2) << "offset " << i;
        prev = cam.step();
        const QImage image = RasterRows::draw(display, cells, cam);
        ASSERT_FALSE(image.isNull());
        int hot = 0, first = -1;
        for (int y = 0; y < image.height(); ++y)
            if (qAlpha(image.pixel(3, y)) > 0) {
                if (first < 0) first = y;
                ++hot;
            }
        if (first <= 0 || first + hot >= image.height()) continue; // the row is cut by an edge
        heights.insert(hot);
        EXPECT_EQ(hot, cam.rowPx) << "drag offset " << i << " (" << -0.3 * i << " device px): the hot row is " << hot
                                  << " px tall at y=" << first;
        EXPECT_NEAR(first, cam.yDev(double(hotBin + 1) * RasterRows::tick), 1e-6) << "its top edge is the camera's";
        ++drawn;
    }
    EXPECT_GE(drawn, 250);
    EXPECT_EQ(heights, std::set<int>{2});
}

// B2: ten rows of distinct colours over the view: every band is exactly P tall, for
// P = 2, 3 and 5, at several sub-pixel drag offsets.
TEST(HeatmapDisplayRaster, EveryDrawnRowHasTheSameHeight) {
    Headless gpu;
    if (!gpu.rhi) GTEST_SKIP() << "GPU case skipped: " << gpu.skipReason();
    RasterRows fx;
    DisplayReadback display(gpu.rhi.get());
    ASSERT_TRUE(display.create(fx.size, RasterRows::rows, *makePalette(legacyDefaultGradients(), {1.0f, 1.0f, 0.0f})));
    // Bins 100,020 .. 100,029 alternate sides with rising codes: neighbours always differ.
    const auto cells = RasterRows::cells([](int64_t bin) {
        if (bin < 100'020 || bin > 100'029) return 0u;
        const uint32_t k = uint32_t(bin - 100'020);
        return validCell | (3000u + k * 2900u) | ((k % 2) ? 0x8000u : 0u);
    });
    for (const auto [r, expected] : {std::pair{2.4, 2}, {3.3, 3}, {4.7, 5}}) {
        for (int i = 0; i < 12; ++i) {
            const auto cam = chart_raster::computeRaster(fx.view(r, 0.37 * i), {});
            ASSERT_TRUE(cam.valid);
            ASSERT_EQ(cam.rowPx, expected);
            const QImage image = RasterRows::draw(display, cells, cam);
            ASSERT_FALSE(image.isNull());
            int bands = 0;
            for (const auto &[colour, length] : RasterRows::interiorRuns(image, 3)) {
                if (qAlpha(colour) == 0) continue; // the empty rows around the ten
                EXPECT_EQ(length, cam.rowPx) << "r=" << r << " offset " << i << ": a band of " << length << " px";
                ++bands;
            }
            EXPECT_EQ(bands, 10) << "r=" << r << " offset " << i;
        }
    }
}

// ---------------------------------------------------------------- smooth zoom (A2)
// Whole-pixel smooth zoom (slice A2): at rest the display shader draws the single cell
// under each pixel centre exactly as slice A did; during a zoom transition it covers
// the pixel footprint in rows AND columns with coverage in linear light (gamma 2.2).
double decodeSrgb(int byte) {
    const double c = byte / 255.0;
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}
double decodedLight(const QImage &image, int x0, int y0, int x1, int y1) {
    double light = 0;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            const QRgb p = image.pixel(x, y);
            light += decodeSrgb(qRed(p)) + decodeSrgb(qGreen(p)) + decodeSrgb(qBlue(p));
        }
    return light;
}

// Rest bytes: the production shader with coverage gamma 0 renders exactly what the
// slice A shader (tests/render/shaders/heatmap_display_slice_a.frag) renders, for every
// cell state, codes on both sides, whole and fractional mappings, clamped and not.
TEST(HeatmapDisplayZoom, RestPathRendersTheSliceAShaderBytes) {
    Headless gpu;
    if (!gpu.rhi) GTEST_SKIP() << "GPU case skipped: " << gpu.skipReason();
    const auto palette = *makePalette(legacyDefaultGradients(), {1.05f, 1.15f, 0.01f});
    const uint32_t rows = 8, columns = 6;
    DisplayReadback production(gpu.rhi.get()), sliceA(gpu.rhi.get());
    ASSERT_TRUE(production.create(QSize(40, 36), rows, palette, columns));
    ASSERT_TRUE(sliceA.create(QSize(40, 36), rows, palette, columns, ":/slicea/heatmap_display_slice_a.frag.qsb"));
    std::vector<uint32_t> cells(rows * columns);
    for (uint32_t i = 0; i < cells.size(); ++i) {
        const uint32_t kind = (i * 7u) % 6u;
        cells[i] = kind == 0 ? 0u : kind == 1 ? 1u << 16 : kind == 2 ? 2u << 16 : kind == 3 ? validCell
                 : validCell | ((i * 2654435761u) % 32767u) | ((i % 2) ? 0x8000u : 0u);
    }
    int compared = 0;
    for (const bool clampRows : {false, true})
        for (const auto [left, across, top, span] : {std::array<float, 4>{0, 6, 0, 8}, {-0.5f, 7.3f, -1.25f, 9.7f},
                                                      {0.37f, 2.5f, 0.6f, 3.1f}, {1, 4, 2, 4}, {-2, 10, -3, 14}}) {
            const auto a = production.render(cells, top, span, clampRows, 0.0f, left, across);
            const auto b = sliceA.render(cells, top, span, clampRows, 0.0f, left, across);
            ASSERT_FALSE(a.isNull());
            ASSERT_FALSE(b.isNull());
            EXPECT_EQ(a, b) << "clamp=" << clampRows << " mapping=" << left << "," << across << "," << top << "," << span;
            ++compared;
        }
    EXPECT_EQ(compared, 10);
}

// Linear-light conservation during a glide: one hot row in an otherwise empty grid,
// its height gliding 2 -> 3 device px with sub-pixel offsets: the decoded light (and
// the alpha area) equal the row's height in pixels within 3% on every frame.
TEST(HeatmapDisplayZoom, ATransitionConservesTheLightOfAnIsolatedRow) {
    Headless gpu;
    if (!gpu.rhi) GTEST_SKIP() << "GPU case skipped: " << gpu.skipReason();
    DisplayReadback display(gpu.rhi.get());
    ASSERT_TRUE(display.create(QSize(8, 48), 16, solidSidePalette()));
    const double fullPixelLight = decodeSrgb(32) + decodeSrgb(128) + decodeSrgb(240);
    for (const uint32_t empty : {validCell, 0u}) {
        std::vector<uint32_t> cells(16, empty);
        cells[7] = hotBid;
        for (int frame = 0; frame <= 12; ++frame) {
            const double rowPx = 2.0 * std::pow(1.5, frame / 12.0); // a 2 -> 3 px glide
            const double top = 7.0 - (17.3 + 0.37 * frame) / rowPx;    // the hot row near y = 17..20
            const auto image = display.render(cells, float(top), float(48.0 / rowPx), false, 2.2f);
            ASSERT_FALSE(image.isNull());
            const double light = decodedLight(image, 4, 0, 5, 48) / fullPixelLight;
            int alpha = 0;
            for (int y = 0; y < 48; ++y) alpha += qAlpha(image.pixel(4, y));
            EXPECT_NEAR(light, rowPx, rowPx * 0.03) << "frame " << frame << " row " << rowPx << " px";
            EXPECT_NEAR(alpha, rowPx * 255, 3.0) << "frame " << frame;
        }
    }
}

// The same in columns: a hot column gliding 2 -> 3 px wide.
TEST(HeatmapDisplayZoom, ATransitionConservesTheLightOfAnIsolatedColumn) {
    Headless gpu;
    if (!gpu.rhi) GTEST_SKIP() << "GPU case skipped: " << gpu.skipReason();
    DisplayReadback display(gpu.rhi.get());
    ASSERT_TRUE(display.create(QSize(48, 6), 1, solidSidePalette(), 16));
    const double fullPixelLight = decodeSrgb(224) + decodeSrgb(64) + decodeSrgb(16);
    std::vector<uint32_t> cells(16, validCell);
    cells[7] = hotAsk;
    for (int frame = 0; frame <= 12; ++frame) {
        const double colPx = 2.0 * std::pow(1.5, frame / 12.0);
        const double left = 7.0 - (17.3 + 0.37 * frame) / colPx;
        const auto image = display.render(cells, 0.0f, 1.0f, false, 2.2f, float(left), float(48.0 / colPx));
        ASSERT_FALSE(image.isNull());
        const double light = decodedLight(image, 0, 3, 48, 4) / fullPixelLight;
        EXPECT_NEAR(light, colPx, colPx * 0.03) << "frame " << frame << " column " << colPx << " px";
    }
}

// Inside one cell the transition draws that cell's exact rest bytes; a half-covered
// hatch edge carries half its light (linear) and half its alpha.
TEST(HeatmapDisplayZoom, InteriorCellsKeepTheirBytesAndHatchEdgesAreWeighted) {
    Headless gpu;
    if (!gpu.rhi) GTEST_SKIP() << "GPU case skipped: " << gpu.skipReason();
    const auto palette = solidSidePalette();
    DisplayReadback display(gpu.rhi.get());
    ASSERT_TRUE(display.create(QSize(8, 24), 8, palette));
    const std::vector<uint32_t> cells{hotAsk, hotBid, hotAsk, hotBid, hotAsk, hotBid, hotAsk, hotBid};
    int compared = 0;
    for (int offset = 0; offset < 16; ++offset) {
        const float top = -1.0f - float(offset) / 16.0f / 2.4f;
        const auto rest = display.render(cells, top, 10.0f);
        const auto blend = display.render(cells, top, 10.0f, false, 2.2f);
        ASSERT_FALSE(rest.isNull());
        ASSERT_FALSE(blend.isNull());
        for (int y = 0; y < 24; ++y) {
            const double lo = top + double(y) / 2.4, hi = top + double(y + 1) / 2.4;
            const int row = int(std::floor(lo));
            if (row < 0 || row >= 8 || hi >= row + 1.0 - 1e-5 || lo <= row + 1e-5) continue;
            EXPECT_EQ(blend.pixel(4, y), rest.pixel(4, y)) << "offset=" << offset << " y=" << y;
            ++compared;
        }
    }
    EXPECT_GE(compared, 100);
    DisplayReadback hatch(gpu.rhi.get());
    ASSERT_TRUE(hatch.create(QSize(16, 24), 8, palette));
    for (const uint32_t state : {1u, 2u}) {
        const auto full = hatch.render(std::vector<uint32_t>(8, state << 16), 0.0f, 8.0f);
        std::vector<uint32_t> one(8, validCell);
        one[3] = state << 16;
        const auto partial = hatch.render(one, 0.25f, 12.0f, false, 2.2f); // y=5 covers [2.75, 3.25]
        ASSERT_FALSE(full.isNull());
        ASSERT_FALSE(partial.isNull());
        const double halfRgb = std::pow(0.5, 1.0 / 2.2);
        for (int x = 0; x < 16; ++x) {
            const QRgb under = full.pixel(x, 5), got = partial.pixel(x, 5);
            EXPECT_NEAR(qRed(got), qRed(under) * halfRgb, 1.0) << "state " << state << " x " << x;
            EXPECT_NEAR(qAlpha(got), qAlpha(under) * 0.5, 1.0) << "state " << state << " x " << x;
        }
    }
}

// ---------------------------------------------------------------- fill pass
// Owner decision 1: the coarsest source bins first, a finer source then writes
// only the cells left veiled, and only valid ones. The combined GPU cells equal
// the CPU oracle (buildCells per source, then fillVeiled) exactly, for ticks
// only the fine source builds ($1, $2), ticks both build ($5, $10, $20: the fine
// source fills the coarse source's gaps), an incompatible tick ($7.50) and a
// grid tall enough for two row blocks.
TEST(HeatmapFillPass, CombinedCellsMatchTheCpuOracle) {
    Headless gpu;
    if (!gpu.rhi) GTEST_SKIP() << gpu.skipReason();
    Shape shape;
    shape.coarseLo = 60'000; // a tall coarse book: two row blocks at $1
    shape.coarseHi = 140'000;
    const int64_t tf = minute, tile = firstTile(tf) + 1;
    const int64_t start = tiles::tileStartMs(tile, tf), end = tiles::tileEndMs(tile, tf);
    const int64_t availableStart = start + 3 * tf, availableEnd = end - 2 * tf; // no data at both edges
    auto composedOf = [&](const std::string &source) {
        const auto data = minuteColumns(source, recording::floorDiv(start, kHourMs) * kHourMs,
                                        recording::floorDiv(end + kHourMs - 1, kHourMs) * kHourMs, 1, shape);
        ComposeOptions clip;
        clip.startMs = start;
        clip.endMs = end;
        const SparseColumns *level = &data;
        return compose(std::span<const SparseColumns *const>(&level, 1), tf, clip);
    };
    const auto coarse = composedOf(kCoarse), fine = composedOf(kFine);
    GpuSourceOptions options;
    options.availableStartMs = availableStart;
    options.availableEndMs = availableEnd;
    auto coarseSource = std::make_shared<const GpuSource>(buildGpuSource(coarse, options));
    auto fineSource = std::make_shared<const GpuSource>(buildGpuSource(fine, options));
    HeatmapGpuBinner binner(gpu.rhi.get());
    binner.forceKernel(KernelVariant::Fast);
    const uint64_t c = uploadResident(gpu.rhi.get(), binner, coarseSource);
    const uint64_t f = uploadResident(gpu.rhi.get(), binner, fineSource);
    tiles::CellOptions cells;
    cells.availableStartMs = availableStart;
    cells.availableEndMs = availableEnd;
    for (const int64_t tick : {100, 200, 500, 1000, 2000, 750}) {
        // Rows from below the coarse book to above it, through the fine band.
        const int64_t firstBin = (shape.coarseLo - 400) * 100 / tick, endBin = (shape.coarseHi + 400) * 100 / tick;
        const auto grid = tileGridFor(tile, tf, tick, firstBin, uint32_t(endBin - firstBin));
        if (tick == 100) EXPECT_GT(grid.rows, tiles::kTileBlockRows) << "two row blocks";
        auto oracle = tiles::buildCells(coarse, grid, cells);
        const auto firstOnly = oracle;
        tiles::fillVeiled(oracle, tiles::buildCells(fine, grid, cells));
        const auto gpuCells = binPasses(gpu.rhi.get(), binner, {c, f}, toBinGrid(grid));
        std::array<uint64_t, 4> states{};
        uint64_t mismatches = 0, filled = 0;
        for (size_t i = 0; i < oracle.size(); ++i) {
            ++states[tiles::cellState(oracle[i])];
            filled += oracle[i] != firstOnly[i];
            if (tiles::cellState(oracle[i]) != tiles::cellState(gpuCells[i])) { ++mismatches; continue; }
            if (tiles::cellState(oracle[i]) == tiles::kCellValid) mismatches += (oracle[i] & 0xffffu) != (gpuCells[i] & 0xffffu);
        }
        EXPECT_EQ(mismatches, 0u) << "tick=" << tick;
        EXPECT_GT(states[tiles::kCellNoData], 0u) << tick;
        EXPECT_GT(states[tiles::kCellVeil], 0u) << tick << ": beyond the fine band";
        if (tick == 750) {
            EXPECT_EQ(states[tiles::kCellValid], 0u);
        } else {
            EXPECT_GT(states[tiles::kCellValid], 0u) << tick;
            EXPECT_GT(filled, 0u) << tick << ": the fine source filled veiled cells";
        }
        // Without the fill flag the second pass overwrites everything: the rule matters.
        if (tick == 500 || tick == 1000) {
            const auto overwritten = binPasses(gpu.rhi.get(), binner, {c, f}, toBinGrid(grid), false);
            uint64_t differ = 0;
            for (size_t i = 0; i < oracle.size(); ++i) differ += oracle[i] != overwritten[i];
            EXPECT_GT(differ, 0u) << "a plain second pass is not the fill rule (tick " << tick << ")";
        }
    }
}

// ---------------------------------------------------------------- scene
int colorDistance(const QColor &a, const QColor &b) {
    return std::abs(a.red() - b.red()) + std::abs(a.green() - b.green()) + std::abs(a.blue() - b.blue());
}
SpanRank visible(int64_t d = 0) { return {SpanTier::Visible, d}; }
SpanRank prefetch(int64_t d) { return {SpanTier::Prefetch, d}; }

// No data, loading, veil and data draw as the display shader defines them; a pan
// inside the binned rows is a pure translation (no compute pass).
TEST(HeatmapTileNodeScene, DrawsTheCellStatesAndPansWithoutRebinning) {
    if (const QString why = lab::gpuUnavailableReason(); !why.isEmpty()) GTEST_SKIP() << "GPU case skipped: " << why.toStdString();
    Scene scene;
    ASSERT_TRUE(scene.create(QSize(200, 100))) << scene.error.toStdString();
    FakeSpans spans;
    const int64_t tile = firstTile();
    const int64_t first = tiles::tileStartMs(tile, minute);
    spans.availableStartMs = first + 2 * minute; // buckets 0-1: before the data (no data)
    // Coarse only (one source): bucket 3 is a recorder gap (veil), then data.
    const auto set = spans.set(minute, {{tile, visible(), {spans.build(minute, tile, kCoarse)}}});
    scene.host->frame.spans = set;
    scene.host->frame.tfMs = minute;
    scene.host->frame.tickUnits = 1000; // $10
    // 10 buckets over 200 px (20 px each); $99,950..$100,050 over 100 px (10 bins).
    scene.host->frame.view = {double(first), double(first + 10 * minute), 99'950, 100'050};
    for (int i = 0; i < 4; ++i) ASSERT_TRUE(scene.frame()) << scene.error.toStdString();
    ASSERT_TRUE(scene.host->stats->complete.load());
    ASSERT_EQ(scene.host->stats->errors.load(), 0u);
    const auto at = [&](int x, int y = 45) { return scene.image.pixelColor(x, y); };
    EXPECT_EQ(at(10), QColor(Qt::black)) << "no data draws nothing";
    std::set<QRgb> veilTones;
    for (int x = 61; x < 79; ++x) // bucket 3: the gap
        for (int y = 41; y < 49; ++y) {
            const QColor v = at(x, y);
            veilTones.insert(v.rgb());
            EXPECT_GE(colorDistance(v, QColor(Qt::black)), 150) << "veil vs background at " << x << "," << y;
            EXPECT_LE(std::max({v.red(), v.green(), v.blue()}) - std::min({v.red(), v.green(), v.blue()}), 12)
                << "veil is neutral grey";
        }
    EXPECT_EQ(veilTones.size(), 2u) << "veil is a two-tone hatch";
    EXPECT_GE(colorDistance(at(150, 70), QColor(Qt::black)), 40) << "bids below the mid draw";
    // Pan 0.3 bucket and 0.4 bin: the same cells move, no compute pass.
    const uint64_t passes = scene.host->stats->binPasses.load();
    const QImage before = scene.image;
    scene.host->frame.view.timeLoMs += 0.3 * minute;
    scene.host->frame.view.timeHiMs += 0.3 * minute;
    ASSERT_TRUE(scene.frame());
    EXPECT_EQ(scene.host->stats->binPasses.load(), passes) << "a pan inside the rows is a translation";
    EXPECT_EQ(scene.image.pixelColor(150 - 6, 70), before.pixelColor(150, 70));
    // A later bucket not yet loaded (the next tile is not in the snapshot) draws the loading hatch.
    scene.host->frame.view = {double(first + 50 * minute), double(first + 90 * minute), 99'950, 100'050};
    ASSERT_TRUE(scene.frame());
    EXPECT_EQ(scene.host->stats->loadingSlots.load(), 1u);
    EXPECT_FALSE(scene.host->stats->complete.load());
    const QColor loadA = scene.image.pixelColor(150, 50), loadB = scene.image.pixelColor(155, 50);
    EXPECT_NE(loadA, loadB) << "loading is a hatch";
    EXPECT_NE(loadA, QColor(Qt::black));
}

// S6b palette parity (owner decision 4): the node draws a valid cell with the
// chart's palette texture and the legacy tone mapping, so a preset gives the same
// colour for the same recording code as heatmap_intensity.frag. The reference is
// legacyRecordingColor on the shared palette image (the colours of the retired
// legacy renderer, deleted in S8a, which built the same image with paletteTexels).
TEST(HeatmapTileNodeScene, PresetPaletteMatchesTheLegacyColourForEachCode) {
    if (const QString why = lab::gpuUnavailableReason(); !why.isEmpty()) GTEST_SKIP() << "GPU case skipped: " << why.toStdString();
    Scene scene;
    ASSERT_TRUE(scene.create(QSize(200, 100))) << scene.error.toStdString();
    FakeSpans spans;
    const int64_t tile = firstTile();
    const int64_t first = tiles::tileStartMs(tile, minute);
    const auto set = spans.set(minute, {{tile, visible(), {spans.build(minute, tile, kCoarse)}}});
    auto &frame = scene.host->frame;
    frame.spans = set;
    frame.tfMs = minute;
    frame.tickUnits = 1000; // $10: 10 buckets x 10 bins of 20 x 10 px
    frame.view = {double(first), double(first + 10 * minute), 99'950, 100'050};
    const CodeWindow window = codeWindow(0.01, 5);
    frame.style.codeFloor = window.floor;
    frame.style.codeRange = window.range;
    const PaletteTone tone{1.05f, 1.15f, 0.01f};
    for (const char *preset : {"Fire", "Ocean"}) {
        frame.palette = makePalette(*presetGradients(preset), tone);
        frame.capture = std::make_shared<HeatmapCellCapture>();
        for (int i = 0; i < 4; ++i) ASSERT_TRUE(scene.frame()) << scene.error.toStdString();
        ASSERT_TRUE(scene.frame()) << scene.error.toStdString(); // the capture of the last frame is filled
        ASSERT_TRUE(scene.host->stats->complete.load());
        const auto &view = frame.view;
        int compared = 0, asks = 0, bids = 0, worst = 0;
        for (const auto &block : frame.capture->blocks) {
            ASSERT_TRUE(block.result);
            const auto &g = block.grid;
            const QByteArray &data = block.result->data;
            ASSERT_EQ(data.size(), qsizetype(g.columns) * g.rows * 4);
            for (int y = 5; y < 100; y += 10)
                for (int x = 10; x < 200; x += 20) {
                    const double t = view.timeLoMs + (x + 0.5) / 200.0 * (view.timeHiMs - view.timeLoMs);
                    const double p = view.priceHi - (y + 0.5) / 100.0 * (view.priceHi - view.priceLo);
                    const int64_t col = int64_t(std::floor(t / double(g.tfMs))) - g.firstBucket;
                    const int64_t bin = int64_t(std::floor(p / g.displayTick));
                    const int64_t row = g.firstBin + int64_t(g.rows) - 1 - bin;
                    if (col < 0 || col >= int64_t(g.columns) || row < 0 || row >= int64_t(g.rows)) continue;
                    uint32_t cell = 0;
                    std::memcpy(&cell, data.constData() + (row * g.columns + col) * 4, 4);
                    if (((cell >> 16) & 3u) != uint32_t(CellState::Valid) || (cell & 0x7fffu) == 0) continue;
                    const auto want = legacyRecordingColor(uint16_t(cell & 0xffffu), window, *frame.palette);
                    const QColor got = scene.image.pixelColor(x, y);
                    const int dr = std::abs(got.red() - int(std::lround(want[0] * 255)));
                    const int dg = std::abs(got.green() - int(std::lround(want[1] * 255)));
                    const int db = std::abs(got.blue() - int(std::lround(want[2] * 255)));
                    worst = std::max({worst, dr, dg, db});
                    EXPECT_LE(std::max({dr, dg, db}), 3) << preset << " code " << (cell & 0x7fff) << " ask "
                                                          << ((cell & 0x8000) != 0) << " at " << x << "," << y;
                    ++compared;
                    ((cell & 0x8000) ? asks : bids)++;
                }
        }
        EXPECT_GE(compared, 30) << preset;
        EXPECT_GT(asks, 0) << preset;
        EXPECT_GT(bids, 0) << preset;
        std::cout << "[palette] " << preset << " cells compared=" << compared << " worst channel delta=" << worst << std::endl;
    }
}

// B1 carry-over (a): an A -> B -> C tick change under a GPU cap tighter than the
// visible content. Every frame, every drawn bin (current, held, fading) is
// resident with its sources, both earlier ticks keep fading, and the cap only
// takes content nothing draws. With the retention rules disabled (the node then
// frees what the current target does not use, as the B1 node freed tiles the
// lab stopped naming), the same scenario draws content that is gone.
struct RetentionResult {
    uint64_t missingDraws = 0, unpinnedDraws = 0, framesDrawnNotResident = 0, evictions = 0;
    uint32_t maxFading = 0;
};
RetentionResult runRetention(Scene &scene, FakeSpans &spans) {
    RetentionResult r;
    const int64_t tile = firstTile();
    auto set = spans.set(minute, {spans.both(minute, tile, visible(0)), spans.both(minute, tile + 1, visible(1)),
                                  spans.both(minute, tile + 2, prefetch(1)), spans.both(minute, tile + 3, prefetch(2)),
                                  spans.both(5 * minute, firstTile(5 * minute), {SpanTier::RecentTf, 0})});
    auto &frame = scene.host->frame;
    frame.spans = set;
    frame.tfMs = minute;
    frame.tickUnits = 500;
    frame.crossfadeMs = 60'000; // fades outlast the scenario
    frame.view = {double(tiles::tileStartMs(tile, minute)), double(tiles::tileEndMs(tile + 1, minute)), 99'800, 100'200};
    auto check = [&] {
        const auto &s = *scene.host->stats;
        if (!subset(s.drawnIds(), s.residentIds())) ++r.framesDrawnNotResident;
        r.maxFading = std::max(r.maxFading, s.fadingLayers.load());
    };
    for (int i = 0; i < 6; ++i) { scene.frame(); check(); }
    // Tight cap: the visible content alone exceeds it; prefetch and recent-tf go.
    frame.gpuCapBytes = scene.host->stats->sourceBytes.load() / 2;
    for (const int64_t tick : {1000, 2000}) { // A=$5 -> B=$10 -> C=$20
        frame.tickUnits = tick;
        for (int i = 0; i < 3; ++i) { scene.frame(); check(); }
    }
    r.missingDraws = scene.host->stats->missingDraws.load();
    r.unpinnedDraws = scene.host->stats->unpinnedDraws.load();
    r.evictions = scene.host->stats->evictions.load();
    return r;
}
TEST(HeatmapTileNodeScene, TickChangesUnderATightCapNeverDropDrawnContent) {
    if (const QString why = lab::gpuUnavailableReason(); !why.isEmpty()) GTEST_SKIP() << "GPU case skipped: " << why.toStdString();
    FakeSpans spans;
    {
        Scene scene;
        ASSERT_TRUE(scene.create(QSize(400, 200))) << scene.error.toStdString();
        const auto r = runRetention(scene, spans);
        EXPECT_EQ(r.framesDrawnNotResident, 0u);
        EXPECT_EQ(r.missingDraws, 0u);
        EXPECT_EQ(r.unpinnedDraws, 0u) << "drawn bins keep their sources resident";
        EXPECT_EQ(r.maxFading, 2u) << "A and B both fade under C";
        EXPECT_GT(r.evictions, 0u) << "the cap evicted what nothing draws (prefetch, recent-tf)";
        EXPECT_EQ(scene.host->stats->drawnTickUnits.load(), 2000);
    }
    // The same scenario without the retention rules fails.
    HeatmapTileNode::setRetentionDisabledForTest(true);
    {
        Scene scene;
        ASSERT_TRUE(scene.create(QSize(400, 200))) << scene.error.toStdString();
        const auto r = runRetention(scene, spans);
        EXPECT_GT(r.missingDraws + r.framesDrawnNotResident, 0u) << "without retention, fading content is dropped";
    }
    HeatmapTileNode::setRetentionDisabledForTest(false);
}

// The capacity contract (INV-084), against a fake controller: a report after
// every upload (with the uploaded keys, once, only when complete) and after every
// release; free = cap - resident; a released image the node holds draws on;
// one it never held is reported missing.
TEST(HeatmapTileNodeScene, CapacityReportsFollowUploadsAndReleases) {
    if (const QString why = lab::gpuUnavailableReason(); !why.isEmpty()) GTEST_SKIP() << "GPU case skipped: " << why.toStdString();
    Scene scene;
    ASSERT_TRUE(scene.create(QSize(300, 150))) << scene.error.toStdString();
    FakeSpans spans;
    const int64_t tile = firstTile();
    const auto a = spans.both(minute, tile, visible(0)), p = spans.both(minute, tile + 1, prefetch(1));
    auto capacity = std::make_shared<HeatmapCapacity>();
    auto &frame = scene.host->frame;
    frame.spans = spans.set(minute, {a, p});
    frame.capacity = capacity;
    frame.tfMs = minute;
    frame.tickUnits = 500;
    frame.uploadBudgetBytes = 256 * 1024; // several frames per source
    frame.gpuCapBytes = 64ull << 20;
    frame.view = {double(tiles::tileStartMs(tile, minute)), double(tiles::tileEndMs(tile, minute)), 99'800, 100'200};
    std::map<SpanSourceKey, int> uploaded;
    uint64_t epoch = capacity->capacityEpoch.load(), reports = 0;
    auto poll = [&] {
        if (capacity->capacityEpoch.load() == epoch) return false;
        epoch = capacity->capacityEpoch.load();
        auto report = capacity->take();
        ++reports;
        for (const auto &key : report.uploaded) ++uploaded[key];
        EXPECT_EQ(report.freeBytes, frame.gpuCapBytes - scene.host->stats->residentBytes.load());
        EXPECT_FALSE(report.lost);
        return true;
    };
    uint64_t sourcesUploaded = 0;
    for (int i = 0; i < 200 && uploaded.size() < 4; ++i) {
        ASSERT_TRUE(scene.frame());
        const bool reported = poll();
        const uint64_t now = scene.host->stats->sourcesUploaded.load();
        if (now != sourcesUploaded) EXPECT_TRUE(reported) << "an upload is reported in its frame";
        sourcesUploaded = now;
        EXPECT_EQ(uploaded.size(), size_t(now)) << "a key is reported once its upload completed, not before";
    }
    ASSERT_EQ(uploaded.size(), 4u);
    for (const auto &[key, count] : uploaded) EXPECT_EQ(count, 1) << key.source << " reported once";
    // The controller releases the images: same keys, gpu == nullptr. Nothing changes.
    FakeSpans::Span ra = a, rp = p;
    for (auto &b : ra.sources) b = FakeSpans::released(b);
    for (auto &b : rp.sources) b = FakeSpans::released(b);
    frame.spans = spans.set(minute, {ra, rp});
    for (int i = 0; i < 3; ++i) ASSERT_TRUE(scene.frame());
    EXPECT_TRUE(scene.host->stats->complete.load()) << "the resident copies draw";
    EXPECT_EQ(scene.host->stats->missingReports.load(), 0u);
    poll();
    // The prefetch span leaves the snapshot: its sources are freed and reported.
    const uint64_t resident = scene.host->stats->residentBytes.load();
    frame.spans = spans.set(minute, {ra});
    ASSERT_TRUE(scene.frame());
    ASSERT_TRUE(poll()) << "a release is reported";
    EXPECT_LT(scene.host->stats->residentBytes.load(), resident);
    // A released image the node never held (another node uploaded it) is missing.
    FakeSpans::Span far = spans.both(minute, tile + 5, prefetch(3));
    for (auto &b : far.sources) b = FakeSpans::released(b);
    frame.spans = spans.set(minute, {ra, far});
    ASSERT_TRUE(scene.frame());
    ASSERT_NE(capacity->capacityEpoch.load(), epoch);
    const auto report = capacity->take();
    epoch = capacity->capacityEpoch.load();
    EXPECT_EQ(report.missing.size(), 2u);
    ASSERT_TRUE(scene.frame());
    EXPECT_EQ(capacity->capacityEpoch.load(), epoch) << "missing keys are reported once";
}

// Visible spans the CPU ceiling refused (SpanSet::refused) draw as loading, and
// do not keep the view from being complete otherwise.
TEST(HeatmapTileNodeScene, RefusedSpansDrawTheLoadingHatch) {
    if (const QString why = lab::gpuUnavailableReason(); !why.isEmpty()) GTEST_SKIP() << "GPU case skipped: " << why.toStdString();
    Scene scene;
    ASSERT_TRUE(scene.create(QSize(256, 100))) << scene.error.toStdString();
    FakeSpans spans;
    const int64_t tile = firstTile();
    auto &frame = scene.host->frame;
    frame.spans = spans.set(minute, {spans.both(minute, tile, visible(0))}, {SpanId{"BTC-USD", minute, tile + 1}});
    frame.tfMs = minute;
    frame.tickUnits = 500;
    // Two tiles over 256 px: the left one drawn, the right one refused.
    frame.view = {double(tiles::tileStartMs(tile, minute)), double(tiles::tileEndMs(tile + 1, minute)), 99'900, 100'100};
    for (int i = 0; i < 4; ++i) ASSERT_TRUE(scene.frame());
    EXPECT_EQ(scene.host->stats->refusedSlots.load(), 1u);
    EXPECT_EQ(scene.host->stats->loadingSlots.load(), 1u);
    EXPECT_EQ(scene.host->stats->readySlots.load(), 1u);
    EXPECT_TRUE(scene.host->stats->complete.load()) << "a refused span waits for capacity, not for the node";
    std::set<QRgb> tones;
    for (int x = 140; x < 250; ++x) tones.insert(scene.image.pixelColor(x, 50).rgb());
    EXPECT_GE(tones.size(), 2u) << "the refused tile shows the hatch";
    EXPECT_EQ(tones.count(QColor(Qt::black).rgb()), 0u);
}

// FM-102: a tile quad edge exactly on a pixel centre leaves no transparent seam
// (1500 px over 720 one-minute columns, an edge 114 columns in).
TEST(HeatmapTileNodeScene, TileEdgesOnPixelCentresLeaveNoSeam) {
    if (const QString why = lab::gpuUnavailableReason(); !why.isEmpty()) GTEST_SKIP() << "GPU case skipped: " << why.toStdString();
    Scene scene;
    ASSERT_TRUE(scene.create(QSize(1500, 60))) << scene.error.toStdString();
    FakeSpans spans;
    spans.availableEndMs = kEpoch + 24 * kHourMs; // the view stays inside the data
    spans.shape.coarseLo = 99'900;
    spans.shape.coarseHi = 100'100;
    spans.shape.coarseGaps = false;
    const int64_t first = firstTile();
    std::vector<FakeSpans::Span> list;
    for (int64_t t = first; t < first + 13; ++t) list.push_back({t, visible(t - first), {spans.build(minute, t, kCoarse)}});
    auto &frame = scene.host->frame;
    frame.spans = spans.set(minute, list);
    frame.tfMs = minute;
    frame.tickUnits = 1000;
    const double start = double(tiles::tileFirstBucket(first + 2) - 114);
    // Above the book: every present column draws its outside-the-book veil (opaque).
    frame.view = {start * double(minute), (start + 720) * double(minute), 101'000, 101'400};
    for (int i = 0; i < 3; ++i) ASSERT_TRUE(scene.frame());
    ASSERT_TRUE(scene.host->stats->complete.load());
    const QImage image = scene.image.convertToFormat(QImage::Format_RGBA8888);
    int seams = 0;
    QString where;
    for (int x = 0; x < image.width(); ++x)
        if (image.pixelColor(x, 30).rgb() == QColor(Qt::black).rgb()) {
            ++seams;
            if (seams < 40) where += QString::number(x) + " ";
        }
    EXPECT_EQ(seams, 0) << "transparent pixel columns between tiles at x = " << where.toStdString();
}

// FM-099: scene graph invalidation right after the first frames (the precision
// self-test in flight) tears node, binner and QRhi down cleanly.
TEST(HeatmapTileNodeScene, SceneGraphInvalidationAfterTheFirstFrameIsClean) {
    if (const QString why = lab::gpuUnavailableReason(); !why.isEmpty()) GTEST_SKIP() << "GPU case skipped: " << why.toStdString();
    FakeSpans spans;
    const auto set = spans.set(minute, {spans.both(minute, firstTile(), visible())});
    for (int frames = 1; frames <= 3; ++frames) {
        HeatmapGpuBinner::clearSelfTestCacheForTest();
        auto scene = std::make_unique<Scene>();
        ASSERT_TRUE(scene->create(QSize(200, 100))) << scene->error.toStdString();
        scene->host->frame.spans = set;
        scene->host->frame.tfMs = minute;
        scene->host->frame.tickUnits = 1000;
        scene->host->frame.view = {double(tiles::tileStartMs(firstTile(), minute)),
                                   double(tiles::tileEndMs(firstTile(), minute)), 99'900, 100'100};
        for (int i = 0; i < frames; ++i) ASSERT_TRUE(scene->frame()) << scene->error.toStdString();
        scene.reset();
    }
}

// Review fix 1: the QRhi dies first (scene graph invalidation). Its cleanup
// traversal runs the nodes' callbacks and the binners' callbacks in hash order;
// no callback may destroy an object that owns a callback (the binner), or its
// destructor removes a callback from the hash the QRhi is iterating. Several
// nodes make some node callback run before its binner's.
TEST(HeatmapTileNodeScene, QRhiDestroyedFirstLeavesTheCleanupTraversalIntact) {
    if (const QString why = lab::gpuUnavailableReason(); !why.isEmpty()) GTEST_SKIP() << "GPU case skipped: " << why.toStdString();
    auto device = std::make_unique<lab::HeadlessRhi>();
    ASSERT_TRUE(device->create()) << device->error.toStdString();
    std::vector<std::unique_ptr<HeatmapTileNode>> nodes;
    for (int i = 0; i < 16; ++i) {
        nodes.push_back(std::make_unique<HeatmapTileNode>());
        nodes.back()->attachRhiForTest(device->rhi.get());
    }
    const auto before = HeatmapGpuBinner::callbackRemovalsDuringCleanupForTest();
    device->rhi.reset(); // the QRhi goes first
    EXPECT_EQ(HeatmapGpuBinner::callbackRemovalsDuringCleanupForTest(), before)
        << "a cleanup callback destroyed an object that owns a callback";
    nodes.clear(); // after the QRhi: must not call into it
}

// Review fix 2: the cap is enforced before a frame records uploads, and never
// releases a source the same frame uploaded into (its commands still reference
// the buffers). Here one frame uploads a prefetch span, then the visible bin
// pushes residency over the cap.
TEST(HeatmapTileNodeScene, CapEvictionNeverReleasesWhatTheFrameUploaded) {
    if (const QString why = lab::gpuUnavailableReason(); !why.isEmpty()) GTEST_SKIP() << "GPU case skipped: " << why.toStdString();
    Scene scene;
    ASSERT_TRUE(scene.create(QSize(400, 400))) << scene.error.toStdString();
    FakeSpans spans;
    const int64_t tile = firstTile();
    const auto v = spans.both(minute, tile, visible()), p = spans.both(minute, tile + 1, prefetch(1));
    size_t sources = 0;
    for (const auto *span : {&v, &p})
        for (const auto &b : span->sources) sources += b->uploadBytes + 256;
    auto &frame = scene.host->frame;
    frame.spans = spans.set(minute, {v, p});
    frame.tfMs = minute;
    frame.tickUnits = 100; // $1 over $3000: a tall bin
    frame.view = {double(tiles::tileStartMs(tile, minute)), double(tiles::tileEndMs(tile, minute)), 98'500, 101'500};
    frame.uploadBudgetBytes = 1ull << 30; // everything uploads in the first frame
    frame.gpuCapBytes = sources + 64 * 1024; // the sources fit, the visible bin does not
    for (int i = 0; i < 4; ++i) ASSERT_TRUE(scene.frame());
    const auto &st = *scene.host->stats;
    EXPECT_GT(st.binBytes.load(), 64u * 1024) << "the bin pushes residency over the cap";
    EXPECT_GT(st.evictions.load(), 0u) << "the prefetch source was evicted";
    EXPECT_EQ(st.sameFrameReleases.load(), 0u) << "never in the frame that uploaded it";
    EXPECT_EQ(st.missingDraws.load(), 0u);
    EXPECT_TRUE(st.complete.load());
}

// Review fix 3: a visible span whose sources all failed terminally is resolved,
// not pending: a timeframe change finishes (that span draws loading) instead of
// holding the old picture forever.
TEST(HeatmapTileNodeScene, ASpanWhoseSourcesAllFailedDoesNotHoldATransition) {
    if (const QString why = lab::gpuUnavailableReason(); !why.isEmpty()) GTEST_SKIP() << "GPU case skipped: " << why.toStdString();
    Scene scene;
    ASSERT_TRUE(scene.create(QSize(300, 150))) << scene.error.toStdString();
    FakeSpans spans;
    const int64_t tile = firstTile();
    auto &frame = scene.host->frame;
    frame.spans = spans.set(minute, {spans.both(minute, tile, visible()), spans.both(minute, tile + 1, visible(1))});
    frame.tfMs = minute;
    frame.tickUnits = 500;
    frame.view = {double(tiles::tileStartMs(tile, minute)), double(tiles::tileEndMs(tile + 1, minute)), 99'800, 100'200};
    for (int i = 0; i < 3; ++i) ASSERT_TRUE(scene.frame());
    ASSERT_TRUE(scene.host->stats->complete.load());
    // 5m: the one visible span has only failed sources.
    FakeSpans::Span failed{firstTile(5 * minute), visible(), {}, 5 * minute, {kCoarse, kFine}};
    frame.spans = spans.set(5 * minute, {failed});
    frame.tfMs = 5 * minute;
    for (int i = 0; i < 3; ++i) ASSERT_TRUE(scene.frame());
    EXPECT_FALSE(scene.host->stats->holding.load()) << "the transition finished";
    EXPECT_EQ(scene.host->stats->drawnTfMs.load(), 5 * minute);
    EXPECT_GE(scene.host->stats->loadingSlots.load(), 1u) << "the failed span draws loading";
    EXPECT_FALSE(scene.host->stats->complete.load()) << "but the view is not complete";
}

// Review fix 4: a raised GPU cap is new free bytes even when nothing moves: the
// node reports it, so suppressed prefetch can come back.
TEST(HeatmapTileNodeScene, ARaisedCapIsReportedWithNothingMoving) {
    if (const QString why = lab::gpuUnavailableReason(); !why.isEmpty()) GTEST_SKIP() << "GPU case skipped: " << why.toStdString();
    Scene scene;
    ASSERT_TRUE(scene.create(QSize(300, 150))) << scene.error.toStdString();
    FakeSpans spans;
    auto capacity = std::make_shared<HeatmapCapacity>();
    auto &frame = scene.host->frame;
    frame.spans = spans.set(minute, {spans.both(minute, firstTile(), visible())});
    frame.capacity = capacity;
    frame.tfMs = minute;
    frame.tickUnits = 500;
    frame.gpuCapBytes = 32ull << 20;
    frame.view = {double(tiles::tileStartMs(firstTile(), minute)), double(tiles::tileEndMs(firstTile(), minute)), 99'800, 100'200};
    for (int i = 0; i < 4; ++i) ASSERT_TRUE(scene.frame());
    capacity->take();
    const uint64_t epoch = capacity->capacityEpoch.load();
    ASSERT_TRUE(scene.frame());
    ASSERT_EQ(capacity->capacityEpoch.load(), epoch) << "a static view reports nothing";
    frame.gpuCapBytes = 64ull << 20;
    ASSERT_TRUE(scene.frame());
    ASSERT_NE(capacity->capacityEpoch.load(), epoch) << "the new cap is reported";
    EXPECT_EQ(capacity->take().freeBytes, (64ull << 20) - scene.host->stats->residentBytes.load());
}

// ---------------------------------------------------------------- with the controller
// The production pair: HeatmapSourceController on FakeChunkTransport (manual
// build executor) and the node, frame by frame on one thread.
class NodeWithController : public testing::Test {
protected:
    ChunkStore store;
    FakeChunkTransport transport;
    ChunkFetcher fetcher{store, transport};
    std::deque<std::function<void()>> jobs;
    std::unique_ptr<SpanSourceCache> cache;
    std::unique_ptr<HeatmapSourceController> controller;
    std::unique_ptr<Scene> scene;
    size_t answered = 0;
    uint64_t revision = 1;
    const int64_t start = kEpoch, end = kEpoch + 8 * kHourMs;

    void SetUp() override {
        if (const QString why = lab::gpuUnavailableReason(); !why.isEmpty()) GTEST_SKIP() << "GPU case skipped: " << why.toStdString();
        SpanSourceCache::Options options;
        options.executor = [this](std::function<void()> job, int) { jobs.push_back(std::move(job)); };
        cache = std::make_unique<SpanSourceCache>(options);
        HeatmapSourceController::Options c;
        c.capacityPollMs = 0;
        controller = std::make_unique<HeatmapSourceController>(store, fetcher, *cache, c);
        transport.goOnline();
        transport.push(availability(start, end));
        drain();
        scene = std::make_unique<Scene>();
        ASSERT_TRUE(scene->create(QSize(400, 200))) << scene->error.toStdString();
        scene->host->frame.capacity = controller->capacity();
        scene->host->frame.tfMs = minute;
        scene->host->frame.tickUnits = 500;
    }
    void TearDown() override {
        scene.reset();
        controller.reset();
        jobs.clear();
        cache.reset();
        drain();
    }
    static void drain() {
        for (int i = 0; i < 32; ++i) QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    }
    void view(int64_t tfMs, double lo, double hi) {
        scene->host->frame.tfMs = tfMs;
        scene->host->frame.view.timeLoMs = lo;
        scene->host->frame.view.timeHiMs = hi;
        controller->setView("BTC-USD", tfMs, lo, hi);
    }
    // One frame: answer requests, run builds (unless `serve` is false: a slow
    // network and a busy build pool), render, then the controller takes the
    // node's report.
    bool frame(bool serve = true) {
        drain();
        while (serve && answered < transport.requests.size()) {
            const auto request = transport.requests[answered++];
            for (size_t i = 0; i < request.starts.size(); ++i) transport.reply(request.id, chunkFrame(request.key(i), revision));
        }
        drain();
        while (serve && !jobs.empty()) {
            auto job = std::move(jobs.front());
            jobs.pop_front();
            job();
        }
        drain();
        scene->host->frame.spans = controller->latestSnapshot();
        if (!scene->frame()) return false;
        controller->pollCapacity();
        drain();
        return true;
    }
    bool settle(int maxFrames = 200) {
        for (int i = 0; i < maxFrames; ++i) {
            if (!frame()) return false;
            if (scene->host->stats->complete.load() && jobs.empty() && answered == transport.requests.size()) return true;
        }
        return false;
    }
    bool released() {
        const auto set = controller->latestSnapshot();
        if (!set) return false;
        bool any = false;
        for (const auto &span : set->spans)
            for (const auto &source : span.sources) {
                if (!source.build || span.rank.tier != SpanTier::Visible) continue;
                if (source.build->gpu) return false;
                any = true;
            }
        return any;
    }
};

// Uploaded -> the controller releases the CPU image; the node draws its copy.
TEST_F(NodeWithController, UploadedImagesAreReleasedAndTheNodeKeepsDrawing) {
    view(minute, double(kEpoch + 2 * kTileMs), double(kEpoch + 3 * kTileMs));
    scene->host->frame.view.priceLo = 99'800;
    scene->host->frame.view.priceHi = 100'200;
    ASSERT_TRUE(settle());
    for (int i = 0; i < 5 && !released(); ++i) ASSERT_TRUE(frame());
    EXPECT_TRUE(released()) << "every visible image released once uploaded";
    EXPECT_GT(controller->stats().releasedImages, 0u);
    EXPECT_EQ(cache->stats().claimedBytes, 0u) << "no CPU claim left for uploaded images";
    ASSERT_TRUE(frame());
    EXPECT_TRUE(scene->host->stats->complete.load());
    EXPECT_GT(scene->host->stats->fillPasses.load(), 0u) << "the fine source fills where the view overlaps its band";
    EXPECT_EQ(scene->host->stats->missingReports.load(), 0u);
}

// QRhi loss: the node reports lost, the controller rebuilds the released images
// from its local chunks (no new requests), and the new node uploads them.
TEST_F(NodeWithController, QRhiRecreationReportsLossAndTheControllerRebuilds) {
    view(minute, double(kEpoch + 2 * kTileMs), double(kEpoch + 3 * kTileMs));
    scene->host->frame.view.priceLo = 99'800;
    scene->host->frame.view.priceHi = 100'200;
    ASSERT_TRUE(settle());
    for (int i = 0; i < 5 && !released(); ++i) ASSERT_TRUE(frame());
    ASSERT_TRUE(released());
    const auto builds = cache->stats().builds;
    const auto requests = transport.requests.size();
    // Move the chart to a new scene (new QRhi): the old node goes with its GPU content.
    const auto keep = scene->host->frame;
    scene.reset();
    auto fresh = std::make_unique<Scene>();
    ASSERT_TRUE(fresh->create(QSize(400, 200))) << fresh->error.toStdString();
    fresh->host->frame = keep;
    scene = std::move(fresh);
    ASSERT_TRUE(settle());
    EXPECT_GT(cache->stats().builds, builds) << "released images were rebuilt after the loss";
    EXPECT_EQ(transport.requests.size(), requests) << "from local chunks";
    EXPECT_TRUE(scene->host->stats->complete.load());
    EXPECT_GT(scene->host->stats->sourcesUploaded.load(), 0u);
}

// Never blank: across a timeframe change and back, tick changes and a revised
// chunk, every frame after the first complete picture draws content over the
// whole view (held, fading, fallback or new content): no loading hatch, no
// visible span without a drawn bin, nothing drawn that is not resident.
TEST_F(NodeWithController, NeverBlanksAcrossTimeframeTickAndRevision) {
    const double lo = double(kEpoch + 2 * kTileMs), hi = double(kEpoch + 4 * kTileMs);
    view(minute, lo, hi);
    scene->host->frame.view.priceLo = 99'700;
    scene->host->frame.view.priceHi = 100'300;
    scene->host->frame.crossfadeMs = 30;
    ASSERT_TRUE(settle());
    uint64_t blankFrames = 0, loadingFrames = 0, frames = 0;
    uint64_t heldFrames = 0, fallbackFrames = 0, partialFrames = 0;
    auto watch = [&](int n, bool serve = true) {
        for (int i = 0; i < n; ++i) {
            ASSERT_TRUE(frame(serve));
            ++frames;
            const auto &s = *scene->host->stats;
            loadingFrames += s.loadingSlots.load() > 0;
            // Holding draws the old picture over the view (gaps would be hatched);
            // otherwise every visible span draws a bin of its own.
            const bool covered = s.holding.load() ||
                                 s.readySlots.load() + s.fallbackSlots.load() + s.partialSlots.load() == s.slotCount.load();
            blankFrames += !covered || s.drawnIds().empty() || !subset(s.drawnIds(), s.residentIds());
            heldFrames += s.holding.load();
            fallbackFrames += s.fallbackSlots.load() > 0;
            partialFrames += s.partialSlots.load() > 0; // content missing a source: never after the first picture
        }
    };
    view(5 * minute, lo, hi); // the old picture holds until the 5m spans are built
    watch(5, false);         // nothing arrives for 5 frames
    watch(12);
    ASSERT_TRUE(settle());
    EXPECT_EQ(scene->host->stats->drawnTfMs.load(), 5 * minute);
    view(minute, lo, hi); // back: the recent-tf spans are resident
    watch(6);
    ASSERT_TRUE(settle());
    for (const int64_t tick : {1000, 2000, 500}) {
        scene->host->frame.tickUnits = tick;
        watch(3);
    }
    // A revised chunk: every span that used it rebuilds; the old bins draw meanwhile.
    ++revision;
    const auto set = controller->latestSnapshot();
    ASSERT_TRUE(set);
    const auto newestHour = recording::floorDiv(int64_t(hi) - 1, kHourMs) * kHourMs;
    const ChunkKey key{"BTC-USD", kCoarse, kMinuteMs, newestHour};
    const auto body = chunkFrame(key, revision);
    ASSERT_TRUE(store.put(key, std::shared_ptr<const SparseColumns>(body, &body->columns), body->state, body->contentHash));
    scene->host->frame.uploadBudgetBytes = 64 * 1024; // the rebuilt source uploads over several frames
    watch(5, false); // the rebuild waits for the pool: the stale version draws
    watch(40);
    scene->host->frame.uploadBudgetBytes = 8ull << 20;
    ASSERT_TRUE(settle());
    EXPECT_GT(frames, 30u);
    EXPECT_EQ(loadingFrames, 0u) << "no frame fell back to the loading hatch";
    EXPECT_EQ(blankFrames, 0u) << "no frame left a visible span without drawn content";
    EXPECT_GE(heldFrames, 5u) << "the timeframe change held the old picture while nothing arrived";
    EXPECT_GT(fallbackFrames, 0u) << "the revised span drew its previous content while the new one uploaded";
    EXPECT_EQ(partialFrames, 0u) << "never a span drawn without one of its sources";
    EXPECT_EQ(scene->host->stats->missingDraws.load(), 0u);
}
// Ported B1 check: repeated revisions of the visible spans return to the
// residency budget: every old version is freed once its successor draws.
TEST_F(NodeWithController, RepeatedVisibleRevisionsReturnToTheBudget) {
    view(minute, double(kEpoch + 2 * kTileMs), double(kEpoch + 3 * kTileMs));
    scene->host->frame.view.priceLo = 99'800;
    scene->host->frame.view.priceHi = 100'200;
    ASSERT_TRUE(settle());
    for (int i = 0; i < 5; ++i) ASSERT_TRUE(frame());
    const auto &st = *scene->host->stats;
    const uint64_t sources = st.residentSources.load(), bytes = st.residentBytes.load();
    // A tight cap: exactly what is resident now.
    scene->host->frame.gpuCapBytes = bytes;
    controller->setGpuBudget(bytes);
    const int64_t hourInView = recording::floorDiv(kEpoch + 2 * kTileMs, kHourMs) * kHourMs;
    for (int r = 0; r < 4; ++r) {
        ++revision;
        for (const auto *source : {&kCoarse, &kFine}) {
            const ChunkKey key{"BTC-USD", *source, kMinuteMs, hourInView};
            const auto body = chunkFrame(key, revision);
            ASSERT_TRUE(store.put(key, std::shared_ptr<const SparseColumns>(body, &body->columns), body->state,
                                  body->contentHash));
        }
        ASSERT_TRUE(settle()) << "revision " << r;
        for (int i = 0; i < 5; ++i) ASSERT_TRUE(frame());
        EXPECT_LE(st.residentSources.load(), sources) << "revision " << r << ": old versions freed";
        EXPECT_LE(st.residentBytes.load(), bytes) << "revision " << r << ": back within the budget";
    }
    EXPECT_EQ(st.missingDraws.load(), 0u);
}

// Ported B1 check: with the cap full of the view, the evicted prefetch settles
// (no rebuild or upload churn while the view stands) and comes back once the
// budget grows.
TEST_F(NodeWithController, BudgetEvictedPrefetchSettlesAndReturnsWhenTheBudgetGrows) {
    view(minute, double(kEpoch + 2 * kTileMs), double(kEpoch + 3 * kTileMs));
    scene->host->frame.view.priceLo = 99'800;
    scene->host->frame.view.priceHi = 100'200;
    ASSERT_TRUE(settle());
    const auto &st = *scene->host->stats;
    const uint64_t all = st.residentSources.load();
    // Room for the visible span (its two sources and its bin), not for prefetch.
    uint64_t visibleBytes = st.binBytes.load() + 64 * 1024;
    for (const auto &span : controller->latestSnapshot()->spans)
        if (span.rank.tier == SpanTier::Visible)
            for (const auto &source : span.sources) visibleBytes += source.build->uploadBytes + 256;
    scene->host->frame.gpuCapBytes = visibleBytes;
    controller->setGpuBudget(visibleBytes);
    for (int i = 0; i < 20; ++i) ASSERT_TRUE(frame());
    ASSERT_LT(st.residentSources.load(), all) << "prefetch evicted";
    EXPECT_TRUE(st.complete.load());
    const auto builds = cache->stats().builds;
    const uint64_t uploads = st.sourcesUploaded.load(), evictions = st.evictions.load();
    const auto requests = transport.requests.size();
    for (int i = 0; i < 60; ++i) ASSERT_TRUE(frame()); // the view stands
    EXPECT_EQ(cache->stats().builds, builds) << "no rebuild churn";
    EXPECT_EQ(st.sourcesUploaded.load(), uploads) << "no upload churn";
    EXPECT_EQ(st.evictions.load(), evictions) << "no eviction churn";
    EXPECT_EQ(transport.requests.size(), requests);
    // The budget grows: prefetch returns.
    scene->host->frame.gpuCapBytes = 320ull << 20;
    controller->setGpuBudget(320ull << 20);
    for (int i = 0; i < 40 && st.residentSources.load() < all; ++i) ASSERT_TRUE(frame());
    EXPECT_EQ(st.residentSources.load(), all) << "prefetch is back";
}
} // namespace

int main(int argc, char **argv) {
    // Offscreen unless set: Vulkan needs a real platform plugin (QT_QPA_PLATFORM=windows|xcb).
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    lab::selectQuickSceneGraph(); // before any QQuickWindow: Qt fixes the backend at the first one
    ::testing::InitGoogleTest(&argc, argv);
    std::cout << "[sentinel] " << lab::describeRhi().toStdString() << std::endl;
    return RUN_ALL_TESTS();
}
