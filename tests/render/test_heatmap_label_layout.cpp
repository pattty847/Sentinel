// Slice S7b: liquidity label layout on the GPU chart (HeatmapLabelLayout) and the
// label text format. Pure CPU (the MSDF atlas is built from the bundled font);
// plan section 6 item 5: no capacity growth over 120 frames, no glyph outside
// drawRect, device-pixel snapping at 1x/1.5x/2x, the label gate and the budget.
#include "render/ChartTextAtlas.hpp"
#include "render/heatmap/HeatmapLabelLayout.hpp"
#include <QGuiApplication>
#include <gtest/gtest.h>
#include <cmath>
#include <map>
#include <random>
#include <set>

namespace {
using namespace heatmap;
using namespace heatmap::gpu;
constexpr int64_t minute = 60'000;

const ChartTextAtlas &atlas() {
    static ChartTextAtlas a = [] {
        ChartTextAtlas out;
        ChartTextAtlas::BuildParams p; // as UnifiedGridRenderer::buildMsdfAtlas
        p.fontFamily = "Roboto Mono";
        p.fontPx = 64;
        p.pxRange = 4.0f;
        p.charset = QStringLiteral(" 0123456789.+-,:/$%kMBABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz");
        p.resourceFont = QStringLiteral(":/fonts/RobotoMono/RobotoMono-Regular.ttf");
        out.build(p);
        return out;
    }();
    return a;
}

std::string text(double value, bool usd) {
    std::array<char, 48> out{};
    formatLabelAmount(out, value, usd, "BTC");
    return out.data();
}

// A window of `columns` x `rows` cells at tf 1m / tick $1 starting at bucket 1000,
// bin 100000 (bottom); every cell valid with code `code` unless `word` overrides.
std::shared_ptr<LabelCells> window(uint32_t columns, uint32_t rows, uint16_t code = 20000,
                                   std::function<uint32_t(uint32_t x, uint32_t y)> word = {}) {
    auto out = std::make_shared<LabelCells>();
    out->key.serial = 1;
    out->key.tfMs = minute;
    out->key.tickUnits = 100;
    out->key.firstBucket = 1000;
    out->key.firstBin = 100'000;
    out->key.columns = columns;
    out->key.rows = rows;
    out->grid = {minute, 0, 1000, columns, 100, 1.0, 100, 100'000, rows};
    out->cells.resize(size_t(columns) * rows);
    for (uint32_t y = 0; y < rows; ++y)
        for (uint32_t x = 0; x < columns; ++x) {
            auto &cell = out->cells[size_t(y) * columns + x];
            cell.word = word ? word(x, y) : tiles::cellWord(tiles::kCellValid, code);
            cell.value = 0.5 + 0.37 * double(x + 3 * y);
            formatLabelAmount(cell.usd, cell.value * 100'000, true, "BTC");
            formatLabelAmount(cell.asset, cell.value, false, "BTC");
        }
    return out;
}

// The viewport-only mapping of the gpu chart (UnifiedGridRenderer::computeGpuFrameMapping).
TimeAxisMapping mapping(QRectF rect, double t0, double t1, double p0, double p1, double tf = minute, double tick = 1) {
    TimeAxisMapping m;
    m.viewStartMs = t0;
    m.viewEndMs = t1;
    m.viewMinPrice = p0;
    m.viewMaxPrice = p1;
    m.dataStartMs = std::floor(t0 / tf) * tf;
    m.dataEndMs = std::ceil(t1 / tf) * tf;
    m.dataMinPrice = p0;
    m.dataMaxPrice = p1;
    m.appendMs = tf;
    m.tickSize = tick;
    m.drawRect = rect;
    m.srcRect = QRectF((t0 - m.dataStartMs) / tf, 0.0, (t1 - t0) / tf, (p1 - p0) / tick);
    m.valid = true;
    return m;
}

// Cells (1000..1000+cols) x (100000..100000+rows) exactly filling rect.
TimeAxisMapping fit(QRectF rect, uint32_t cols, uint32_t rows) {
    return mapping(rect, 1000.0 * minute, (1000.0 + cols) * minute, 100'000, 100'000.0 + rows);
}

LabelStyle style(float floor = 6000) {
    LabelStyle s;
    s.window = {floor, 24000};
    return s;
}

// Label boxes (debugAnchor) of a frame: anchor -> glyph count.
std::map<std::pair<double, double>, int> labels(const std::vector<ChartGlyphInstance> &glyphs) {
    std::map<std::pair<double, double>, int> out;
    for (const auto &g : glyphs) ++out[{g.debugAnchor.x(), g.debugAnchor.y()}];
    return out;
}

TEST(HeatmapLabelFormat, ThreeSignificantDigitsWithTrailingZerosKept) {
    EXPECT_EQ(text(1'240'000, true), "$1.24M");
    EXPECT_EQ(text(847'000, true), "$847k");
    EXPECT_EQ(text(11'000, true), "$11.0k");
    EXPECT_EQ(text(1'000'000, true), "$1.00M");
    EXPECT_EQ(text(999.6, true), "$1.00k");
    EXPECT_EQ(text(9.996, true), "$10.0");
    EXPECT_EQ(text(99.96, true), "$100");
    EXPECT_EQ(text(12.5, false), "12.5 BTC");
    EXPECT_EQ(text(0.000123, false), "0.000123 BTC");
    EXPECT_EQ(text(2.5e12, true), "$2.50T");
    EXPECT_EQ(text(0, true), "");
    EXPECT_EQ(text(std::nan(""), true), "");
}

class LabelLayout : public testing::Test {
protected:
    void SetUp() override {
        if (!atlas().isBuilt()) GTEST_SKIP() << "MSDF atlas unavailable";
        glyphs.reserve(HeatmapLabelLayout::kMaxGlyphs);
    }
    HeatmapLabelLayout layout;
    std::vector<ChartGlyphInstance> glyphs;
};

// The label gate (owner decision 2): only valid cells the palette colours
// (code - codeFloor > 0, exactly the shader's test) get a label; veil, loading,
// no data and cells at or below the low handle get none.
TEST_F(LabelLayout, OnlyColouredCellsGetLabels) {
    const float floor = 6000;
    auto cells = window(8, 5, 0, [&](uint32_t x, uint32_t y) -> uint32_t {
        switch ((x + y * 8) % 6) {
        case 0: return tiles::cellWord(tiles::kCellValid, 9000);      // coloured
        case 1: return tiles::cellWord(tiles::kCellValid, 6001, true); // coloured (ask), just above
        case 2: return tiles::cellWord(tiles::kCellValid, 6000);      // at the low handle: no colour
        case 3: return tiles::cellWord(tiles::kCellVeil);
        case 4: return tiles::cellWord(tiles::kCellLoading);
        default: return tiles::cellWord(tiles::kCellNoData);
        }
    });
    const QRectF rect(0, 0, 8 * 80, 5 * 40);
    layout.layout(cells, atlas(), fit(rect, 8, 5), 1.0, style(floor), glyphs);
    size_t expected = 0;
    for (uint32_t i = 0; i < 40; ++i) expected += i % 6 < 2;
    EXPECT_EQ(layout.stats().labels, expected);
    EXPECT_EQ(labels(glyphs).size(), expected);
    // Each label sits inside the cell it belongs to.
    for (const auto &[anchor, n] : labels(glyphs)) {
        const int x = int(anchor.first / 80), y = int(anchor.second / 40);
        EXPECT_LT((x + y * 8) % 6, 2) << "label in cell " << x << "," << y;
    }
    // Raising the low handle hides labels with the colour.
    layout.layout(cells, atlas(), fit(rect, 8, 5), 1.0, style(9000), glyphs);
    EXPECT_EQ(layout.stats().labels, 0u);
}

// Owner decision 1: the text fits at 12 px plus padding or the cell has no label;
// it grows with the cells up to 15 px and never beyond.
TEST_F(LabelLayout, SizeGrowsFrom12To15AndNeverShrinksBelow12) {
    auto cells = window(6, 4);
    struct Case {
        double cellW, cellH, minSize, maxSize;
        bool any;
    };
    for (const Case c : {Case{120, 10, 0, 0, false},     // shorter than 12 px + padding: none
                         Case{120, 14, 12, 12, true},    // exactly fits 12 px
                         Case{120, 15.5, 13.5, 13.5, true},
                         Case{200, 80, 15, 15, true},    // large cells: capped at 15
                         Case{42, 80, 12, 12, true}}) {  // narrow: 12 px, only the labels that fit
        SCOPED_TRACE(std::to_string(c.cellW) + "x" + std::to_string(c.cellH));
        const QRectF rect(0, 0, 6 * c.cellW, 4 * c.cellH);
        layout.layout(cells, atlas(), fit(rect, 6, 4), 1.0, style(), glyphs);
        EXPECT_EQ(layout.stats().labels > 0, c.any);
        if (c.any) {
            EXPECT_GE(layout.stats().sizePx, c.minSize - 1e-9);
            EXPECT_LE(layout.stats().sizePx, c.maxSize + 1e-9);
        }
        // Text height: the glyph quads never exceed the em box plus the atlas padding.
        for (const auto &g : glyphs) EXPECT_LE(g.rect.height(), layout.stats().sizePx * 1.6);
    }
    // Narrow cells: every drawn label's ink fits its cell; the rest are counted.
    const QRectF narrow(0, 0, 6 * 42, 4 * 80);
    layout.layout(cells, atlas(), fit(narrow, 6, 4), 1.0, style(), glyphs);
    EXPECT_GT(layout.stats().tooNarrow, 0u);
    for (const auto &[anchor, n] : labels(glyphs)) {
        double right = 0;
        for (const auto &g : glyphs)
            if (g.debugAnchor == QPointF(anchor.first, anchor.second)) right = std::max(right, g.rect.right());
        const double cellLeft = std::floor(anchor.first / 42) * 42;
        EXPECT_LE(right, cellLeft + 42 + 0.5) << "ink plus the atlas padding stays in the cell";
    }
}

// No glyph outside the mapping's drawRect, over random views that cut cells at
// every edge (sub-cell pans and zooms), with a rect offset from the origin.
TEST_F(LabelLayout, NoGlyphOutsideTheDrawRect) {
    auto cells = window(40, 30);
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> u(0, 1);
    const QRectF rect(13.5, 7.25, 900, 500);
    size_t drawn = 0;
    for (int i = 0; i < 200; ++i) {
        const double cols = 6 + 20 * u(rng), rows = 8 + 20 * u(rng);
        const double t0 = (1000 - 3 + 30 * u(rng)) * minute, p0 = 100'000 - 3 + 20 * u(rng);
        const double dpr = i % 3 == 0 ? 1.0 : (i % 3 == 1 ? 1.5 : 2.0);
        layout.layout(cells, atlas(), mapping(rect, t0, t0 + cols * minute, p0, p0 + rows), dpr, style(), glyphs);
        drawn += glyphs.size();
        for (const auto &g : glyphs) {
            ASSERT_GE(g.rect.left(), rect.left() - 1e-9);
            ASSERT_GE(g.rect.top(), rect.top() - 1e-9);
            ASSERT_LE(g.rect.right(), rect.right() + 1e-9);
            ASSERT_LE(g.rect.bottom(), rect.bottom() + 1e-9);
        }
    }
    EXPECT_GT(drawn, 1000u);
}

// Each label's box is snapped to DEVICE pixels (round(x * dpr) / dpr), not to
// logical pixels: at 1.5x a half-logical-pixel origin is a whole device pixel.
TEST_F(LabelLayout, LabelBoxesSnapToDevicePixels) {
    auto cells = window(12, 10);
    for (const double dpr : {1.0, 1.5, 2.0}) {
        SCOPED_TRACE(dpr);
        const QRectF rect(0.3, 0.7, 12 * 61.3, 10 * 23.7); // odd sizes: centres fall between pixels
        layout.layout(cells, atlas(), mapping(rect, 1000.2 * minute, 1012.2 * minute, 100'000.4, 100'010.4), dpr, style(),
                      glyphs);
        ASSERT_GT(layout.stats().labels, 20u);
        size_t halfPixel = 0;
        for (const auto &[anchor, n] : labels(glyphs)) {
            EXPECT_NEAR(anchor.first * dpr, std::round(anchor.first * dpr), 1e-6);
            EXPECT_NEAR(anchor.second * dpr, std::round(anchor.second * dpr), 1e-6);
            halfPixel += std::abs(anchor.first - std::round(anchor.first)) > 0.25 ||
                         std::abs(anchor.second - std::round(anchor.second)) > 0.25;
        }
        if (dpr != 1.0) EXPECT_GT(halfPixel, 0u) << "some origins sit between logical pixels (device snapped)";
    }
}

// The hot path: the glyph runs are built once per LabelCells/currency; 120 frames
// of pans and zooms rebuild nothing and grow no vector.
TEST_F(LabelLayout, NoCapacityGrowthOver120Frames) {
    auto cells = window(100, 160); // the 16,000-cell query limit
    const QRectF rect(0, 0, 1600, 900);
    layout.layout(cells, atlas(), mapping(rect, 1010.0 * minute, 1030.0 * minute, 100'010, 100'050), 2.0, style(),
                  glyphs);
    ASSERT_GT(layout.stats().labels, 0u);
    const size_t internal = layout.capacityBytes(), outCapacity = glyphs.capacity();
    const uint64_t rebuilds = layout.stats().rebuilds;
    EXPECT_EQ(rebuilds, 1u);
    for (int i = 0; i < 120; ++i) {
        const double pan = 0.37 * i, zoom = 1.0 + 0.004 * i;
        layout.layout(cells, atlas(),
                      mapping(rect, (1010.0 + pan) * minute, (1010.0 + pan + 20 * zoom) * minute, 100'010 + pan / 2,
                              100'010 + pan / 2 + 40 * zoom),
                      2.0, style(), glyphs);
        ASSERT_GT(layout.stats().labels, 0u) << "frame " << i;
    }
    EXPECT_EQ(layout.capacityBytes(), internal);
    EXPECT_EQ(glyphs.capacity(), outCapacity);
    EXPECT_EQ(layout.stats().rebuilds, rebuilds) << "no glyph run rebuild while only the view changes";
    // A new result (or currency) rebuilds once.
    auto again = window(100, 160);
    layout.layout(again, atlas(), mapping(rect, 1010.0 * minute, 1050.0 * minute, 100'010, 100'050), 2.0, style(), glyphs);
    EXPECT_EQ(layout.stats().rebuilds, rebuilds + 1);
}

// The glyph budget: at most maxGlyphs glyphs; the rest are counted.
TEST_F(LabelLayout, TheGlyphBudgetIsRespected) {
    auto cells = window(20, 20);
    auto s = style();
    s.maxGlyphs = 60;
    layout.layout(cells, atlas(), fit(QRectF(0, 0, 20 * 90, 20 * 30), 20, 20), 1.0, s, glyphs);
    EXPECT_LE(glyphs.size(), 60u);
    EXPECT_GT(layout.stats().droppedBudget, 0u);
    EXPECT_EQ(layout.stats().labels + layout.stats().droppedBudget, 400u);
}

// USD and asset text come from the query's preformatted buffers; switching
// rebuilds once and draws the other strings.
TEST_F(LabelLayout, CurrencyToggleDrawsTheOtherBuffer) {
    auto cells = window(4, 3);
    const QRectF rect(0, 0, 4 * 160, 3 * 40);
    auto s = style();
    layout.layout(cells, atlas(), fit(rect, 4, 3), 1.0, s, glyphs);
    const size_t usdGlyphs = glyphs.size();
    s.usd = false;
    layout.layout(cells, atlas(), fit(rect, 4, 3), 1.0, s, glyphs);
    EXPECT_EQ(layout.stats().rebuilds, 2u);
    EXPECT_EQ(layout.stats().labels, 12u);
    EXPECT_NE(glyphs.size(), usdGlyphs); // " BTC" vs "$"
}

// Text colour follows the cell's palette colour: dark on bright cells.
TEST_F(LabelLayout, DarkTextOnBrightCells) {
    auto cells = window(2, 1, 0, [](uint32_t x, uint32_t) {
        return tiles::cellWord(tiles::kCellValid, x == 0 ? 6500 : 30000);
    });
    PaletteGradients white;
    white.bid = {{0, 0, 0, 0}, {1, 255, 255, 255}};
    white.ask = white.bid;
    auto s = style();
    s.palette = makePalette(white, {});
    layout.layout(cells, atlas(), fit(QRectF(0, 0, 400, 60), 2, 1), 1.0, s, glyphs);
    ASSERT_EQ(layout.stats().labels, 2u);
    std::set<QRgb> left, right;
    for (const auto &g : glyphs) (g.debugAnchor.x() < 200 ? left : right).insert(g.color.rgba());
    EXPECT_EQ(left, std::set<QRgb>{HeatmapLabelLayout::kLightText});
    EXPECT_EQ(right, std::set<QRgb>{HeatmapLabelLayout::kDarkText});
}
} // namespace

int main(int argc, char **argv) {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    Q_INIT_RESOURCE(sentinel_ui_fonts);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
