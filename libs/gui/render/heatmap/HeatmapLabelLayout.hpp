#pragma once
// Liquidity labels on the GPU main chart. Render thread only (updatePaintNode,
// GUI thread blocked).
//
// Input: the HeatmapCellQuery LabelCells that HeatmapGpuLayer::labelsForFrame()
// matched to the drawn picture (symbol epoch, timeframe, tick). Output: MSDF glyph
// quads for ChartTextRenderer::submitGlyphs.
// - Glyph runs are built once per (LabelCells, currency, atlas) at unit scale (atlas
//   pixels), one run per cell and chart::NumberForm, relative to each label's baseline.
//   Each frame only scales them, translates them to their cell (TimeAxisMapping
//   helpers), snaps them to DEVICE pixels and culls into a reserved vector: no QString,
//   no text measuring and no allocation per frame.
// - A label is wanted on every coloured cell: valid and above the range filter's low
//   handle exactly as heatmap_display.frag colours it (code - codeFloor > 0), in a
//   column that shows the picture the labels describe, in a row at least the rule's hard
//   minimum tall (minPx + 2 * padPx), and whose smallest box is placeable: the narrowest
//   form at minPx, placed by the same function as drawing (placeLabel, the snapped quad
//   rectangle with atlas padding), inside the drawRect and off every occlusion rect.
//   Veil, loading and no-data cells get none. The decision counts every such label; a
//   label whose chosen-size box is then rejected by position (edge or occlusion) is
//   dropped individually: that is position clipping, not a fit failure.
// - All or none (chart::ChartTextRule, chart/core/ChartTextRule.hpp): once per frame the
//   most glyphs of any wanted label decide the size and the number form for every
//   label; when even the Compact form does not fit at the rule's smallest size with its
//   padding, or the wanted labels of a form need more than maxGlyphs glyphs, that form is
//   not used, and with no form left the frame draws no label. The decision keeps its
//   hysteresis across frames; a new symbol, timeframe, tick or currency mode forgets it,
//   as does reset().
// - Placement: centred in the cell horizontally; vertically the digits' ink is centred
//   on the cell's device-pixel-snapped edges, so a label's baseline moves with its cell
//   while zooming, never on its own rounding.
// - A label is drawn only if its whole box lies inside the mapping's drawRect and off
//   every occlusion rect (candles and footprint cards: no text on or behind them). A
//   label of a wanted cell that its edge or a card cuts is left out on its own.
// - Text colour follows the cell's palette colour: dark text on bright cells.
#include "HeatmapCellQuery.hpp"
#include "HeatmapPalette.hpp"
#include "chart/core/ChartTextRule.hpp"
#include "render/ChartTextAtlas.hpp"
#include "render/ChartTextPrimitives.hpp"
#include "render/TimeAxisMapping.hpp"
#include <array>
#include <memory>
#include <vector>
#include <span>
#include <string>

namespace heatmap::gpu {

// The layer's text budget (all or none): a 3840x2160 view of 96x108 cells with four-glyph
// labels needs 41,472 glyphs; within ChartTextRenderer::kMaxGlyphs.
inline constexpr size_t kLabelMaxGlyphs = 64000;

struct LabelStyle {
    std::span<const QRectF> occlusion;
    bool usd = true;
    bool currencySymbol = false;
    chart::ChartTextRule rule;      // sizes and padding, logical px
    CodeWindow window;              // the colour mapping (DrawStyle codeFloor/codeRange)
    std::shared_ptr<const HeatmapPalette> palette; // text colour (nullptr: always light)
    size_t maxGlyphs = kLabelMaxGlyphs;
};

struct LabelLayoutStats {
    size_t labels = 0, glyphs = 0;
    size_t wanted = 0;        // coloured, matched cells of the visible window that have text
    size_t hidden = 0;        // wanted labels not drawn because the layer's text does not fit
    size_t droppedBudget = 0; // labels left out by maxGlyphs (never, when the decision counts them)
    size_t unmatched = 0;     // labels of columns that do not match the drawn picture (matchLabelColumns)
    double sizePx = 0;        // font size this frame (0: none drawn)
    int form = 0;             // chart::NumberForm drawn this frame
    uint64_t rebuilds = 0;    // glyph run rebuilds (once per LabelCells/currency/atlas)
};

class HeatmapLabelLayout {
public:
    static constexpr size_t kMaxGlyphs = kLabelMaxGlyphs;
    static constexpr QRgb kLightText = 0xF0FFFFFFu, kDarkText = 0xF00A0A10u;
    HeatmapLabelLayout();

    // Clears `out` and fills it with this frame's glyphs. `out` keeps its capacity
    // (reserve kMaxGlyphs once); nothing else allocates after the first rebuild.
    // `columns` (optional): one byte per label column, 0 = the column does not
    // show the picture the labels describe (matchLabelColumns): no label there.
    void layout(const std::shared_ptr<const LabelCells> &cells, const ChartTextAtlas &atlas,
                const TimeAxisMapping &mapping, double dpr, const LabelStyle &style,
                std::vector<ChartGlyphInstance> &out, const std::vector<uint8_t> *columns = nullptr);
    // Forget the cached runs (a new atlas, a new scene graph root) and the text decision.
    void reset();
    const LabelLayoutStats &stats() const { return stats_; }
    const chart::ChartTextFit &fit() const { return fit_; }
    // Capacity of every internal vector (tests: no growth in steady state).
    size_t capacityBytes() const;

private:
    struct UnitGlyph {
        float left = 0, top = 0, width = 0, height = 0; // atlas px; left from the run box, top from the baseline
        QRectF uv;
    };
    struct Run {
        uint32_t first = 0;
        uint16_t count = 0;
        float boxWidth = 0;          // quad box width in atlas px (glyph padding included)
        float top = 0, bottom = 0;   // quad box extent from the baseline, atlas px
    };
    struct Window {
        int64_t c0 = 0, c1 = 0, b0 = 0, b1 = 0;
        double cellW = 0, cellH = 0;
    };
    void rebuild(const LabelCells &cells, const ChartTextAtlas &atlas, bool usd, bool currencySymbol);
    bool visibleWindow(const LabelCells &cells, const TimeAxisMapping &mapping, Window &w) const;
    struct Placed { double left = 0, baseline = 0; QRectF box; };
    Placed placeLabel(const Run &run, size_t x, size_t y, double scale, double dpr) const;
    static bool placeable(const QRectF &box, const QRectF &clip, const LabelStyle &style);
    void edges(const LabelCells &cells, const Window &w, const TimeAxisMapping &mapping, double dpr);
    bool wanted(const LabelCells &cells, const Window &w, int64_t c, int64_t b, const QRectF &clip,
                const LabelStyle &style, const std::vector<uint8_t> *columns, bool count);
    void decide(const LabelCells &cells, const Window &w, const ChartTextAtlas &atlas, double dpr,
                const QRectF &clip, const LabelStyle &style, const std::vector<uint8_t> *columns);
    void place(const LabelCells &cells, const Window &w, double dpr, const QRectF &clip, const LabelStyle &style,
               std::vector<ChartGlyphInstance> &out, const std::vector<uint8_t> *columns);

    std::shared_ptr<const LabelCells> built_;
    const ChartTextAtlas *builtAtlas_ = nullptr;
    qint64 builtAtlasKey_ = 0;
    bool builtUsd_ = true;
    bool builtCurrencySymbol_ = false;
    std::vector<UnitGlyph> glyphs_;
    std::vector<Run> runs_;
    std::array<std::vector<int32_t>, chart::kNumberForms> runOfCell_; // per form, per cell: index into runs_, -1 none
    float digitTop_ = 0, digitBottom_ = 0; // the digits' ink from the baseline, atlas px
    std::vector<double> columnX_, rowY_; // per frame, reserved
    double unitScale_ = 0;               // this frame's font size per atlas px
    double minScale_ = 0;                // the rule's smallest size per atlas px
    double dpr_ = 1;                     // this frame's device pixel ratio
    chart::ChartTextFit fit_;
    struct Scope { // what the decision belongs to: symbol, timeframe, tick, currency mode
        std::string asset;
        int64_t tfMs = 0, tickUnits = 0;
        double priceScale = 0;
        bool usd = true;
    } scope_;
    LabelLayoutStats stats_;
};
} // namespace heatmap::gpu
