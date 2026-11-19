// Slice S7b: liquidity label layout on the GPU chart (HeatmapLabelLayout) and the
// label text format. Pure CPU (the MSDF atlas is built from the bundled font);
// plan section 6 item 5: no capacity growth over 120 frames, no glyph outside
// drawRect, device-pixel snapping at 1x/1.5x/2x, the label gate and the budget.
// W-0001: the chart text rule (all or none per frame, compact before hide, hysteresis,
// baseline with the cell) and one number format across chart layers.
#include "render/ChartTextAtlas.hpp"
#include "render/ChartTextNode.hpp"
#include "render/ChartTextRenderer.hpp"
#include "render/heatmap/HeatmapLabelLayout.hpp"
#include "chart/core/FlowFooter.hpp"
#include "chart/core/FootprintLod.hpp"
#include "chart/layers/CvdLayerLayout.hpp"
#include "chart/layers/LadderBadges.hpp"
#include "config/AgentHostMode.hpp"
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <gtest/gtest.h>
#include <cmath>
#include <cstring>
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

class AtlasCacheTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(home.isValid());
        oldHome = qgetenv("HOME");
        oldCfHome = qgetenv("CFFIXED_USER_HOME");
        oldTestMode = QStandardPaths::isTestModeEnabled();
        qputenv("HOME", home.path().toUtf8());
        // macOS standard directories come from CoreFoundation, independently of HOME.
        qputenv("CFFIXED_USER_HOME", home.path().toUtf8());
        QStandardPaths::setTestModeEnabled(true);
        AgentHostMode::resetForTests();
        standardData = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        // Refuse to run the write checks if Qt did not honor the temporary home.
        ASSERT_TRUE(standardData.startsWith(home.path() + '/')) << qPrintable(standardData);
        ASSERT_TRUE(filesUnder(standardData).isEmpty());
    }

    void TearDown() override {
        AgentHostMode::resetForTests();
        QStandardPaths::setTestModeEnabled(oldTestMode);
        qputenv("HOME", oldHome);
        if (oldCfHome.isNull()) qunsetenv("CFFIXED_USER_HOME");
        else qputenv("CFFIXED_USER_HOME", oldCfHome);
    }

    static QStringList filesUnder(const QString& dir) {
        QStringList files;
        QDirIterator it(dir, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) files << it.next();
        files.sort();
        return files;
    }

    void checkColdCache(bool hosted) {
        const QString session = home.path() + "/session";
        QString error;
        if (hosted) ASSERT_TRUE(AgentHostMode::activate(session, {}, &error)) << qPrintable(error);
        const QString data = hosted ? AgentHostMode::dataDir() : standardData;
        ASSERT_TRUE(filesUnder(data).isEmpty());

        ChartTextAtlas::BuildParams params;
        params.fontFamily = "Roboto Mono";
        params.fontPath = home.path() + "/missing.ttf";
        params.resourceFont = ":/fonts/RobotoMono/RobotoMono-Regular.ttf";
        params.charset = "0123";
        params.fontPx = 32;
        ASSERT_TRUE(QFile::exists(params.resourceFont));
        const QString realFont = home.path() + "/real.ttf";
        ASSERT_TRUE(QFile::copy(params.resourceFont, realFont));

        QStringList messages;
        capturedMessages = &messages;
        const auto previous = qInstallMessageHandler(captureMessage);
        const auto restoreHandler = qScopeGuard([&] {
            qInstallMessageHandler(previous);
            capturedMessages = nullptr;
        });
        ChartTextAtlas embedded;
        ASSERT_TRUE(embedded.build(params));
        const auto fonts = filesUnder(data + "/fonts/msdf");
        ASSERT_EQ(fonts.size(), 1);
        EXPECT_TRUE(fonts.front().endsWith("_RobotoMono-Regular.ttf"));
        QFile extracted(fonts.front());
        QFile resource(params.resourceFont);
        ASSERT_TRUE(extracted.open(QIODevice::ReadOnly));
        ASSERT_TRUE(resource.open(QIODevice::ReadOnly));
        EXPECT_EQ(extracted.readAll(), resource.readAll());

        ChartTextAtlas direct;
        params.fontPath = realFont;
        ASSERT_TRUE(direct.build(params));
        const QString cache = data + "/glyph-cache/msdf";
        const auto cacheFiles = filesUnder(cache);
        ASSERT_EQ(cacheFiles.size(), 4);
        EXPECT_EQ(QDir(cache).entryList({"*.json"}, QDir::Files).size(), 2);
        EXPECT_EQ(QDir(cache).entryList({"*.png"}, QDir::Files).size(), 2);
        for (const auto& path : cacheFiles) EXPECT_GT(QFileInfo(path).size(), 0) << qPrintable(path);
        EXPECT_EQ(messages.filter("MSDF atlas built:").size(), 2);
        EXPECT_TRUE(messages.filter("MSDF atlas loaded from cache:").isEmpty());

        messages.clear();
        ChartTextAtlas directCached;
        ASSERT_TRUE(directCached.build(params));
        params.fontPath = home.path() + "/missing.ttf";
        ChartTextAtlas embeddedCached;
        ASSERT_TRUE(embeddedCached.build(params));
        EXPECT_EQ(messages.filter("MSDF atlas loaded from cache:").size(), 2);
        EXPECT_TRUE(messages.filter("MSDF atlas built:").isEmpty());
        EXPECT_EQ(filesUnder(cache), cacheFiles);
        if (hosted) {
            EXPECT_EQ(data, QFileInfo(session).canonicalFilePath() + "/data");
            EXPECT_TRUE(filesUnder(standardData).isEmpty()) << qPrintable(filesUnder(standardData).join('\n'));
        } else {
            EXPECT_EQ(filesUnder(standardData).size(), 5);
            EXPECT_FALSE(QFileInfo::exists(session));
        }
    }

private:
    static void captureMessage(QtMsgType, const QMessageLogContext&, const QString& message) {
        capturedMessages->append(message);
    }
    inline static QStringList* capturedMessages = nullptr;
    QTemporaryDir home;
    QByteArray oldHome;
    QByteArray oldCfHome;
    bool oldTestMode = false;
    QString standardData;
};

TEST_F(AtlasCacheTest, HostedColdCacheAndEmbeddedFontStayInTheSession) {
    checkColdCache(true);
}

TEST_F(AtlasCacheTest, InactiveColdCacheAndEmbeddedFontUseAppLocalData) {
    checkColdCache(false);
}

std::string text(double value, bool usd, int form = 0) {
    LabelCell cell;
    cell.value = value;
    formatLabelCell(cell, 1.0);
    return usd ? cell.usd[form] : cell.asset[form];
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
            formatLabelCell(cell, 100'000);
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

TEST(HeatmapLabelFormat, AtMostThreeSignificantDigitsTrailingZerosDropped) {
    // Owner 2026-10-02: $11k not $11.0k, $1.2M not $1.20M; asset text is the number only.
    // The chart number format (formatChartNumber): at most three decimals, never zero.
    EXPECT_EQ(text(1'240'000, true), "$1.24M");
    EXPECT_EQ(text(1'200'000, true), "$1.2M");
    EXPECT_EQ(text(847'000, true), "$847k");
    EXPECT_EQ(text(11'000, true), "$11k");
    EXPECT_EQ(text(1'000'000, true), "$1M");
    EXPECT_EQ(text(999.6, true), "$1k");
    EXPECT_EQ(text(9.996, true), "$10");
    EXPECT_EQ(text(99.96, true), "$100");
    EXPECT_EQ(text(1'236, true), "$1.24k");
    EXPECT_EQ(text(12.5, false), "12.5");
    EXPECT_EQ(text(0.358, false), "0.358");
    EXPECT_EQ(text(0.000123, false), "<0.001");
    EXPECT_EQ(text(0.0642, false), "0.064");
    EXPECT_EQ(text(1'240'000, true, 1), "$1.2M") << "the compact form";
    EXPECT_EQ(text(12.5, false, 1), "13");
    EXPECT_EQ(text(2.5e12, true), "$2.5T");
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

TEST_F(LabelLayout, CurrencySymbolToggleChangesOnlyUsdCellGlyphs) {
    auto cells = window(1, 1);
    const auto camera = fit({0, 0, 180, 36}, 1, 1);
    auto appearance = style();
    EXPECT_FALSE(appearance.currencySymbol);
    layout.layout(cells, atlas(), camera, 1, appearance, glyphs);
    ASSERT_EQ(layout.stats().labels, 1u);
    const auto plainCount = glyphs.size();
    ASSERT_GT(plainCount, 0u);
    const auto plainFirst = glyphs.front().uv;
    appearance.currencySymbol = true;
    layout.layout(cells, atlas(), camera, 1, appearance, glyphs);
    EXPECT_EQ(glyphs.size(), plainCount + 1);
    EXPECT_NE(glyphs.front().uv, plainFirst);
    appearance.usd = false;
    layout.layout(cells, atlas(), camera, 1, appearance, glyphs);
    const auto assetCount = glyphs.size();
    appearance.currencySymbol = false;
    layout.layout(cells, atlas(), camera, 1, appearance, glyphs);
    EXPECT_EQ(glyphs.size(), assetCount);
}

TEST_F(LabelLayout, FootprintCardsSuppressWholeIntersectingLabelsWithoutCapacityGrowth) {
    auto cells = window(2, 1);
    const auto camera = fit({0, 0, 240, 30}, 2, 1);
    auto appearance = style();
    layout.layout(cells, atlas(), camera, 1, appearance, glyphs);
    ASSERT_EQ(layout.stats().labels, 2u);
    const auto capacity = layout.capacityBytes();
    const auto glyphCapacity = glyphs.capacity();
    // Touch just one glyph edge: the entire first label must disappear.
    const auto edge = glyphs.front().rect;
    std::array<QRectF, 1> cards{{{edge.left(), edge.top(), 1, edge.height()}}};
    appearance.occlusion = cards;
    for (int i = 0; i < 120; ++i) {
        layout.layout(cells, atlas(), camera, 1, appearance, glyphs);
        EXPECT_EQ(layout.stats().labels, 1u);
        for (const auto& glyph : glyphs) EXPECT_GT(glyph.rect.center().x(), 120);
        EXPECT_EQ(layout.capacityBytes(), capacity); EXPECT_EQ(glyphs.capacity(), glyphCapacity);
    }
    const auto rebuilds = layout.stats().rebuilds;
    // Same heatmap snapshot, atlas and mapping; only the footprint occupancy pans.
    cards[0].translate(120, 0);
    layout.layout(cells, atlas(), camera, 1, appearance, glyphs);
    EXPECT_EQ(layout.stats().labels, 1u);
    for (const auto& glyph : glyphs) EXPECT_LT(glyph.rect.center().x(), 120);
    EXPECT_EQ(layout.stats().rebuilds, rebuilds) << "cached heatmap runs still relayout against new cards";
    appearance.occlusion = {};
    layout.layout(cells, atlas(), camera, 1, appearance, glyphs);
    EXPECT_EQ(layout.stats().labels, 2u);
}

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

// The chart text rule: text grows with the cells from 13 to 18 px in whole logical
// pixels and needs 2 px on each side; never smaller than 13 px.
TEST_F(LabelLayout, SizeGrowsFrom13To18InWholePixels) {
    auto cells = window(6, 4);
    auto symbolStyle = style();
    symbolStyle.currencySymbol = true; // preserve the worst-case width when the optional $ is enabled
    struct Case {
        double cellW, cellH, size;
    };
    for (const Case c : {Case{120, 16, 0},     // shorter than 13 px + 2 x 2 px: none
                         Case{120, 17.5, 0},   // turning text on needs one spare pixel of height
                         Case{120, 18, 13},    // 13 px with its padding and the spare pixel
                         Case{120, 20.5, 15},  // whole pixels: 15.5 rounds down
                         Case{200, 80, 18}}) { // large cells: capped at 18
        SCOPED_TRACE(std::to_string(c.cellW) + "x" + std::to_string(c.cellH));
        HeatmapLabelLayout fresh;
        const QRectF rect(0, 0, 6 * c.cellW, 4 * c.cellH);
        fresh.layout(cells, atlas(), fit(rect, 6, 4), 1.0, symbolStyle, glyphs);
        EXPECT_EQ(fresh.stats().wanted, c.cellH < 17 ? 0u : 24u) << "rows below the hard minimum want no label";
        EXPECT_EQ(fresh.stats().labels, c.size > 0 ? 24u : 0u);
        EXPECT_EQ(fresh.stats().sizePx, c.size);
        // Text height: the glyph quads never exceed the em box plus the atlas padding.
        for (const auto &g : glyphs) EXPECT_LE(g.rect.height(), fresh.stats().sizePx * 1.6);
    }
}

// The width the text rule gives `text` at `px`: monospace, its glyph count times the advance.
double inkWidth(const char *text, double px) {
    return double(std::strlen(text)) * atlas().glyph(QChar('0')).advance * px / atlas().fontPx();
}

// All or none: in a frame where the short labels fit but the widest does not even in
// its compact form, no cell shows text (before W-0001 the cells that fit drew theirs
// and the others waited for room). With room for the widest, every cell shows text.
TEST_F(LabelLayout, AFrameDrawsEveryWantedLabelOrNone) {
    auto cells = window(6, 4); // $50k .. $568k
    auto s = style();
    s.currencySymbol = true;
    const double widest = inkWidth("$568k", 13) + 4, shortest = inkWidth("$50k", 13) + 4;
    ASSERT_LT(shortest + 1, widest - 1);
    for (const double cellW : {shortest + 1, widest - 1}) {
        HeatmapLabelLayout fresh;
        fresh.layout(cells, atlas(), fit(QRectF(0, 0, 6 * cellW, 4 * 40), 6, 4), 1.0, s, glyphs);
        // Edge cells whose label overflows the drawRect even at its smallest are not wanted.
        EXPECT_GT(fresh.stats().wanted, 12u) << cellW;
        EXPECT_EQ(fresh.stats().labels, 0u) << cellW;
        EXPECT_EQ(fresh.stats().hidden, fresh.stats().wanted) << cellW;
        EXPECT_TRUE(glyphs.empty()) << cellW;
    }
    HeatmapLabelLayout fresh;
    const double roomy = widest + chart::ChartTextRule::kHysteresisPx;
    fresh.layout(cells, atlas(), fit(QRectF(0, 0, 6 * roomy, 4 * 40), 6, 4), 1.0, s, glyphs);
    EXPECT_EQ(fresh.stats().labels, 24u);
    EXPECT_EQ(labels(glyphs).size(), 24u);
}

// Before hiding, every label of the frame steps to the compact form at once.
TEST_F(LabelLayout, CompactFormForEveryLabelBeforeHiding) {
    const double values[] = {12.4, 3.33, 0.7, 1.11}; // x $100k: $1.24M $333k $70k $111k
    auto cells = window(4, 1, 20000);
    for (size_t i = 0; i < 4; ++i) {
        cells->cells[i].value = values[i];
        formatLabelCell(cells->cells[i], 100'000);
    }
    const double full = inkWidth("1.24M", 13) + 4;
    double compact = 0;
    for (const char *t : {"1.2M", "333k", "70k", "111k"}) compact = std::max(compact, inkWidth(t, 13) + 4);
    ASSERT_LT(compact + chart::ChartTextRule::kHysteresisPx, full);
    const double cellW = compact + chart::ChartTextRule::kHysteresisPx;
    layout.layout(cells, atlas(), fit(QRectF(0, 0, 4 * cellW, 40), 4, 1), 1.0, style(), glyphs);
    EXPECT_EQ(layout.stats().labels, 4u) << "every label, not only those whose full form fits";
    EXPECT_EQ(layout.stats().form, 1);
    std::map<double, int> perCell;
    for (const auto &g : glyphs) ++perCell[std::floor(g.debugAnchor.x() / cellW)];
    EXPECT_EQ(perCell[0], 4) << "1.2M";
    EXPECT_EQ(perCell[1], 4) << "333k: integer digits stay";
    EXPECT_EQ(perCell[2], 3) << "70k";
    // Room for the full form (with the hysteresis margin): back to it.
    const double roomy = full + chart::ChartTextRule::kHysteresisPx;
    layout.layout(cells, atlas(), fit(QRectF(0, 0, 4 * roomy, 40), 4, 1), 1.0, style(), glyphs);
    EXPECT_EQ(layout.stats().form, 0);
    EXPECT_EQ(layout.stats().labels, 4u);
}

// Hysteresis across frames: zooming back and forth inside the band never flips the text.
TEST_F(LabelLayout, NoFlipWhileZoomingInsideTheHysteresisBand) {
    auto cells = window(6, 4);
    auto s = style();
    s.currencySymbol = true;
    const double exact = inkWidth("$568k", 13) + 4;
    const auto frame = [&](double cellW) {
        layout.layout(cells, atlas(), fit(QRectF(0, 0, 6 * cellW, 4 * 40), 6, 4), 1.0, s, glyphs);
        return layout.stats().labels;
    };
    for (int i = 0; i < 20; ++i) EXPECT_EQ(frame(exact + 0.1 * (i % 20)), 0u) << "off: no spare width yet";
    EXPECT_EQ(frame(exact + chart::ChartTextRule::kHysteresisPx + 0.01), 24u);
    for (int i = 0; i < 20; ++i) EXPECT_EQ(frame(exact + 0.1 * (i % 20)), 24u) << "on: it still fits";
    EXPECT_EQ(frame(exact - 0.5), 0u);
    layout.reset();
    EXPECT_EQ(frame(exact + 1), 0u) << "reset forgets the decision";
    // A new timeframe, tick or symbol forgets it too (a new LabelCells of the same view).
    EXPECT_EQ(frame(exact + chart::ChartTextRule::kHysteresisPx + 0.01), 24u);
    EXPECT_EQ(frame(exact + 1), 24u);
    for (const auto change : {+[](LabelCells &c) { c.key.tickUnits = 50; }, +[](LabelCells &c) { c.key.asset = "ETH"; },
                              +[](LabelCells &c) { c.key.tfMs = 5 * minute; }}) {
        auto other = window(6, 4);
        change(*other);
        other->grid.tfMs = other->key.tfMs;
        // The same cells at the same pixels: the queried window at its own timeframe.
        const double tf = double(other->grid.tfMs);
        const auto camera = mapping(QRectF(0, 0, 6 * (exact + 1), 4 * 40), 1000.0 * tf, 1006.0 * tf, 100'000,
                                    100'004, tf);
        layout.layout(other, atlas(), camera, 1.0, s, glyphs);
        EXPECT_EQ(layout.stats().wanted, 24u) << "geometrically the same frame";
        EXPECT_EQ(layout.stats().labels, 0u) << "a new scope decides afresh";
        cells = window(6, 4);
        EXPECT_EQ(frame(exact + chart::ChartTextRule::kHysteresisPx + 0.01), 24u);
        EXPECT_EQ(frame(exact + 1), 24u);
    }
}

// HM-8: the label's baseline moves with its cell's snapped edge while panning, never on
// its own rounding: the offset from the cell's device-pixel top is constant.
TEST_F(LabelLayout, BaselineMovesWithTheSnappedCell) {
    auto cells = window(1, 6);
    for (const double dpr : {1.0, 2.0}) {
        std::set<double> offsets;
        for (int i = 0; i < 40; ++i) {
            const double pan = 0.037 * i; // price units; the row is 30 px tall
            const QRectF rect(0, 0, 160, 120);
            const auto m = mapping(rect, 1000.0 * minute, 1001.0 * minute, 100'001 + pan, 100'005 + pan);
            layout.layout(cells, atlas(), m, dpr, style(), glyphs);
            ASSERT_GT(layout.stats().labels, 0u);
            for (const auto &g : glyphs) {
                // The cell holding this label: rows 100001..100005 (top edge at the higher price).
                const double y = g.debugAnchor.y();
                double best = 1e9;
                for (int b = 100'002; b <= 100'006; ++b) {
                    const double top = std::round(m.priceToScreenY(double(b)) * dpr) / dpr;
                    if (top <= y && y - top < best) best = y - top;
                }
                offsets.insert(std::round(best * dpr * 1000) / 1000);
            }
        }
        EXPECT_EQ(offsets.size(), 1u) << "dpr " << dpr;
    }
}

// Principle 8: the same value prints the same text in every chart layer.
// wanted() places the smallest box exactly as drawing does: the snapped glyph quads with the
// atlas padding, not the ink. A card that touches only the padding of the narrowest box at
// the smallest size rules the label out; one just clear of it does not.
TEST_F(LabelLayout, WantedUsesTheGlyphQuadNotTheInk) {
    auto cells = window(1, 1);
    auto s = style();
    s.rule = chart::ChartTextRule::clamped(13, 13, 2); // the drawn size is the smallest size
    const auto camera = fit(QRectF(0, 0, 160, 40), 1, 1);
    layout.layout(cells, atlas(), camera, 1.0, s, glyphs);
    ASSERT_EQ(layout.stats().labels, 1u);
    const double quadLeft = glyphs.front().rect.left();
    const double padding = atlas().paddingPx() * 13.0 / atlas().fontPx();
    ASSERT_GT(padding, 0.3);
    for (const auto &[right, wanted] : {std::pair{quadLeft + 0.2, 0u}, std::pair{quadLeft - 0.05, 1u}}) {
        std::array<QRectF, 1> card{QRectF(QPointF(0, 0), QPointF(right, 40))};
        s.occlusion = card;
        HeatmapLabelLayout fresh;
        fresh.layout(cells, atlas(), camera, 1.0, s, glyphs);
        EXPECT_EQ(fresh.stats().wanted, wanted) << "card right edge " << right - quadLeft << " px from the quad";
        EXPECT_EQ(fresh.stats().labels, wanted);
    }
    // Rows below the hard minimum (13 + 2 x 2) want nothing; rows in the retention band do.
    HeatmapLabelLayout band;
    band.layout(cells, atlas(), fit(QRectF(0, 0, 160, 17), 1, 1), 1.0, style(), glyphs);
    EXPECT_EQ(band.stats().wanted, 1u) << "17 px rows are wanted (no turn-on margin here)";
    band.layout(cells, atlas(), fit(QRectF(0, 0, 160, 16.4), 1, 1), 1.0, style(), glyphs);
    EXPECT_EQ(band.stats().wanted, 0u);
}

// Principle 8: the same value and the same form give the same text in every chart layer
// (layers choose their form independently). Footprint numbers print each value through the
// same formatter, also in mixed-magnitude frames (no shared unit or decimals).
TEST(ChartNumbers, SameValueSameTextInEveryLayer) {
    for (const double value : {0.0042, 0.0642, 0.5, 1.005, 3.6, 12.5, 999.6, 12'345.0, 1'240'000.0, 2.1e9}) {
        SCOPED_TRACE(value);
        LabelCell cell;
        cell.value = value;
        formatLabelCell(cell, 1.0);
        for (const auto form : {chart::NumberForm::Full, chart::NumberForm::Compact}) {
            char shared[24];
            chart::formatChartNumber(value, shared, {form});
            EXPECT_STREQ(cell.asset[int(form)], shared);
            EXPECT_EQ(std::string(cell.usd[int(form)]), std::string("$") + shared);
        }
        char full[24], footer[24], cvd[24], badge[24];
        chart::formatChartNumber(value, full);
        chart::footerNumber(value, footer, false);
        chart::CvdGutterValues::format(value, cvd);
        const int n = chart::formatMoney(value, badge);
        badge[n] = 0;
        EXPECT_STREQ(footer, full);
        EXPECT_STREQ(cvd, full);
        EXPECT_STREQ(badge, cell.usd[0]);
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
    layout.layout(cells, atlas(), mapping(rect, 1010.0 * minute, 1030.0 * minute, 100'010, 100'040), 2.0, style(),
                  glyphs);
    ASSERT_GT(layout.stats().labels, 0u);
    const size_t internal = layout.capacityBytes(), outCapacity = glyphs.capacity();
    const uint64_t rebuilds = layout.stats().rebuilds;
    EXPECT_EQ(rebuilds, 1u);
    for (int i = 0; i < 120; ++i) {
        const double pan = 0.37 * i, zoom = 1.0 + 0.004 * i;
        layout.layout(cells, atlas(),
                      mapping(rect, (1010.0 + pan) * minute, (1010.0 + pan + 20 * zoom) * minute, 100'010 + pan / 2,
                              100'010 + pan / 2 + 30 * zoom),
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

// The glyph budget, all or none: a form whose wanted labels need more than maxGlyphs glyphs
// is not used; Compact when it fits the budget, else no label at all, never a subset.
TEST_F(LabelLayout, TheGlyphBudgetIsAllOrNone) {
    auto cells = window(20, 20);
    const auto camera = fit(QRectF(0, 0, 20 * 90, 20 * 30), 20, 20);
    auto s = style();
    layout.layout(cells, atlas(), camera, 1.0, s, glyphs);
    ASSERT_EQ(layout.stats().labels, 400u);
    ASSERT_EQ(layout.stats().form, 0);
    size_t full = glyphs.size(), compact = 0;
    for (const auto &c : cells->cells) compact += std::strlen(c.usd[1]) - (std::strchr(c.usd[1], '$') ? 1 : 0);
    ASSERT_LT(compact, full);
    s.maxGlyphs = full - 1;
    HeatmapLabelLayout one;
    one.layout(cells, atlas(), camera, 1.0, s, glyphs);
    EXPECT_EQ(one.stats().labels, 400u) << "every label in the compact form";
    EXPECT_EQ(one.stats().form, 1);
    EXPECT_EQ(one.stats().droppedBudget, 0u);
    s.maxGlyphs = compact - 1;
    HeatmapLabelLayout none;
    none.layout(cells, atlas(), camera, 1.0, s, glyphs);
    EXPECT_EQ(none.stats().labels, 0u);
    EXPECT_TRUE(glyphs.empty());
    EXPECT_EQ(none.stats().hidden, 400u);
}

// Only labels the frame can show count for its decision: a wide label in a cell behind a
// footprint card or the footer band, or cut by the drawRect's edge, neither compacts nor
// hides the narrow labels that are visible.
TEST_F(LabelLayout, HiddenCellsDoNotDecideTheVisibleLabels) {
    auto cells = window(4, 1, 20000);
    const double values[] = {0.5, 0.6, 12.4, 0.7}; // x $100k: $50k $60k $1.24M $70k
    for (size_t i = 0; i < 4; ++i) {
        cells->cells[i].value = values[i];
        formatLabelCell(cells->cells[i], 100'000);
    }
    const double narrow = inkWidth("70k", 13) + 4 + chart::ChartTextRule::kHysteresisPx;
    ASSERT_LT(narrow, inkWidth("1.2M", 13) + 4);
    const auto camera = fit(QRectF(0, 0, 4 * narrow, 40), 4, 1);
    {
        HeatmapLabelLayout plain;
        plain.layout(cells, atlas(), camera, 1.0, style(), glyphs);
        EXPECT_EQ(plain.stats().labels, 0u) << "with the wide label visible, none fits";
    }
    for (const QRectF card : {QRectF(2 * narrow - 1, -5, narrow + 2, 50),   // a footprint card over the cell
                              QRectF(2 * narrow, 0, narrow, 40)}) {        // exactly the cell (footer band)
        std::array<QRectF, 1> cards{card};
        auto s = style();
        s.occlusion = cards;
        HeatmapLabelLayout fresh;
        fresh.layout(cells, atlas(), camera, 1.0, s, glyphs);
        EXPECT_EQ(fresh.stats().wanted, 3u);
        EXPECT_EQ(fresh.stats().labels, 3u);
        EXPECT_EQ(fresh.stats().form, 0) << "the narrow labels keep their full form";
    }
    // The wide cell cut by the drawRect's edge through its middle: its label can never draw.
    HeatmapLabelLayout clipped;
    auto view = mapping(QRectF(0, 0, 2.5 * narrow, 40), 1000.0 * minute, 1002.5 * minute, 100'000, 100'001);
    clipped.layout(cells, atlas(), view, 1.0, style(), glyphs);
    EXPECT_EQ(clipped.stats().labels, 2u);
    EXPECT_EQ(clipped.stats().form, 0);
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
    EXPECT_NE(glyphs.size(), usdGlyphs); // "$" and k/M vs the plain number
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
// The budgets at the shared renderer's limit: a 3840x2160 view of 96x108 cells with
// four-glyph labels (41,472 glyphs) draws all its labels and the renderer takes them with
// the small consumers' room; one Low glyph past ChartTextRenderer::kMaxGlyphs is dropped whole.
TEST_F(LabelLayout, A4kFrameFitsTheSharedTextLimit) {
    static_assert(HeatmapLabelLayout::kMaxGlyphs + 2000 <= size_t(ChartTextRenderer::kMaxGlyphs));
    auto cells = window(96, 108);
    for (auto &cell : cells->cells) {
        cell.value = 1.234; // x $100k: 123k in both forms
        formatLabelCell(cell, 100'000);
    }
    std::vector<ChartGlyphInstance> out;
    out.reserve(HeatmapLabelLayout::kMaxGlyphs);
    layout.layout(cells, atlas(), fit(QRectF(0, 0, 3840, 2160), 96, 108), 1.0, style(), out);
    ASSERT_EQ(layout.stats().labels, 96u * 108u);
    ASSERT_EQ(out.size(), 41'472u);
    QSGNode root;
    ChartTextRenderer renderer;
    renderer.beginFrame(&root, nullptr, atlas());
    renderer.submitGlyphs(out, ChartTextRenderer::Priority::Low);
    EXPECT_EQ(renderer.submittedGlyphs(), 41'472);
    std::vector<ChartGlyphInstance> rest(size_t(ChartTextRenderer::kMaxGlyphs) - out.size(), out.front());
    renderer.submitGlyphs(rest, ChartTextRenderer::Priority::Low);
    EXPECT_EQ(renderer.submittedGlyphs(), ChartTextRenderer::kMaxGlyphs) << "exactly at the limit";
    renderer.submitGlyphs({out.front()}, ChartTextRenderer::Priority::Low);
    EXPECT_EQ(renderer.droppedLowGlyphs(), 1);
    EXPECT_EQ(renderer.submittedGlyphs(), ChartTextRenderer::kMaxGlyphs);
    renderer.endFrame();
}

// The submit path (S7b review): ChartTextRenderer counts glyphs per colour with
// no per-call container and grows its colour buckets geometrically; ChartTextNode
// grows its geometry geometrically. Rising glyph counts reallocate O(log n) times,
// steady counts never.
TEST_F(LabelLayout, TextSubmissionAndGeometryGrowGeometrically) {
    std::vector<ChartGlyphInstance> batch;
    batch.reserve(40'000);
    auto fill = [&](size_t n) {
        batch.clear();
        for (size_t i = 0; i < n; ++i) {
            ChartGlyphInstance g;
            g.rect = QRectF(double(i % 900), double(i / 900), 6, 9);
            g.uv = QRectF(0, 0, 0.01, 0.5);
            g.color = QColor::fromRgba(i % 3 ? HeatmapLabelLayout::kLightText : HeatmapLabelLayout::kDarkText);
            batch.push_back(g);
        }
    };
    QSGNode root;
    ChartTextRenderer renderer;
    ChartTextNode node;
    size_t rendererGrowths = 0, nodeGrowths = 0, lastBytes = 0;
    int lastCapacity = 0;
    for (int frame = 0; frame < 120; ++frame) { // 250 .. 30,000 glyphs, rising every frame
        fill(250 + size_t(frame) * 250);
        renderer.beginFrame(&root, nullptr, atlas());
        renderer.submitGlyphs(batch, ChartTextRenderer::Priority::High);
        node.updateGeometry(batch);
        rendererGrowths += renderer.capacityBytes() != lastBytes;
        nodeGrowths += node.capacityGlyphs() != lastCapacity;
        lastBytes = renderer.capacityBytes();
        lastCapacity = node.capacityGlyphs();
    }
    EXPECT_LE(rendererGrowths, 30u) << "buckets grow geometrically, not to every new maximum";
    EXPECT_LE(nodeGrowths, 16u) << "geometry grows geometrically, not to every new maximum";
    // Exact-size growth would reallocate on all 120 rising frames.
    // Steady and falling counts: nothing grows.
    for (int frame = 0; frame < 120; ++frame) {
        fill(frame % 2 ? 30'000 : 12'000);
        renderer.beginFrame(&root, nullptr, atlas());
        renderer.submitGlyphs(batch, ChartTextRenderer::Priority::High);
        node.updateGeometry(batch);
        EXPECT_EQ(renderer.capacityBytes(), lastBytes);
        EXPECT_EQ(node.capacityGlyphs(), lastCapacity);
    }
    // Unused vertices after the drawn glyphs are degenerate (zero), so a shorter
    // frame draws nothing stale.
    fill(10);
    node.updateGeometry(batch);
    const auto *v = node.geometry()->vertexDataAsTexturedPoint2D();
    for (int i = 10 * 6; i < 30'000 * 6; i += 997) EXPECT_EQ(v[i].x, 0.0f) << i;
    // More colours than the counter tracks still submit every glyph.
    batch.clear();
    for (int i = 0; i < 40; ++i) {
        ChartGlyphInstance g;
        g.color = QColor::fromRgb(i * 5, 0, 0);
        batch.push_back(g);
    }
    renderer.beginFrame(&root, nullptr, atlas());
    renderer.submitGlyphs(batch, ChartTextRenderer::Priority::High);
    EXPECT_EQ(renderer.droppedGlyphs(), 0);
}
} // namespace

int main(int argc, char **argv) {
    QTemporaryDir home;
    if (!home.isValid()) return 1;
    qputenv("HOME", home.path().toUtf8());
    qputenv("CFFIXED_USER_HOME", home.path().toUtf8());
    QStandardPaths::setTestModeEnabled(true);
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    const auto data = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (!data.startsWith(home.path() + '/')) {
        qCritical() << "Test data directory escaped the temporary home:" << data;
        return 1;
    }
    Q_INIT_RESOURCE(sentinel_ui_fonts);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
