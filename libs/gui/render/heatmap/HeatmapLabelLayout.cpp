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
    for (auto &runs : runOfCell_) runs.reserve(16'000);
    runs_.reserve(16'000 * chart::kNumberForms);
    glyphs_.reserve(16'000 * 8 * chart::kNumberForms);
    columnX_.reserve(16'385);
    rowY_.reserve(16'385);
}

void HeatmapLabelLayout::reset() {
    built_.reset();
    builtAtlas_ = nullptr;
    builtAtlasKey_ = 0;
    fit_ = {};
    scope_ = {};
}

size_t HeatmapLabelLayout::capacityBytes() const {
    size_t cells = 0;
    for (const auto &runs : runOfCell_) cells += runs.capacity();
    return glyphs_.capacity() * sizeof(UnitGlyph) + runs_.capacity() * sizeof(Run) + cells * sizeof(int32_t) +
           (columnX_.capacity() + rowY_.capacity()) * sizeof(double);
}

void HeatmapLabelLayout::rebuild(const LabelCells &cells, const ChartTextAtlas &atlas, bool usd, bool currencySymbol) {
    ++stats_.rebuilds;
    glyphs_.clear();
    runs_.clear();
    const float pad = float(atlas.paddingPx());
    const auto &zero = atlas.glyph(QChar('0'));
    digitTop_ = float(zero.bounds.top()) + pad;
    digitBottom_ = float(zero.bounds.bottom()) - pad;
    for (int form = 0; form < chart::kNumberForms; ++form) {
        auto &runOfCell = runOfCell_[size_t(form)];
        runOfCell.assign(cells.cells.size(), -1);
        for (size_t i = 0; i < cells.cells.size(); ++i) {
            const auto &cell = cells.cells[i];
            if (tiles::cellState(cell.word) != tiles::kCellValid || !(cell.word & 0x7fffu)) continue;
            const char *text = usd ? cell.usd[form] : cell.asset[form];
            Run run;
            run.first = uint32_t(glyphs_.size());
            float pen = 0, minX = std::numeric_limits<float>::max(), maxX = std::numeric_limits<float>::lowest();
            run.top = std::numeric_limits<float>::max();
            run.bottom = std::numeric_limits<float>::lowest();
            for (const char *c = text; *c; ++c) {
                if (usd && !currencySymbol && *c == '$') continue;
                const auto &g = atlas.glyph(QChar(*c));
                if (g.advance > 0 && !g.uv.isNull()) {
                    minX = std::min(minX, pen + float(g.bounds.left()));
                    maxX = std::max(maxX, pen + float(g.bounds.right()));
                    run.top = std::min(run.top, float(g.bounds.top()));
                    run.bottom = std::max(run.bottom, float(g.bounds.bottom()));
                    glyphs_.push_back({pen + float(g.bounds.left()), float(g.bounds.top()), float(g.bounds.width()),
                                       float(g.bounds.height()), g.uv});
                }
                pen += g.advance;
            }
            run.count = uint16_t(glyphs_.size() - run.first);
            if (!run.count) continue;
            for (uint32_t k = run.first; k < glyphs_.size(); ++k) glyphs_[k].left -= minX;
            run.boxWidth = maxX - minX;
            runOfCell[i] = int32_t(runs_.size());
            runs_.push_back(run);
        }
    }
}

bool HeatmapLabelLayout::visibleWindow(const LabelCells &cells, const TimeAxisMapping &mapping, Window &w) const {
    const auto &grid = cells.grid;
    const double tf = double(grid.tfMs), tick = grid.tick;
    const double viewLo = mapping.viewStartMs, viewHi = mapping.viewEndMs;
    const double priceLo = mapping.viewMinPrice, priceHi = mapping.viewMaxPrice;
    if (!finite(viewLo) || !finite(viewHi) || !(viewHi > viewLo) || !finite(priceLo) || !finite(priceHi) ||
        !(priceHi > priceLo))
        return false;
    const int64_t firstBucket = grid.firstBucket, endBucket = grid.firstBucket + int64_t(grid.columns);
    w.c0 = std::max(firstBucket, int64_t(std::floor(viewLo / tf)));
    w.c1 = std::min(endBucket, int64_t(std::ceil(viewHi / tf)));
    const int64_t firstBin = grid.firstBin, endBin = grid.firstBin + int64_t(grid.rows);
    w.b0 = std::max(firstBin, int64_t(std::floor(priceLo / tick)));
    w.b1 = std::min(endBin, int64_t(std::ceil(priceHi / tick)));
    if (w.c1 <= w.c0 || w.b1 <= w.b0) return false;
    w.cellW = std::abs(mapping.timeToScreenX(double(w.c0 + 1) * tf) - mapping.timeToScreenX(double(w.c0) * tf));
    w.cellH = std::abs(mapping.priceToScreenY(double(w.b0) * tick) - mapping.priceToScreenY(double(w.b0 + 1) * tick));
    return true;
}

// The one test of which cells want a label, for the decision and for placement: coloured
// exactly as heatmap_display.frag ((code - codeFloor) / range > 0), with text, in a column
// that shows the labels' picture, and whose label can draw at all: its smallest box (the
// narrowest form at the rule's smallest size, centred in the cell) lies inside the
// drawRect and off every occlusion rect. A cell outside the drawRect, cut by its edge
// through that box, or behind a footprint card or the footer band wants none, so it
// cannot compact or hide the labels that are visible.
bool HeatmapLabelLayout::wanted(const LabelCells &cells, const Window &w, int64_t c, int64_t b, const QRectF &clip,
                                const LabelStyle &style, const std::vector<uint8_t> *columns, bool count) {
    const auto &grid = cells.grid;
    const size_t row = size_t(grid.firstBin + int64_t(grid.rows) - 1 - b); // LabelCells: highest price first
    const size_t column = size_t(c - grid.firstBucket), index = row * grid.columns + column;
    if (runOfCell_[0][index] < 0) return false;
    if (!(float(cells.cells[index].word & 0x7fffu) - style.window.floor > 0.0f)) return false;
    if (columns && (columns->size() != grid.columns || !(*columns)[column])) {
        if (count) ++stats_.unmatched;
        return false;
    }
    const size_t x = size_t(c - w.c0), y = size_t(w.b1 - 1 - b);
    // Rows shorter than the rule's hard minimum hold no text at any size: skip the geometry.
    if (rowY_[y + 1] - rowY_[y] < style.rule.minCellHeight()) return false;
    // The narrowest available form at the rule's smallest size, placed exactly as place() does.
    const int32_t compact = runOfCell_[chart::kNumberForms - 1][index], full = runOfCell_[0][index];
    const int32_t narrowest = compact >= 0 && runs_[size_t(compact)].boxWidth < runs_[size_t(full)].boxWidth ? compact : full;
    return placeable(placeLabel(runs_[size_t(narrowest)], x, y, minScale_, dpr_).box, clip, style);
}

// The snapped glyph-quad rectangle of a label (atlas padding included) at `scale`: centred in
// its cell horizontally, the digits' ink centred between the cell's snapped edges, on a device pixel.
HeatmapLabelLayout::Placed HeatmapLabelLayout::placeLabel(const Run &run, size_t x, size_t y, double scale,
                                                          double dpr) const {
    const double cx = (columnX_[x] + columnX_[x + 1]) * 0.5;
    const double width = double(run.boxWidth) * scale;
    Placed p;
    p.left = snap(cx - width * 0.5, dpr);
    const double cellTop = rowY_[y], cellBottom = rowY_[y + 1];
    const double ink = double(digitBottom_ - digitTop_) * scale;
    p.baseline = cellTop + snap((cellBottom - cellTop - ink) * 0.5 - double(digitTop_) * scale, dpr);
    p.box = QRectF(p.left, p.baseline + double(run.top) * scale, width, double(run.bottom - run.top) * scale);
    return p;
}

// Inside the drawRect and off every occlusion rect.
bool HeatmapLabelLayout::placeable(const QRectF &box, const QRectF &clip, const LabelStyle &style) {
    if (box.left() < clip.left() || box.top() < clip.top() || box.right() > clip.right() || box.bottom() > clip.bottom())
        return false;
    return std::none_of(style.occlusion.begin(), style.occlusion.end(),
                        [&](const QRectF &card) { return card.intersects(box); });
}

// Once per frame: the most glyphs of any wanted label, per form, decides the layer's text;
// a form whose glyphs for every wanted label exceed the budget is not drawn at all.
void HeatmapLabelLayout::decide(const LabelCells &cells, const Window &w, const ChartTextAtlas &atlas, double dpr,
                                const QRectF &clip, const LabelStyle &style, const std::vector<uint8_t> *columns) {
    const auto &grid = cells.grid;
    const int64_t endBin = grid.firstBin + int64_t(grid.rows);
    chart::ChartTextDemand demand;
    demand.forms = chart::kNumberForms;
    std::array<size_t, chart::kNumberForms> total{};
    for (int64_t b = w.b0; b < w.b1; ++b) {
        const size_t row = size_t(endBin - 1 - b);
        for (int64_t c = w.c0; c < w.c1; ++c) {
            if (!wanted(cells, w, c, b, clip, style, columns, true)) continue;
            ++stats_.wanted;
            const size_t index = row * grid.columns + size_t(c - grid.firstBucket);
            for (size_t f = 0; f < total.size(); ++f) {
                const int32_t run = runOfCell_[f][index];
                if (run < 0) continue;
                const int count = runs_[size_t(run)].count;
                demand.glyphs[f] = std::max(demand.glyphs[f], count);
                total[f] += size_t(count);
            }
        }
    }
    if (!stats_.wanted) return; // nothing to show: keep the decision for the next frame
    demand.withinBudget = 0;
    for (size_t f = 0; f < total.size(); ++f)
        if (total[f] <= style.maxGlyphs) demand.withinBudget |= 1u << f;
    const double advance = double(atlas.glyph(QChar('0')).advance) / double(atlas.fontPx());
    fit_ = chart::decideText({w.cellW, w.cellH, dpr, style.rule}, advance, demand, fit_);
}

void HeatmapLabelLayout::edges(const LabelCells &cells, const Window &w, const TimeAxisMapping &mapping, double dpr) {
    const double tf = double(cells.grid.tfMs), tick = cells.grid.tick;
    columnX_.clear();
    for (int64_t c = w.c0; c <= w.c1; ++c) columnX_.push_back(mapping.timeToScreenX(double(c) * tf));
    rowY_.clear();
    for (int64_t b = w.b1; b >= w.b0; --b) rowY_.push_back(snap(mapping.priceToScreenY(double(b) * tick), dpr));
}

void HeatmapLabelLayout::place(const LabelCells &cells, const Window &w, double dpr, const QRectF &clip,
                              const LabelStyle &style, std::vector<ChartGlyphInstance> &out,
                              const std::vector<uint8_t> *columns) {
    const auto &grid = cells.grid;
    const int64_t endBin = grid.firstBin + int64_t(grid.rows);
    const auto &runOfCell = runOfCell_[size_t(fit_.form)];
    const auto &palette = style.palette;
    for (int64_t b = w.b1 - 1; b >= w.b0; --b) {
        const size_t rowIndex = size_t(w.b1 - 1 - b), row = size_t(endBin - 1 - b);
        for (int64_t c = w.c0; c < w.c1; ++c) {
            const size_t column = size_t(c - grid.firstBucket), index = row * grid.columns + column;
            if (runOfCell[index] < 0 || !wanted(cells, w, c, b, clip, style, columns, false)) continue;
            const uint32_t word = cells.cells[index].word;
            const Run &run = runs_[size_t(runOfCell[index])];
            // Position clipping: a wanted label whose chosen-size box the edge or a card cuts is
            // dropped on its own (not a fit failure).
            const Placed p = placeLabel(run, size_t(c - w.c0), rowIndex, unitScale_, dpr);
            if (!placeable(p.box, clip, style)) continue;
            const double left = p.left, snapped = p.baseline;
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
                glyph.rect = QRectF(left + double(g.left) * unitScale_, snapped + double(g.top) * unitScale_,
                                    double(g.width) * unitScale_, double(g.height) * unitScale_);
                glyph.uv = g.uv;
                glyph.color = color;
                glyph.debugAnchor = QPointF(left, snapped); // the label's left edge and baseline (tests, debug overlay)
                out.push_back(glyph);
            }
            ++stats_.labels;
        }
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
    if (!(grid.tfMs > 0) || !(grid.tick > 0) || !grid.columns || !grid.rows ||
        cells->cells.size() != size_t(grid.columns) * grid.rows)
        return;
    if (cells != built_ || &atlas != builtAtlas_ || atlas.image().cacheKey() != builtAtlasKey_ ||
        style.usd != builtUsd_ || style.currencySymbol != builtCurrencySymbol_) {
        rebuild(*cells, atlas, style.usd, style.currencySymbol);
        built_ = cells;
        builtAtlas_ = &atlas;
        builtAtlasKey_ = atlas.image().cacheKey();
        builtUsd_ = style.usd;
        builtCurrencySymbol_ = style.currencySymbol;
    }
    // A new symbol, timeframe, tick or currency mode decides afresh.
    const auto &key = cells->key;
    if (scope_.asset != key.asset || scope_.tfMs != int64_t(grid.tfMs) || scope_.tickUnits != key.tickUnits ||
        scope_.priceScale != key.priceScale || scope_.usd != style.usd) {
        scope_ = {key.asset, int64_t(grid.tfMs), key.tickUnits, key.priceScale, style.usd};
        fit_ = {};
    }
    Window w;
    if (runs_.empty() || !visibleWindow(*cells, mapping, w)) return;
    edges(*cells, w, mapping, dpr);
    minScale_ = style.rule.minPx / double(atlas.fontPx());
    dpr_ = dpr;
    decide(*cells, w, atlas, dpr, mapping.drawRect, style, columns);
    if (!stats_.wanted) return;
    if (!fit_.on) {
        stats_.hidden = stats_.wanted;
        return;
    }
    unitScale_ = fit_.sizePx / double(atlas.fontPx());
    place(*cells, w, dpr, mapping.drawRect, style, out, columns);
    stats_.glyphs = out.size();
    stats_.sizePx = stats_.labels ? fit_.sizePx : 0;
    stats_.form = fit_.form;
}
} // namespace heatmap::gpu
