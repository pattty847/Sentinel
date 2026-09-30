// Slice B1 GPU cases: whole-chunk tiles binned on the GPU (HeatmapGpuBinner::binInto)
// equal the CPU render-ready cells (heatmap::tiles::buildCells) cell for cell, and
// HeatmapTileNode draws tiles by mapping only (no work on pan), holds the old
// picture until a new tick's tiles are resident, and hatches unprepared time.
// GPU cases skip cleanly without a Metal device.
#include "heatmap/HeatmapTiles.hpp"
#include "heatmap/TimeComposer.hpp"
#include "lab/OffscreenQuick.hpp"
#include "render/heatmap/HeatmapGpuBinner.hpp"
#include "render/heatmap/HeatmapTileNode.hpp"
#include "servermodel/Hmc2Store.hpp"
#include <QGuiApplication>
#include <QQuickItem>
#include <QQuickWindow>
#include <gtest/gtest.h>
#include <rhi/qrhi.h>
#include <rhi/qrhi_platform.h>
#include <cmath>
#include <cstring>

namespace {
using namespace heatmap;
using namespace heatmap::gpu;
constexpr int64_t minute = kMinuteMs, hour = kHourMs, day = kDayMs;
constexpr int64_t epoch = recording::kHmc2MinMs + 50 * day;

// Minutes with a $10 -> $5 grid change at 50, a size-scale change at 100, gaps,
// partial coverage, and a sparse far-away wall so tall tiles need two row blocks.
SparseColumns minutes(int64_t count, bool farWall) {
    SparseColumns out{"BTC-USD", "deep", minute, epoch, epoch + count * minute, {}, {{epoch, epoch + count * minute}}};
    for (int64_t i = 0; i < count; ++i) {
        if (i % 13 == 5) continue;
        const int64_t tickUnits = i < 50 ? 1000 : 500;
        NativeColumn n;
        n.grid = {uint64_t(i < 50 ? 1 : 2), tickUnits, 100};
        if (i >= 100) n.sizeScale.floor = 1e-8;
        n.observedMs = i % 7 ? minute : 23'000;
        const double tick = tickUnits / 100.0;
        const int64_t mid = int64_t((100'000 + 30 * std::sin(double(i) / 9.0)) / tick);
        const int64_t lo = mid - 20, hi = mid + 19 + (farWall ? int64_t(90'000 / tick) : 0);
        n.baseRow = lo;
        n.coverage[0] = {{lo + (i % 5 == 0 ? 3 : 0), mid + 19, n.observedMs}};
        n.coverage[1] = {{lo, hi, n.observedMs}};
        for (int64_t row = lo; row <= mid + 19; ++row) {
            if ((row + i) % 4 == 1) continue;
            const bool ask = row >= mid;
            const double size = 0.00002 * (1 + (row * 31 + i * 17) % 113) * (1 + (i % 11) * 0.3);
            n.entries.push_back({packRowSide(row, lo, ask), recording::encodeSize(size, n.sizeScale)});
        }
        if (farWall) n.entries.push_back({packRowSide(hi, lo, true), recording::encodeSize(5.0, n.sizeScale)});
        out.columns.push_back({epoch + i * minute, n.observedMs, 0, {std::move(n)}});
    }
    validate(out);
    return out;
}

struct Headless {
    std::unique_ptr<QRhi> rhi;
    Headless() {
        if (!lab::metalDeviceAvailable()) return;
#ifdef Q_OS_MACOS
        QRhiMetalInitParams init;
        rhi.reset(QRhi::create(QRhi::Metal, &init));
#endif
    }
};

BinGrid toBinGrid(const tiles::TileGrid &g) { return {g.tfMs, g.firstBucket, g.columns, g.tick, g.firstBin, g.rows}; }

// Bins `grid` into a fresh buffer block by block (as HeatmapTileNode does) and reads it back.
std::vector<uint32_t> binTile(QRhi *rhi, HeatmapGpuBinner &binner, const BinGrid &grid) {
    std::vector<uint32_t> out(size_t(grid.columns) * grid.rows);
    for (uint32_t top = 0; top < grid.rows; top += tiles::kTileBlockRows) {
        BinGrid block = grid;
        block.rows = std::min(tiles::kTileBlockRows, grid.rows - top);
        block.firstBin = grid.firstBin + int64_t(grid.rows - top - block.rows);
        const uint64_t bytes = uint64_t(block.columns) * block.rows * 4;
        std::unique_ptr<QRhiBuffer> target(rhi->newBuffer(QRhiBuffer::Static, QRhiBuffer::StorageBuffer, quint32((bytes + 15) / 16 * 16)));
        EXPECT_TRUE(target->create());
        QRhiReadbackResult result;
        QRhiCommandBuffer *cb = nullptr;
        QString error;
        EXPECT_EQ(rhi->beginOffscreenFrame(&cb), QRhi::FrameOpSuccess);
        EXPECT_TRUE(binner.binInto(cb, block, {}, target.get(), &error)) << error.toStdString();
        auto *updates = rhi->nextResourceUpdateBatch();
        updates->readBackBuffer(target.get(), 0, quint32(bytes), &result);
        cb->resourceUpdate(updates);
        EXPECT_EQ(rhi->endOffscreenFrame(), QRhi::FrameOpSuccess);
        if (result.data.size() != qsizetype(bytes)) { ADD_FAILURE() << "readback size"; return out; }
        std::memcpy(out.data() + size_t(top) * grid.columns, result.data.constData(), bytes);
    }
    return out;
}

TEST(HeatmapTileGpu, BinIntoMatchesTheCpuRenderReadyCellsExactly) {
    Headless gpu;
    if (!gpu.rhi) GTEST_SKIP() << "No MTLDevice; GPU tile parity requires Metal";
    for (const bool farWall : {false, true}) {
        const auto data = minutes(150, farWall);
        for (const int64_t tf : {minute, 5 * minute}) {
            const int64_t tile = tiles::tileOfBucket(epoch / tf) + (tf == minute ? 1 : 0);
            const int64_t start = tiles::tileStartMs(tile, tf), end = tiles::tileEndMs(tile, tf);
            ComposeOptions clip;
            clip.startMs = start;
            clip.endMs = end;
            const SparseColumns *level = &data;
            const auto composed = compose(std::span<const SparseColumns *const>(&level, 1), tf, clip);
            GpuSourceOptions options;
            options.availableStartMs = std::max(epoch, start) + 3 * tf; // no data before, inside the tile
            options.availableEndMs = std::min(epoch + 140 * minute, end - 2 * tf);
            auto source = std::make_shared<const GpuSource>(buildGpuSource(composed, options));
            HeatmapGpuBinner binner(gpu.rhi.get());
            binner.forceKernel(KernelVariant::Fast);
            QString error;
            QRhiCommandBuffer *cb = nullptr;
            ASSERT_TRUE(binner.setSource(source, &error));
            ASSERT_EQ(gpu.rhi->beginOffscreenFrame(&cb), QRhi::FrameOpSuccess);
            ASSERT_TRUE(binner.uploadAll(cb, &error)) << error.toStdString();
            ASSERT_EQ(gpu.rhi->endOffscreenFrame(), QRhi::FrameOpSuccess);
            tiles::CellOptions cellOptions;
            cellOptions.availableStartMs = *options.availableStartMs;
            cellOptions.availableEndMs = *options.availableEndMs;
            for (const int64_t tickUnits : {500, 1000, 2000, 5000, 750}) {
                const auto units = tiles::usefulUnits(composed, 100);
                const auto grid = tiles::tileGrid(tile, tf, tickUnits, 100, tiles::binsOf(units, tickUnits), 0);
                ASSERT_TRUE(grid);
                if (farWall && tickUnits == 500) EXPECT_GT(grid->rows, tiles::kTileBlockRows) << "two row blocks";
                const auto cpu = tiles::buildCells(composed, *grid, cellOptions);
                const auto gpuCells = binTile(gpu.rhi.get(), binner, toBinGrid(*grid));
                uint64_t valid = 0, mismatches = 0;
                std::array<uint64_t, 4> states{};
                for (size_t i = 0; i < cpu.size(); ++i) {
                    const uint32_t a = cpu[i], b = gpuCells[i];
                    ++states[tiles::cellState(a)];
                    if (tiles::cellState(a) != tiles::cellState(b)) { ++mismatches; continue; }
                    if (tiles::cellState(a) == tiles::kCellValid) {
                        ++valid;
                        mismatches += (a & 0xffffu) != (b & 0xffffu);
                    }
                }
                EXPECT_EQ(mismatches, 0u) << "tf=" << tf << " tick=" << tickUnits << " farWall=" << farWall;
                if (tickUnits == 750) EXPECT_EQ(valid, 0u);
                else EXPECT_GT(valid, 0u);
                EXPECT_GT(states[tiles::kCellVeil], 0u);
                EXPECT_GT(states[tiles::kCellNoData], 0u);
            }
        }
    }
}

// ---------------------------------------------------------------- scene
class TileHost : public QQuickItem {
public:
    TileHost() { setFlag(ItemHasContents, true); }
    std::shared_ptr<HeatmapTileStats> stats = std::make_shared<HeatmapTileStats>();
    HeatmapTileNode::Frame frame;
    QSGNode *updatePaintNode(QSGNode *old, UpdatePaintNodeData *) override {
        auto *node = old ? static_cast<HeatmapTileNode *>(old) : new HeatmapTileNode(stats);
        auto f = frame;
        f.rect = QRectF(0, 0, width(), height());
        node->setFrame(std::move(f));
        return node;
    }
};

TEST(HeatmapTileNodeScene, PanIsMappingOnlyAndANewTickHoldsTheOldPictureUntilResident) {
    if (!lab::metalDeviceAvailable()) GTEST_SKIP() << "No MTLDevice";
    lab::OffscreenQuick scene;
    QString error;
    ASSERT_TRUE(scene.create(QSize(320, 200), &error)) << error.toStdString();
    auto *host = new TileHost;
    host->setParentItem(scene.window()->contentItem());
    host->setSize(QSizeF(320, 200));
    const auto data = minutes(150, false);
    const int64_t tf = minute;
    const int64_t firstTile = tiles::tileOfBucket(epoch / tf);
    auto makeTiles = [&](int64_t tickUnits, uint64_t idBase) {
        std::vector<TileRef> refs;
        for (int64_t t = firstTile; t < firstTile + 3; ++t) {
            ComposeOptions clip;
            clip.startMs = tiles::tileStartMs(t, tf);
            clip.endMs = tiles::tileEndMs(t, tf);
            const SparseColumns *level = &data;
            const auto composed = compose(std::span<const SparseColumns *const>(&level, 1), tf, clip);
            auto extent = tiles::binsOf(tiles::usefulUnits(composed, 100), tickUnits);
            if (extent.empty()) extent = {0, 1};
            const auto grid = *tiles::tileGrid(t, tf, tickUnits, 100, extent, 0);
            TileRef ref;
            ref.id = idBase + uint64_t(t - firstTile);
            ref.grid = toBinGrid(grid);
            ref.cells = std::make_shared<const std::vector<uint32_t>>(tiles::buildCells(composed, grid, {}));
            refs.push_back(std::move(ref));
        }
        return refs;
    };
    auto slotsFor = [&](const std::vector<TileRef> &refs) {
        std::vector<TileSlot> out;
        for (const auto &ref : refs) out.push_back({ref.grid.firstBucket, ref.grid.firstBucket + ref.grid.columns, ref.id, 0, true});
        return out;
    };
    const auto ten = makeTiles(1000, 100);
    host->frame.tiles = ten;
    host->frame.visible = slotsFor(ten);
    host->frame.key = 10;
    host->frame.tfMs = tf;
    host->frame.view = {double(epoch), double(epoch + 120 * minute), 99'800, 100'200};
    host->frame.uploadBudgetBytes = 1; // one tile per frame
    for (int i = 0; i < 4; ++i) ASSERT_FALSE(scene.renderFrame(&error).isNull());
    EXPECT_TRUE(host->stats->complete.load());
    EXPECT_EQ(host->stats->tilesUploaded.load(), 3u);
    const QImage before = scene.renderFrame(&error);
    // Pan (time and price): no upload, no bin, the picture moves.
    host->frame.view = {double(epoch + 7 * minute), double(epoch + 127 * minute), 99'790, 100'190};
    host->update();
    const QImage panned = scene.renderFrame(&error);
    EXPECT_EQ(host->stats->tilesUploaded.load(), 3u);
    EXPECT_EQ(host->stats->tilesBinned.load(), 0u);
    EXPECT_TRUE(host->stats->complete.load());
    EXPECT_NE(before, panned);
    // A new tick whose tiles become resident one per frame: the old picture is
    // held (not blanked, not mixed) until every visible tile is ready.
    const auto twenty = makeTiles(2000, 200);
    host->frame.tiles = ten;
    host->frame.tiles.insert(host->frame.tiles.end(), twenty.begin(), twenty.end());
    host->frame.visible = slotsFor(twenty);
    host->frame.key = 20;
    host->update();
    ASSERT_FALSE(scene.renderFrame(&error).isNull());
    EXPECT_TRUE(host->stats->holding.load());
    EXPECT_FALSE(host->stats->complete.load());
    const auto drawnWhileHolding = host->stats->drawnIds();
    for (const auto id : drawnWhileHolding) EXPECT_LT(id, 200u) << "only the old tick is drawn while holding";
    for (int i = 0; i < 4; ++i) ASSERT_FALSE(scene.renderFrame(&error).isNull());
    EXPECT_FALSE(host->stats->holding.load());
    EXPECT_TRUE(host->stats->complete.load());
    EXPECT_EQ(host->stats->drawnKey.load(), 20u);
    // Unprepared available time draws the loading hatch; dropped tiles are freed.
    host->frame.tiles = twenty;
    host->frame.visible = slotsFor(twenty);
    host->frame.visible.push_back({twenty.back().grid.firstBucket + 64, twenty.back().grid.firstBucket + 128, 0, 0, true});
    host->frame.view = {double(epoch), double(epoch + 250 * minute), 99'800, 100'200};
    host->update();
    ASSERT_FALSE(scene.renderFrame(&error).isNull());
    EXPECT_EQ(host->stats->loadingSlots.load(), 1u);
    EXPECT_FALSE(host->stats->complete.load());
    EXPECT_EQ(host->stats->residentTiles.load(), 3u);
    delete host;
    ASSERT_FALSE(scene.renderFrame(&error).isNull());
}
TEST(HeatmapTileNodeScene, TileEdgesOnPixelCentresLeaveNoSeam) {
    if (!lab::metalDeviceAvailable()) GTEST_SKIP() << "No MTLDevice";
    // The geometry that showed 1-px transparent seams on real data: 1500 px over
    // 720 one-minute columns, a tile edge 114 columns in, i.e. at x = 237.5.
    lab::OffscreenQuick scene;
    QString error;
    ASSERT_TRUE(scene.create(QSize(1500, 60), &error)) << error.toStdString();
    auto *host = new TileHost;
    host->setParentItem(scene.window()->contentItem());
    host->setSize(QSizeF(1500, 60));
    const auto data = minutes(150, false);
    const int64_t tf = minute, first = tiles::tileOfBucket(epoch / tf);
    for (int64_t t = first; t < first + 13; ++t) {
        ComposeOptions clip;
        clip.startMs = tiles::tileStartMs(t, tf);
        clip.endMs = tiles::tileEndMs(t, tf);
        const SparseColumns *level = &data;
        const auto composed = compose(std::span<const SparseColumns *const>(&level, 1), tf, clip);
        const auto grid = *tiles::tileGrid(t, tf, 1000, 100, {10'000, 10'040}, 0);
        TileRef ref;
        ref.id = uint64_t(t - first + 1);
        ref.grid = toBinGrid(grid);
        ref.cells = std::make_shared<const std::vector<uint32_t>>(tiles::buildCells(composed, grid, {}));
        host->frame.tiles.push_back(ref);
        host->frame.visible.push_back({ref.grid.firstBucket, ref.grid.firstBucket + 64, ref.id, 0, true});
    }
    const double start = double(tiles::tileFirstBucket(first + 2) - 114);
    // Above the book: every column is veiled (present, gap) or loading: opaque.
    host->frame.view = {start * double(tf), (start + 720) * double(tf), 101'000, 101'400};
    host->frame.key = 1;
    host->frame.tfMs = tf;
    QImage image;
    for (int i = 0; i < 3; ++i) image = scene.renderFrame(&error);
    ASSERT_FALSE(image.isNull());
    ASSERT_TRUE(host->stats->complete.load());
    image = image.convertToFormat(QImage::Format_RGBA8888);
    const QRgb background = scene.window()->color().rgb();
    int seams = 0;
    for (int x = 0; x < image.width(); ++x) seams += image.pixelColor(x, 30).rgb() == background;
    EXPECT_EQ(seams, 0) << "transparent pixel columns between tiles; drawn=" << host->stats->drawnPrimary.load()
                        << " loading=" << host->stats->loadingSlots.load() << " pixel0=" << std::hex
                        << image.pixelColor(700, 30).rgba() << " bg=" << background;
    delete host;
}
} // namespace

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
