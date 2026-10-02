#include "HeatmapLabelLayout.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace heatmap::gpu {
namespace {
double snap(double v, double dpr) { return std::round(v * dpr) / dpr; }
bool finite(double v) { return std::isfinite(v); }
} // namespace

HeatmapLabelLayout::HeatmapLabelLayout() {
    // The query's window limit (16,000 cells) bounds every per-cell vector; the
    // grid limits bound the per-frame column/row edges.
    runOfCell_.reserve(16'000);
    runs_.reserve(16'000);
    glyphs_.reserve(16'000 * 8);
    columnX_.reserve(16'385);
    rowY_.reserve(16'385);
}

void HeatmapLabelLayout::reset() {
    built_.reset();
    builtAtlas_ = nullptr;
    builtAtlasKey_ = 0;
}

size_t HeatmapLabelLayout::capacityBytes() const {
    return glyphs_.capacity() * sizeof(UnitGlyph) + runs_.capacity() * sizeof(Run) +
           runOfCell_.capacity() * sizeof(int32_t) + (columnX_.capacity() + rowY_.capacity()) * sizeof(double);
}

void HeatmapLabelLayout::rebuild(const LabelCells &cells, const ChartTextAtlas &atlas, bool usd) {
    ++stats_.rebuilds;
    glyphs_.clear();
    runs_.clear();
    runOfCell_.assign(cells.cells.size(), -1);
    maxInkWidth_ = 0;
    const float top = atlas.glyphTopPx();
    const float pad = float(atlas.paddingPx());
    for (size_t i = 0; i < cells.cells.size(); ++i) {
        const auto &cell = cells.cells[i];
        if (tiles::cellState(cell.word) != tiles::kCellValid || !(cell.word & 0x7fffu)) continue;
        const auto &text = usd ? cell.usd : cell.asset;
        if (!text[0]) continue;
        Run run;
        run.first = uint32_t(glyphs_.size());
        float pen = 0, minX = std::numeric_limits<float>::max(), maxX = std::numeric_limits<float>::lowest();
        for (size_t c = 0; c < text.size() && text[c]; ++c) {
            const auto &g = atlas.glyph(QChar(text[c]));
            if (g.advance > 0 && !g.uv.isNull()) {
                minX = std::min(minX, pen + float(g.bounds.left()));
                maxX = std::max(maxX, pen + float(g.bounds.right()));
                glyphs_.push_back({pen + float(g.bounds.left()), float(g.bounds.top()) - top, float(g.bounds.width()),
                                   float(g.bounds.height()), g.uv});
            }
            pen += g.advance;
        }
        run.count = uint16_t(glyphs_.size() - run.first);
        if (!run.count) continue;
        for (uint32_t k = run.first; k < glyphs_.size(); ++k) glyphs_[k].left -= minX;
        run.boxWidth = maxX - minX;
        run.inkWidth = std::max(0.0f, run.boxWidth - 2 * pad);
        maxInkWidth_ = std::max(maxInkWidth_, run.inkWidth);
        runOfCell_[i] = int32_t(runs_.size());
        runs_.push_back(run);
    }
}

void HeatmapLabelLayout::layout(const std::shared_ptr<const LabelCells> &cells, const ChartTextAtlas &atlas,
                                const TimeAxisMapping &mapping, double dpr, const LabelStyle &style,
                                std::vector<ChartGlyphInstance> &out, const std::vector<uint8_t> *columns) {
    out.clear();
    const uint64_t rebuilds = stats_.rebuilds;
    stats_ = {};
    stats_.rebuilds = rebuilds;
    if (!cells || !atlas.isBuilt() || atlas.fontPx() <= 0 || !mapping.valid || !(dpr > 0) ||
        mapping.drawRect.isEmpty())
        return;
    const auto &grid = cells->grid;
    const double tf = double(grid.tfMs), tick = grid.tick;
    if (!(tf > 0) || !(tick > 0) || !grid.columns || !grid.rows ||
        cells->cells.size() != size_t(grid.columns) * grid.rows)
        return;
    if (cells != built_ || &atlas != builtAtlas_ || atlas.image().cacheKey() != builtAtlasKey_ || style.usd != builtUsd_) {
        rebuild(*cells, atlas, style.usd);
        built_ = cells;
        builtAtlas_ = &atlas;
        builtAtlasKey_ = atlas.image().cacheKey();
        builtUsd_ = style.usd;
    }
    if (runs_.empty()) return;

    // Visible cells of the window.
    const double viewLo = mapping.viewStartMs, viewHi = mapping.viewEndMs;
    const double priceLo = mapping.viewMinPrice, priceHi = mapping.viewMaxPrice;
    if (!finite(viewLo) || !finite(viewHi) || !(viewHi > viewLo) || !finite(priceLo) || !finite(priceHi) ||
        !(priceHi > priceLo))
        return;
    const int64_t firstBucket = grid.firstBucket, endBucket = grid.firstBucket + int64_t(grid.columns);
    const int64_t c0 = std::max(firstBucket, int64_t(std::floor(viewLo / tf)));
    const int64_t c1 = std::min(endBucket, int64_t(std::ceil(viewHi / tf)));
    const int64_t firstBin = grid.firstBin, endBin = grid.firstBin + int64_t(grid.rows);
    const int64_t b0 = std::max(firstBin, int64_t(std::floor(priceLo / tick)));
    const int64_t b1 = std::min(endBin, int64_t(std::ceil(priceHi / tick)));
    if (c1 <= c0 || b1 <= b0) return;

    // One size per frame (owner decision 1).
    const double cellW = std::abs(mapping.timeToScreenX(double(c0 + 1) * tf) - mapping.timeToScreenX(double(c0) * tf));
    const double cellH = std::abs(mapping.priceToScreenY(double(b0) * tick) - mapping.priceToScreenY(double(b0 + 1) * tick));
    const double fontPx = double(atlas.fontPx());
    const double sizeForHeight = cellH - 2 * style.padY; // the em box plus padding fits the cell
    const double sizeForWidth = maxInkWidth_ > 0 ? (cellW - 2 * style.padX) * fontPx / maxInkWidth_ : style.maxPx;
    double size = std::min({style.maxPx, sizeForHeight, sizeForWidth});
    const bool everyLabelFits = size >= style.minPx;
    if (!everyLabelFits) {
        if (sizeForHeight < style.minPx) return; // too short at minPx: no labels, never smaller text
        size = style.minPx;
    }
    const double scale = size / fontPx;
    const double boxHeight = double(atlas.glyphBottomPx() - atlas.glyphTopPx()) * scale;

    columnX_.clear();
    for (int64_t c = c0; c <= c1; ++c) columnX_.push_back(mapping.timeToScreenX(double(c) * tf));
    rowY_.clear();
    for (int64_t b = b1; b >= b0; --b) rowY_.push_back(mapping.priceToScreenY(double(b) * tick)); // top edges first
    const QRectF &clip = mapping.drawRect;
    const auto &palette = style.palette;
    for (int64_t b = b1 - 1; b >= b0; --b) {
        const size_t rowIndex = size_t(b1 - 1 - b);
        const double cy = (rowY_[rowIndex] + rowY_[rowIndex + 1]) * 0.5;
        const size_t row = size_t(endBin - 1 - b); // LabelCells: highest price first
        for (int64_t c = c0; c < c1; ++c) {
            const size_t index = row * grid.columns + size_t(c - firstBucket);
            const int32_t runIndex = runOfCell_[index];
            if (runIndex < 0) continue;
            const uint32_t word = cells->cells[index].word;
            // Coloured exactly as heatmap_display.frag: (code - codeFloor) / range > 0.
            if (!(float(word & 0x7fffu) - style.window.floor > 0.0f)) continue;
            if (columns && (columns->size() != grid.columns || !(*columns)[size_t(c - firstBucket)])) {
                ++stats_.unmatched;
                continue;
            }
            const Run &run = runs_[size_t(runIndex)];
            if (!everyLabelFits && double(run.inkWidth) * scale + 2 * style.padX > cellW) {
                ++stats_.tooNarrow;
                continue;
            }
            const double cx = (columnX_[size_t(c - c0)] + columnX_[size_t(c - c0) + 1]) * 0.5;
            const double width = double(run.boxWidth) * scale;
            const double left = snap(cx - width * 0.5, dpr), top = snap(cy - boxHeight * 0.5, dpr);
            if (left < clip.left() || top < clip.top() || left + width > clip.right() || top + boxHeight > clip.bottom())
                continue;
            if (out.size() + run.count > style.maxGlyphs) {
                ++stats_.droppedBudget;
                continue;
            }
            QColor color = QColor::fromRgba(kLightText);
            if (palette) {
                const auto rgba = legacyRecordingColor(uint16_t(word & 0xffffu), style.window, *palette);
                if (0.2126f * rgba[0] + 0.7152f * rgba[1] + 0.0722f * rgba[2] > 0.55f) color = QColor::fromRgba(kDarkText);
            }
            for (uint32_t k = run.first; k < run.first + run.count; ++k) {
                const auto &g = glyphs_[k];
                ChartGlyphInstance glyph;
                glyph.rect = QRectF(left + double(g.left) * scale, top + double(g.top) * scale, double(g.width) * scale,
                                    double(g.height) * scale);
                glyph.uv = g.uv;
                glyph.color = color;
                glyph.debugAnchor = QPointF(left, top); // the label box origin (tests, debug overlay)
                out.push_back(glyph);
            }
            ++stats_.labels;
        }
    }
    stats_.glyphs = out.size();
    stats_.sizePx = stats_.labels ? size : 0;
}
} // namespace heatmap::gpu
