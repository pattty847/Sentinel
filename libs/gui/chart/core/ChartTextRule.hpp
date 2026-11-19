#pragma once
// One text rule for numbers and letters in chart cells and rows (brief principle 3:
// crisp or hidden). Heatmap cell labels, footprint cell numbers and TPO letters decide
// their text here; the flow footer and the CVD gutter take their row size and padding
// from the same defaults, and every layer prints numbers with formatChartNumber.
// Qt-free; the render thread calls decideText() once per layer per frame.
// - Size: text grows with the cell from minPx to maxPx (logical px), in whole logical
//   pixels so glyphs do not shimmer while zooming. Never below minPx: MSDF text is poor
//   below about 13 px, and the owner wants normal text, not tiny.
// - Width class: chart text is monospace (Roboto Mono), so a label's width is its glyph
//   count times the advance. A layer passes the most glyphs any label it wants to draw
//   has, per NumberForm; labels its own filters drop, and cells fully clipped or fully
//   behind an occlusion, do not count.
// - Fit: a label fits when its glyphs plus padPx on each side fit the cell width and the
//   font size plus padPx above and below fits the cell height.
// - All or none: decideText() picks the fullest form whose widest label fits at minPx
//   and whose glyphs for every wanted label fit the layer's glyph budget; when no form
//   does, the layer draws no text at all, never a subset.
// - Pure decision: the layer's text this frame depends only on the current frame and the
//   validated previous decision {on, form, sizePx}; no memory of past label widths.
// - Accepted limitation (owner 2026-11-18): at an unchanged view whose cells sit exactly at
//   the size limit, a live value that changes length ("999" -> "1k") can switch a layer's
//   text on or off (tested: ChartTextRule.ALengthChangeAtTheLimitTogglesText).
// - Hysteresis (on the previous decision only): text that is drawn stays at its size and form while it still fits exactly; turning text on, going back to a
//   fuller form or growing one pixel needs kHysteresisPx of spare width and
//   kHeightHysteresisPx of spare height. Shrinking, stepping to Compact and hiding happen
//   at once.
// - A decision made under another rule or dpr is no decision: the next frame decides
//   afresh. Layers also forget it on symbol, timeframe, tick and mode changes.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace chart {
struct ChartTextRule {
    static constexpr double kDefaultMinPx = 13, kDefaultMaxPx = 18, kDefaultPadPx = 2;
    static constexpr double kHysteresisPx = 3, kHeightHysteresisPx = 1;
    // Settings limits (textMinPx, textMaxPx, textPadPx in the heatmap settings table).
    static constexpr double kLowestPx = 10, kHighestPx = 32, kMaxPadPx = 8;
    double minPx = kDefaultMinPx, maxPx = kDefaultMaxPx, padPx = kDefaultPadPx;
    bool operator==(const ChartTextRule&) const = default;

    // Whole pixels inside the limits; maxPx never below minPx; non-finite values take the default.
    static ChartTextRule clamped(double minPx, double maxPx, double padPx) {
        const auto whole = [](double v, double lo, double hi, double fallback) {
            return std::isfinite(v) ? std::clamp(std::round(v), lo, hi) : fallback;
        };
        ChartTextRule rule;
        rule.minPx = whole(minPx, kLowestPx, 24, kDefaultMinPx);
        rule.maxPx = std::max(rule.minPx, whole(maxPx, kLowestPx, kHighestPx, kDefaultMaxPx));
        rule.padPx = whole(padPx, 0, kMaxPadPx, kDefaultPadPx);
        return rule;
    }
    // The smallest cell height that holds text.
    double minCellHeight() const { return minPx + 2 * padPx; }
    // The largest whole-pixel size in [minPx, maxPx] at which a label `inkPerPx` wide per px of
    // font size fits a cell of (width - widthMargin) x (height - heightMargin); 0 when not even
    // minPx fits.
    double fitSize(double width, double height, double inkPerPx, double widthMargin = 0,
                   double heightMargin = 0) const {
        if (!std::isfinite(width) || !std::isfinite(height) || !std::isfinite(inkPerPx) || inkPerPx < 0) return 0;
        double size = std::min(maxPx, height - heightMargin - 2 * padPx);
        if (inkPerPx > 0) size = std::min(size, (width - widthMargin - 2 * padPx) / inkPerPx);
        else if (width - widthMargin - 2 * padPx < 0) return 0;
        size = std::floor(size + 1e-9);
        return size >= minPx ? size : 0;
    }
};

// The rule a chart's settings ask for (HeatmapChartSettings textMinPx, textMaxPx, textPadPx).
template <class Settings>
ChartTextRule textRule(const Settings& s) {
    return ChartTextRule::clamped(s.textMinPx, s.textMaxPx, s.textPadPx);
}

inline constexpr int kMaxTextForms = 2; // chart::NumberForm Full, Compact

// The view a decision holds for: the smallest cell the layer's text must fit, the device
// pixel ratio and the rule.
struct ChartTextView {
    double cellWidth = 0, cellHeight = 0, dpr = 0;
    ChartTextRule rule;
    bool operator==(const ChartTextView&) const = default;
};

// What a layer wants to draw this frame.
struct ChartTextDemand {
    std::array<int, kMaxTextForms> glyphs{}; // per form, the most glyphs of any wanted label
    int forms = 1;                           // forms in use, fuller first
    unsigned withinBudget = ~0u;             // bit f: every wanted label in form f fits the glyph budget
};

struct ChartTextFit {
    bool on = false;
    int form = 0;      // index into the layer's forms (NumberForm order: Full, Compact)
    double sizePx = 0; // whole logical px; 0 when off
    ChartTextView view;                    // what this decision was made for (dpr 0: nothing yet)
    bool operator==(const ChartTextFit&) const = default;
};

namespace text_rule {
// The previous decision if it still belongs to this rule and dpr, else no decision.
inline ChartTextFit carried(const ChartTextFit& previous, const ChartTextView& view) {
    const ChartTextRule& rule = view.rule;
    const bool sameScope = previous.view.dpr > 0 && previous.view.dpr == view.dpr && previous.view.rule == rule;
    const bool allowedSize = !previous.on || (previous.sizePx >= rule.minPx && previous.sizePx <= rule.maxPx);
    return sameScope && allowedSize ? previous : ChartTextFit{};
}
// The size of `form` given the previous decision: shrink at once, grow and turn on with spare room.
inline double formSize(const ChartTextView& view, double ink, int form, const ChartTextFit& prev) {
    const ChartTextRule& rule = view.rule;
    const bool holding = prev.on && form >= prev.form;
    const double margin = holding ? 0 : ChartTextRule::kHysteresisPx;
    const double height = holding ? 0 : ChartTextRule::kHeightHysteresisPx;
    const double size = rule.fitSize(view.cellWidth, view.cellHeight, ink, margin, height);
    if (size <= 0 || !prev.on || form != prev.form || prev.sizePx > size) return size;
    const double grown = rule.fitSize(view.cellWidth, view.cellHeight, ink, ChartTextRule::kHysteresisPx,
                                      ChartTextRule::kHeightHysteresisPx);
    return std::max(prev.sizePx, grown);
}
} // namespace text_rule


// One decision per layer per frame. `advancePerPx` is the monospace advance per px of font
// size.
inline ChartTextFit decideText(const ChartTextView& view, double advancePerPx, const ChartTextDemand& demand,
                               const ChartTextFit& previous) {
    const ChartTextFit prev = text_rule::carried(previous, view);
    ChartTextFit out;
    out.view = view;
    if (!std::isfinite(advancePerPx) || advancePerPx <= 0) return out;
    const int forms = std::clamp(demand.forms, 0, kMaxTextForms);
    for (int form = 0; form < forms; ++form) {
        if (!((demand.withinBudget >> unsigned(form)) & 1u)) continue;
        const double size = text_rule::formSize(view, double(demand.glyphs[size_t(form)]) * advancePerPx, form, prev);
        if (size <= 0) continue;
        out.on = true;
        out.form = form;
        out.sizePx = size;
        return out;
    }
    return out;
}

// TPO letters: one glyph per cell, all sessions or none.
inline ChartTextFit tpoLetterFit(const ChartTextFit& previous, const ChartTextView& view, double advancePerPx, bool wanted) {
    if (!wanted) return {};
    ChartTextDemand letter;
    letter.glyphs[0] = 1;
    return decideText(view, advancePerPx, letter, previous);
}

// TPO letters: the TPO style's letter width (from the owner's TPO lab) stays TPO's own
// largest size, floored to whole pixels and never below the rule's smallest.
inline double tpoLetterPx(const ChartTextFit& fit, const ChartTextRule& rule, double styleWidth) {
    if (!fit.on) return 0;
    const double cap = std::isfinite(styleWidth) ? std::floor(styleWidth) : fit.sizePx;
    return std::max(rule.minPx, std::min(cap, fit.sizePx));
}
} // namespace chart
