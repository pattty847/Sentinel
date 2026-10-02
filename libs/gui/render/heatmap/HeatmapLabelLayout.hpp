#pragma once
// Liquidity labels on the GPU main chart (S7b, docs/research/2026-10-s7-plan.md
// sections 3-4 and owner decisions 1-2). Render thread only (updatePaintNode,
// GUI thread blocked).
//
// Input: the HeatmapCellQuery LabelCells that HeatmapGpuLayer::labelsForFrame()
// matched to the drawn picture (symbol epoch, timeframe, tick). Output: MSDF glyph
// quads for ChartTextRenderer::submitGlyphs.
// - Glyph runs are built once per (LabelCells, currency, atlas) at unit scale (atlas
//   pixels), relative to each label's box. Each frame only scales them, translates
//   them to their cell centre (TimeAxisMapping helpers), snaps each label's box to
//   DEVICE pixels (round(x * dpr) / dpr) and culls into a reserved vector: no
//   QString, no text measuring and no allocation per frame.
// - A label appears on every coloured cell: valid and above the range filter's low
//   handle exactly as heatmap_display.frag colours it (code - codeFloor > 0). Veil,
//   loading and no-data cells get none.
// - Size: one size per frame, the largest in [minPx, maxPx] at which every label of
//   the window fits its cell with padding; when even minPx is too wide for some,
//   minPx is used and only the labels that fit at minPx are drawn. No shrink below
//   minPx: cells shorter than minPx plus padding draw no labels.
// - A label is drawn only if its whole box lies inside the mapping's drawRect, and
//   at most maxGlyphs glyphs are emitted (the rest are counted, not drawn).
// - Text colour follows the cell's palette colour: dark text on bright cells.
#include "HeatmapCellQuery.hpp"
#include "HeatmapPalette.hpp"
#include "render/ChartTextAtlas.hpp"
#include "render/ChartTextPrimitives.hpp"
#include "render/TimeAxisMapping.hpp"
#include <memory>
#include <vector>

namespace heatmap::gpu {

struct LabelStyle {
    bool usd = true;
    double minPx = 12, maxPx = 15; // font size (em) in logical px
    double padX = 3, padY = 1;     // logical px on each side of the text box
    CodeWindow window;              // the colour mapping (DrawStyle codeFloor/codeRange)
    std::shared_ptr<const HeatmapPalette> palette; // text colour (nullptr: always light)
    size_t maxGlyphs = 40000;
};

struct LabelLayoutStats {
    size_t labels = 0, glyphs = 0;
    size_t tooNarrow = 0;    // coloured cells whose text does not fit at minPx
    size_t droppedBudget = 0; // labels left out by maxGlyphs
    double sizePx = 0;        // font size this frame (0: none drawn)
    uint64_t rebuilds = 0;    // glyph run rebuilds (once per LabelCells/currency/atlas)
};

class HeatmapLabelLayout {
public:
    static constexpr size_t kMaxGlyphs = 40000;
    static constexpr QRgb kLightText = 0xF0FFFFFFu, kDarkText = 0xF00A0A10u;
    HeatmapLabelLayout();

    // Clears `out` and fills it with this frame's glyphs. `out` keeps its capacity
    // (reserve kMaxGlyphs once); nothing else allocates after the first rebuild.
    void layout(const std::shared_ptr<const LabelCells> &cells, const ChartTextAtlas &atlas,
                const TimeAxisMapping &mapping, double dpr, const LabelStyle &style,
                std::vector<ChartGlyphInstance> &out);
    // Forget the cached runs (a new atlas, a new scene graph root).
    void reset();
    const LabelLayoutStats &stats() const { return stats_; }
    // Capacity of every internal vector (tests: no growth in steady state).
    size_t capacityBytes() const;

private:
    struct UnitGlyph {
        float left = 0, top = 0, width = 0, height = 0; // atlas px, relative to the run box
        QRectF uv;
    };
    struct Run {
        uint32_t first = 0;
        uint16_t count = 0;
        float boxWidth = 0; // quad box width in atlas px (glyph padding included)
        float inkWidth = 0; // boxWidth less the atlas glyph padding at both ends
    };
    void rebuild(const LabelCells &cells, const ChartTextAtlas &atlas, bool usd);

    std::shared_ptr<const LabelCells> built_;
    const ChartTextAtlas *builtAtlas_ = nullptr;
    qint64 builtAtlasKey_ = 0;
    bool builtUsd_ = true;
    std::vector<UnitGlyph> glyphs_;
    std::vector<Run> runs_;
    std::vector<int32_t> runOfCell_; // per cell: index into runs_, -1 none
    float maxInkWidth_ = 0;
    std::vector<double> columnX_, rowY_; // per frame, reserved
    LabelLayoutStats stats_;
};
} // namespace heatmap::gpu
