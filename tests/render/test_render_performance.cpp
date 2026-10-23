#include "chart/core/ChartFrame.hpp"
#include "chart/layers/TradeLayers.hpp"
#include "render/CandlestickBatched.hpp"
#include "render/ChartTextRenderer.hpp"
#include "render/VolumeProfileRenderer.hpp"
#include <QFile>
#include <QGuiApplication>
#include <QSGGeometryNode>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <gtest/gtest.h>
#include <limits>
#include <memory>
#include <new>

namespace {
thread_local bool countAllocations = false;
thread_local size_t allocations = 0;
}
void* operator new(std::size_t size) {
    if (countAllocations) ++allocations;
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }

struct RenderPerformanceTestAccess {
    static std::array<double, 2> priceRange(const CandlestickBatched& item) {
        return {item.m_priceMin, item.m_priceMax};
    }
    template<class Renderer>
    static std::array<quint64, 2> stats(const Renderer& renderer) {
        return {renderer.m_geometryRebuilds, renderer.m_geometryAllocations};
    }
};

namespace {
class Candles : public CandlestickBatched {
public:
    QSGNode* paint(QSGNode* node) { return updatePaintNode(node, nullptr); }
};

void seed(Candles& item) {
    item.setSize(QSizeF(128, 240));
    item.addCandle(1, 100, 108, 96, 105, 30);
    item.addCandle(2, 106, 110, 98, 101, 20);
    item.addCandle(3, 102, 107, 99, 102, 10);
    item.setSecSignalOverrides({QVariantMap{{"index", 1}, {"signalType", 2}}});
}

void expectGeometry(const std::string& name, QSGNode* root) {
    // Raw little-endian vertex/index bytes captured from main 33081c31.
    // These fixtures are independent of the renderer's allocation and caching.
    QFile file(QStringLiteral(SENTINEL_TEST_SOURCE_DIR "/tests/render/fixtures/render_geometry_33081c31.txt"));
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    const auto lines = file.readAll().split('\n');
    int i = 0;
    for (auto* child = root->firstChild(); child; child = child->nextSibling(), ++i) {
        auto* g = static_cast<QSGGeometryNode*>(child)->geometry();
        const QByteArray vertices(static_cast<const char*>(g->vertexData()), g->vertexCount() * g->sizeOfVertex());
        const QByteArray indices(static_cast<const char*>(g->indexData()), g->indexCount() * g->sizeOfIndex());
        const auto key = QByteArray::fromStdString(name) + "/" + QByteArray::number(i);
        const auto found = std::find_if(lines.begin(), lines.end(), [&](const auto& line) {
            return line.startsWith(key + " ");
        });
        ASSERT_NE(found, lines.end()) << key.constData();
        const auto fields = found->split(' ');
        ASSERT_EQ(fields.size(), 3);
        EXPECT_EQ(vertices, QByteArray::fromHex(fields[1])) << key.constData();
        EXPECT_EQ(indices, QByteArray::fromHex(fields[2])) << key.constData();
        EXPECT_EQ(g->drawingMode(), QSGGeometry::DrawTriangles);
    }
    EXPECT_EQ(i, 3);
}

TEST(RenderPerformance, CandlePriceRangesSkipNonFiniteCandles) {
    for (double invalid : {std::numeric_limits<double>::quiet_NaN(),
                           std::numeric_limits<double>::infinity()}) {
        for (bool invalidFirst : {true, false}) {
            Candles item;
            item.setSize(QSizeF(128, 240));
            const auto addInvalid = [&] { item.addCandle(1, invalid, 1000, -1000, 100, 10); };
            if (invalidFirst) addInvalid();
            item.addCandle(2, 100, 108, 96, 105, 30);
            item.addCandle(3, 106, 110, 98, 101, 20);
            if (!invalidFirst) addInvalid();
            EXPECT_DOUBLE_EQ(item.visiblePriceMin(), 95.3);
            EXPECT_DOUBLE_EQ(item.visiblePriceMax(), 110.7);
            const auto range = RenderPerformanceTestAccess::priceRange(item);
            EXPECT_DOUBLE_EQ(range[0], 95.3);
            EXPECT_DOUBLE_EQ(range[1], 110.7);
        }
    }
}

TEST(RenderPerformance, AllInvalidCandlesUseEmptyPriceRange) {
    Candles item;
    item.setSize(QSizeF(128, 240));
    item.addCandle(1, 100, std::numeric_limits<double>::quiet_NaN(), 90, 100, 10);
    EXPECT_DOUBLE_EQ(item.visiblePriceMin(), 0);
    EXPECT_DOUBLE_EQ(item.visiblePriceMax(), 1);
    EXPECT_EQ(RenderPerformanceTestAccess::priceRange(item), (std::array<double, 2>{0, 1}));
}

TEST(RenderPerformance, CandleVertexBytesMatchBase) {
    Candles item;
    seed(item);
    std::unique_ptr<QSGNode> root;
    for (int style : {0, 1, 2}) {
        item.setCandleStyle(style);
        root.reset(item.paint(root.release()));
        expectGeometry("candles" + std::to_string(style), root.get());
    }
    item.setViewOffset(-8.25f);
    item.setZoomScale(1.3f);
    item.setHeight(281);
    root.reset(item.paint(root.release()));
    expectGeometry("candlesMapped", root.get());
}

TEST(RenderPerformance, VolumeProfileVertexBytesMatchBase) {
    VolumeProfileRenderer renderer;
    QSGNode root;
    VolumeProfileState::Snapshot snap;
    snap.minPrice = 100;
    snap.tickSize = 5;
    snap.gridHeight = 4;
    const std::vector<float> bins{1, 4, 2, 3};
    snap.va = VolumeProfileState::computeValueArea(bins, 100, 5);
    for (bool snapY : {true, false}) {
        renderer.render(&root, true, QRectF(20, 30, 800, 400), 101.3, 118.1, bins, snap, 2.0, snapY);
        expectGeometry(snapY ? "vpSnapped" : "vpFree", &root);
    }
    renderer.setBarColor(QColor(25, 81, 195, 111));
    renderer.setVaColor(QColor(80, 90, 100, 120));
    renderer.setPocColor(QColor(130, 140, 150, 160));
    renderer.setWidthFraction(.23f);
    renderer.render(&root, true, QRectF(20, 30, 800, 400), 110, 120, bins, snap, 1.0, true);
    expectGeometry("vpStyledClipped", &root);
}
TEST(RenderPerformance, CandlesUnchangedFramesDoNotAllocateOrRebuild) {
    Candles item;
    seed(item);
    std::unique_ptr<QSGNode> root(item.paint(nullptr));
    const auto initial = RenderPerformanceTestAccess::stats(item);
    const auto* vol = static_cast<QSGGeometryNode*>(root->firstChild())->geometry()->vertexData();
    allocations = 0;
    countAllocations = true;
    for (int i = 0; i < 100; ++i) item.paint(root.get());
    countAllocations = false;
    EXPECT_EQ(allocations, 0u);
    EXPECT_EQ(RenderPerformanceTestAccess::stats(item), initial);

    const auto change = [&](auto edit) {
        const auto before = RenderPerformanceTestAccess::stats(item);
        edit();
        allocations = 0;
        countAllocations = true;
        item.paint(root.get());
        item.paint(root.get());
        countAllocations = false;
        EXPECT_EQ(allocations, 0u);
        EXPECT_EQ(RenderPerformanceTestAccess::stats(item)[0], before[0] + 1);
        EXPECT_EQ(RenderPerformanceTestAccess::stats(item)[1], before[1]);
    };
    change([&] { item.setViewOffset(-9); });
    change([&] { item.setZoomScale(1.2f); });
    change([&] { item.setHeight(300); });
    change([&] { item.setWidth(140); });
    change([&] { item.setBullishColor(Qt::blue); });
    change([&] { item.setVolumeBarAlpha(91); });
    change([&] { item.clearCandles(); });
    change([&] { seed(item); });
    EXPECT_EQ(static_cast<QSGGeometryNode*>(root->firstChild())->geometry()->vertexData(), vol);

    const auto before = RenderPerformanceTestAccess::stats(item);
    item.setCandleStyle(1); // Grow the body buffer for hollow candles only.
    item.paint(root.get());
    EXPECT_EQ(RenderPerformanceTestAccess::stats(item)[0], before[0] + 1);
    EXPECT_EQ(RenderPerformanceTestAccess::stats(item)[1], before[1] + 1);
    change([&] { item.setCandleStyle(2); });
    change([&] { item.setCandleStyle(0); });
    change([&] { item.setCandleStyle(1); });
}

TEST(RenderPerformance, CandlesKeepSignalOrderAndRebuildAfterRootLoss) {
    Candles item;
    seed(item);
    std::string signalOrder;
    QObject::connect(&item, &CandlestickBatched::visibleRangeChanged, [&] { signalOrder += 'v'; });
    QObject::connect(&item, &CandlestickBatched::viewOffsetChanged, [&] { signalOrder += 'o'; });
    QObject::connect(&item, &CandlestickBatched::renderTimeChanged, [&] { signalOrder += 'r'; });
    std::unique_ptr<QSGNode> root(item.paint(nullptr));
    item.paint(root.get());
    item.setViewOffset(-4);
    item.paint(root.get());
    EXPECT_EQ(signalOrder, "rrvor");
    root.reset();
    const auto before = RenderPerformanceTestAccess::stats(item);
    root.reset(item.paint(nullptr));
    EXPECT_EQ(RenderPerformanceTestAccess::stats(item)[0], before[0] + 1);
    EXPECT_EQ(root->childCount(), 3);
}

TEST(RenderPerformance, VolumeProfileUnchangedFramesDoNotAllocateOrRebuild) {
    VolumeProfileRenderer renderer;
    auto root = std::make_unique<QSGNode>();
    VolumeProfileState::Snapshot snap;
    snap.minPrice = 100;
    snap.tickSize = 5;
    snap.gridHeight = 4;
    std::vector<float> bins{1, 4, 2, 3};
    snap.va = VolumeProfileState::computeValueArea(bins, 100, 5);
    QRectF surface(20, 30, 800, 400);
    double minPrice = 100, maxPrice = 120, dpr = 1;
    bool snapY = true, draw = true;
    const auto paint = [&] { renderer.render(root.get(), draw, surface, minPrice, maxPrice, bins, snap, dpr, snapY); };
    paint();
    const auto initial = RenderPerformanceTestAccess::stats(renderer);
    allocations = 0;
    countAllocations = true;
    for (int i = 0; i < 100; ++i) paint();
    countAllocations = false;
    EXPECT_EQ(allocations, 0u);
    EXPECT_EQ(RenderPerformanceTestAccess::stats(renderer), initial);
    const auto change = [&](auto edit) {
        const auto before = RenderPerformanceTestAccess::stats(renderer);
        edit();
        allocations = 0;
        countAllocations = true;
        paint();
        paint();
        countAllocations = false;
        EXPECT_EQ(allocations, 0u);
        EXPECT_EQ(RenderPerformanceTestAccess::stats(renderer)[0], before[0] + 1);
        EXPECT_EQ(RenderPerformanceTestAccess::stats(renderer)[1], before[1]);
    };
    change([&] { bins[0] = 2; ++snap.generation; });
    change([&] { minPrice = 101; });
    change([&] { maxPrice = 121; });
    change([&] { surface.translate(.25, .5); });
    change([&] { surface.setSize(QSizeF(900, 450)); });
    change([&] { dpr = 2; });
    change([&] { snapY = false; });
    change([&] { snap.minPrice = 101; });
    change([&] { snap.tickSize = 4; });
    change([&] { snap.va.valid = false; });
    change([&] { snap.va.valid = true; snap.va.pocPrice += .25; });
    change([&] { snap.va.valPrice += .25; });
    change([&] { snap.va.vahPrice -= .25; });
    change([&] { renderer.setWidthFraction(.2f); });
    change([&] { renderer.setOverlay(true); });
    change([&] { renderer.setBarColor(Qt::blue); });
    change([&] { renderer.setVaColor(Qt::green); });
    change([&] { renderer.setPocColor(Qt::red); });
    const auto beforeNoOp = RenderPerformanceTestAccess::stats(renderer);
    renderer.setBarColor(Qt::blue);
    renderer.setVaColor(Qt::green);
    renderer.setPocColor(Qt::red);
    renderer.setWidthFraction(.2f);
    renderer.setOverlay(true);
    paint();
    EXPECT_EQ(RenderPerformanceTestAccess::stats(renderer), beforeNoOp);
    draw = false;
    paint();
    EXPECT_EQ(static_cast<QSGGeometryNode*>(root->firstChild()->nextSibling())->geometry()->vertexCount(), 0);
    change([&] { draw = true; });
    root.reset();
    renderer.onRootRebuilt();
    root = std::make_unique<QSGNode>();
    paint();
    EXPECT_EQ(root->childCount(), 3);
    EXPECT_EQ(RenderPerformanceTestAccess::stats(renderer)[0], beforeNoOp[0] + 2);
}

TEST(RenderPerformance, VolumeProfileMailboxDeliversEachUpdateAndClearOnceWithoutCopy) {
    VolumeProfileRenderer renderer;
    VolumeProfileState::Snapshot snap;
    snap.gridHeight = 4;
    snap.tickSize = 1;
    std::vector<float> pending{1, 2, 3, 4}, bins;
    const auto* storage = pending.data();
    renderer.enqueue(std::move(pending), snap);
    ASSERT_TRUE(renderer.drainPending(bins, snap));
    EXPECT_EQ(bins.data(), storage);
    QSGNode root;
    const auto paint = [&] { renderer.render(&root, true, QRectF(0, 0, 400, 200), 0, 4, bins, snap, 1, true); };
    paint();
    const auto initial = RenderPerformanceTestAccess::stats(renderer);
    bool delivered = false;
    allocations = 0;
    countAllocations = true;
    for (int i = 0; i < 100; ++i) {
        delivered |= renderer.drainPending(bins, snap);
        paint();
    }
    countAllocations = false;
    EXPECT_FALSE(delivered);
    EXPECT_EQ(allocations, 0u);
    EXPECT_EQ(bins.data(), storage);
    EXPECT_EQ(RenderPerformanceTestAccess::stats(renderer), initial);
    // A replacement source may reuse the old generation number.
    renderer.enqueue({4, 3, 2, 1}, snap);
    EXPECT_TRUE(renderer.drainPending(bins, snap));
    paint();
    paint();
    EXPECT_EQ(RenderPerformanceTestAccess::stats(renderer)[0], initial[0] + 1);
    renderer.clearPending();
    ASSERT_TRUE(renderer.drainPending(bins, snap));
    EXPECT_TRUE(bins.empty());
    EXPECT_FALSE(renderer.drainPending(bins, snap));
    paint();
    EXPECT_EQ(static_cast<QSGGeometryNode*>(root.firstChild()->nextSibling())->geometry()->vertexCount(), 0);
}

TEST(RenderPerformance, VolumeProfileLayerRetainsDataAndHonorsClearAndRootLoss) {
    chart::VolumeProfileLayer layer;
    ChartTextRenderer text;
    ChartTextAtlas atlas;
    auto root = std::make_unique<QSGNode>();
    chart::LayerNodeContext context{nullptr, root.get(), text, atlas, false};
    chart::ChartFrame frame;
    frame.context.surfaceBounds = QRectF(0, 0, 400, 200);
    frame.context.mapping.viewMinPrice = 0;
    frame.context.mapping.viewMaxPrice = 4;
    frame.context.overlays.volumeProfile = true;
    frame.context.surfaceDpr = 1;
    VolumeProfileState::Snapshot snap;
    snap.gridHeight = 4;
    snap.tickSize = 1;
    layer.renderer.enqueue({1, 2, 3, 4}, snap);
    layer.updateNodes(frame, context);
    const auto initial = RenderPerformanceTestAccess::stats(layer.renderer);
    allocations = 0;
    countAllocations = true;
    for (int i = 0; i < 100; ++i) layer.updateNodes(frame, context);
    countAllocations = false;
    EXPECT_EQ(allocations, 0u);
    EXPECT_EQ(RenderPerformanceTestAccess::stats(layer.renderer), initial);
    root.reset();
    layer.onRootRebuilt();
    root = std::make_unique<QSGNode>();
    context.root = root.get();
    layer.updateNodes(frame, context);
    EXPECT_EQ(root->childCount(), 3);
    EXPECT_EQ(RenderPerformanceTestAccess::stats(layer.renderer)[0], initial[0] + 1);
    layer.renderer.clearPending();
    layer.updateNodes(frame, context);
    EXPECT_EQ(static_cast<QSGGeometryNode*>(root->firstChild()->nextSibling())->geometry()->vertexCount(), 0);
}

TEST(RenderPerformance, VolumeProfileOnlyAllocatesOnGrowth) {
    VolumeProfileRenderer renderer;
    QSGNode root;
    VolumeProfileState::Snapshot snap;
    snap.tickSize = 1;
    snap.gridHeight = 4;
    std::vector<float> bins(4, 1);
    const auto paint = [&] {
        renderer.render(&root, true, QRectF(0, 0, 400, 200), 0, 5000, bins, snap, 1, true);
    };
    paint();
    const auto initial = RenderPerformanceTestAccess::stats(renderer);
    bins.resize(4097, 1);
    snap.gridHeight = 4097;
    ++snap.generation;
    paint();
    paint();
    const auto grown = RenderPerformanceTestAccess::stats(renderer);
    EXPECT_EQ(grown[0], initial[0] + 1);
    EXPECT_EQ(grown[1], initial[1] + 1);
    auto* geometry = static_cast<QSGGeometryNode*>(root.firstChild()->nextSibling())->geometry();
    const auto* storage = geometry->vertexData();
    for (int size : {2, 0, 4097}) {
        bins.resize(size, 1);
        snap.gridHeight = size;
        ++snap.generation;
        paint();
        EXPECT_EQ(RenderPerformanceTestAccess::stats(renderer)[1], grown[1]);
        EXPECT_EQ(geometry->vertexData(), storage);
    }
}

void expectFiniteVertices(QSGNode* root) {
    for (auto* child = root->firstChild(); child; child = child->nextSibling()) {
        auto* geometry = static_cast<QSGGeometryNode*>(child)->geometry();
        const auto* vertices = geometry->vertexDataAsColoredPoint2D();
        for (int i = 0; i < geometry->vertexCount(); ++i) {
            EXPECT_TRUE(std::isfinite(vertices[i].x));
            EXPECT_TRUE(std::isfinite(vertices[i].y));
        }
    }
}

TEST(RenderPerformance, InvalidRecordsNeverReachVertexBuffers) {
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    const auto inf = std::numeric_limits<double>::infinity();
    Candles item;
    seed(item);
    item.addCandle(4, nan, 110, 90, 100, 10);
    item.addCandle(5, 100, 110, 90, 101, inf);
    std::unique_ptr<QSGNode> candles(item.paint(nullptr));
    expectFiniteVertices(candles.get());
    for (int style : {1, 2}) {
        item.setCandleStyle(style);
        item.paint(candles.get());
        expectFiniteVertices(candles.get());
    }

    VolumeProfileRenderer renderer;
    QSGNode root;
    VolumeProfileState::Snapshot snap;
    snap.tickSize = 1;
    snap.gridHeight = 4;
    snap.va.valid = true;
    snap.va.valPrice = nan;
    snap.va.vahPrice = inf;
    snap.va.pocPrice = inf;
    renderer.render(&root, true, QRectF(0, 0, 400, 200), 0, 4,
                    {1, float(nan), float(inf), 2}, snap, 1, true);
    expectFiniteVertices(&root);
    const auto* bars = static_cast<QSGGeometryNode*>(root.firstChild()->nextSibling())->geometry();
    EXPECT_EQ(bars->vertexCount(), 8);
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
