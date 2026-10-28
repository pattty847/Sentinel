#pragma once
// Wall radar text on the chart: one badge per wall row (notional, wall age, new or
// flicker count; no notional while the heatmap draws its own cell value labels)
// right-aligned at the ladder strip's left edge, and up to four arrow pills at the
// drawn window's edges for the nearest wall per side beyond each edge.
// Render thread (LadderLayer::rebuild). Glyph templates are taken from the atlas once
// per atlas; a layout composes fixed char buffers, so nothing allocates after the
// constructor's reservations.
//
// The look is a proposal the owner judges live: every number below is a named
// constant for tuning.
#include "render/ChartTextPrimitives.hpp"
#include <QRectF>
#include <QSGGeometry>
#include <QtGui/qrgb.h>
#include <array>
#include <cstdint>
#include <vector>

class ChartTextAtlas;
struct FrameContext;

namespace chart {
struct LadderSnapshot;

constexpr int kMaxBadges = 64;      // row badges per frame, first from the top; the rest are counted
constexpr int kEdgePills = 4;       // [bid|ask] x [top|bottom edge]
constexpr int kBadgeChars = 24;     // longest badge text plus room
constexpr int kMaxBadgeGlyphs = (kMaxBadges + kEdgePills) * kBadgeChars;
constexpr int kArrowFilledVertices = 3, kArrowHollowVertices = 18; // one triangle, three stroke quads
constexpr int kMaxBadgeVertices = kMaxBadges * 6 + kEdgePills * (6 + kArrowHollowVertices);
constexpr uint32_t kNewMs = 10'000; // a wall for less than this is "new"

// Geometry, logical px.
constexpr double kBadgeFontPx = 12;
constexpr double kBadgePadX = 5, kBadgePadY = 2;
constexpr double kPillH = kBadgeFontPx + 2 * kBadgePadY; // a row badge needs a row at least this tall
constexpr double kBadgeInsetPx = 4;                       // right edge of every pill, left of the strip
constexpr double kEdgeInsetPx = 4;                        // edge pills from the surface's top and bottom
constexpr double kPillGapPx = 2;                          // between stacked edge pills
constexpr double kArrowPx = 8, kArrowGapPx = 4, kArrowStrokePx = 1.5;

// Colours. Text is straight ARGB (the text material); geometry is premultiplied
// (QSGVertexColorMaterial) from the straight values here.
constexpr QRgb kBidText = qRgba(191, 246, 255, 255);
constexpr QRgb kAskText = qRgba(255, 224, 160, 255);
constexpr QRgb kHotText = qRgba(255, 208, 77, 255);
struct BadgeRgba { uchar r, g, b, a; };
constexpr BadgeRgba kPillBackSrgb{12, 16, 22, 235};
constexpr BadgeRgba kBidArrowSrgb{0, 255, 255, 255};
constexpr BadgeRgba kAskArrowSrgb{255, 200, 0, 255};

// Text formatting into a caller's buffer (no terminator); each returns the length.
// Money: three significant digits, $999 / $12.3k / $1.07M / $2.10B, $999B+ beyond.
int formatMoney(double usd, char* out);
// Age from a saturated wall age: 12s / 3m12s / 2h05m / 99h+.
int formatAge(uint32_t ms, char* out);
// Flicker count: x1 .. x65535.
int formatFlickers(uint32_t n, char* out);

struct BadgePill {
    enum Colour : uint8_t { Bid, Ask, Hot };
    QRectF rect; // the background, logical px, on device pixels
    double textLeft = 0, textTop = 0;
    char text[kBadgeChars] = {};
    int length = 0;
    Colour colour = Bid;
    int8_t arrow = 0; // +1 up (wall above), -1 down (below), 0 a row badge
    bool hollow = false; // the side's scan stopped short: a larger wall may lie farther out
};

struct BadgeStats {
    int rowBadges = 0, edgePills = 0, dropped = 0, glyphs = 0;
};

class LadderBadges {
public:
    LadderBadges();
    // Lays out the badges and pills of a drawn snapshot on this frame; nothing when the
    // radar is off or the atlas is not built. cellLabels: the heatmap draws its cell
    // value labels this frame, so row badges leave out the notional.
    void layout(const LadderSnapshot& s, const FrameContext& f, const ChartTextAtlas* atlas, bool cellLabels);
    // Writes backgrounds and arrows (at most kMaxBadgeVertices); returns the count.
    int writeGeometry(QSGGeometry::ColoredPoint2D* v) const;
    const std::vector<ChartGlyphInstance>& glyphs() const { return glyphs_; }
    const std::vector<BadgePill>& pills() const { return pills_; }
    const BadgeStats& stats() const { return stats_; }
    size_t capacityBytes() const; // tests

private:
    struct UnitGlyph {
        float left = 0, top = 0, width = 0, height = 0, advance = 0;
        QRectF uv;
        bool ink = false;
    };
    bool prepareAtlas(const ChartTextAtlas* atlas);
    double textWidth(const BadgePill& p) const;
    void placeEdges(const LadderSnapshot& s, const FrameContext& f);
    void placeRows(const LadderSnapshot& s, const FrameContext& f, bool cellLabels);
    bool placeRow(const LadderSnapshot& s, const FrameContext& f, int index, bool cellLabels);
    void finishPill(BadgePill& p, double right, double top, double dpr);
    void compose(const BadgePill& p);
    std::array<UnitGlyph, 128> unit_{};
    const ChartTextAtlas* atlas_ = nullptr;
    qint64 atlasKey_ = 0;
    double scale_ = 0, boxH_ = 0;
    std::vector<BadgePill> pills_;
    std::vector<ChartGlyphInstance> glyphs_;
    BadgeStats stats_;
};
} // namespace chart
