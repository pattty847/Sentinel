#include "chart/layers/LadderBadges.hpp"
#include "chart/layers/LadderGrid.hpp"
#include "chart/layers/LadderLayer.hpp"
#include "render/ChartTextAtlas.hpp"
#include "render/FrameContext.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace chart {
namespace {
double snap(double v, double dpr) { return std::round(v * dpr) / dpr; }

constexpr BadgeRgba premultiplied(BadgeRgba c) {
    return {uchar((c.r * c.a + 127) / 255), uchar((c.g * c.a + 127) / 255), uchar((c.b * c.a + 127) / 255), c.a};
}
constexpr BadgeRgba kPillBack = premultiplied(kPillBackSrgb);
constexpr BadgeRgba kBidArrow = premultiplied(kBidArrowSrgb);
constexpr BadgeRgba kAskArrow = premultiplied(kAskArrowSrgb);

int writeUint(uint64_t v, char* out, int minDigits = 1) {
    char digits[20];
    int n = 0;
    do {
        digits[n++] = char('0' + v % 10);
        v /= 10;
    } while (v);
    while (n < minDigits) digits[n++] = '0';
    for (int i = 0; i < n; ++i) out[i] = digits[n - 1 - i];
    return n;
}

int append(char* out, const char* text) {
    const int n = int(std::strlen(text));
    std::memcpy(out, text, size_t(n));
    return n;
}

void vertex(QSGGeometry::ColoredPoint2D& v, double x, double y, BadgeRgba c) {
    v.set(float(x), float(y), c.r, c.g, c.b, c.a);
}

int triangle(QSGGeometry::ColoredPoint2D* v, const QPointF (&p)[3], BadgeRgba c) {
    for (int i = 0; i < 3; ++i) vertex(v[i], p[i].x(), p[i].y(), c);
    return 3;
}

// A stroke of width kArrowStrokePx centred on the segment a-b, as two triangles.
int stroke(QSGGeometry::ColoredPoint2D* v, QPointF a, QPointF b, BadgeRgba c) {
    const QPointF d = b - a;
    const double length = std::hypot(d.x(), d.y());
    const QPointF n = length > 0 ? QPointF(-d.y(), d.x()) * (0.5 * kArrowStrokePx / length) : QPointF();
    const QPointF q[6] = {a + n, a - n, b + n, b + n, a - n, b - n};
    for (int i = 0; i < 6; ++i) vertex(v[i], q[i].x(), q[i].y(), c);
    return 6;
}

int quad(QSGGeometry::ColoredPoint2D* v, const QRectF& r, BadgeRgba c) {
    const QPointF q[6] = {r.topLeft(), r.topRight(), r.bottomLeft(), r.bottomLeft(), r.topRight(), r.bottomRight()};
    for (int i = 0; i < 6; ++i) vertex(v[i], q[i].x(), q[i].y(), c);
    return 6;
}

// The arrow left of the pill's text: pointing up at a wall above, down at one below.
int arrow(QSGGeometry::ColoredPoint2D* v, const BadgePill& p) {
    const double x = p.rect.left() + kBadgePadX, y = p.rect.center().y() - 0.5 * kArrowPx;
    const double apexY = p.arrow > 0 ? y : y + kArrowPx, baseY = p.arrow > 0 ? y + kArrowPx : y;
    const QPointF corners[3] = {{x + 0.5 * kArrowPx, apexY}, {x, baseY}, {x + kArrowPx, baseY}};
    const BadgeRgba c = p.colour == BadgePill::Bid ? kBidArrow : kAskArrow;
    if (!p.hollow) return triangle(v, corners, c);
    int n = stroke(v, corners[0], corners[1], c);
    n += stroke(v + n, corners[1], corners[2], c);
    return n + stroke(v + n, corners[2], corners[0], c);
}

using EdgeSlots = std::array<std::array<const WallCandidate*, 2>, 2>;

QRgb textColour(BadgePill::Colour c) { return c == BadgePill::Hot ? kHotText : c == BadgePill::Ask ? kAskText : kBidText; }
} // namespace

int formatMoney(double usd, char* out) {
    static constexpr char kUnits[] = {0, 'k', 'M', 'B'};
    double v = std::isfinite(usd) && usd > 0 ? usd : 0;
    int unit = 0, decimals = 0;
    for (;;) {
        decimals = v < 9.995 ? 2 : v < 99.95 ? 1 : 0;
        if (decimals > 0 || std::round(v) < 1000) break;
        if (unit == 3) return append(out, "$999B+");
        v /= 1000;
        ++unit;
    }
    const uint64_t scale = decimals == 2 ? 100 : decimals == 1 ? 10 : 1;
    const uint64_t r = uint64_t(std::llround(v * double(scale)));
    int n = 0;
    out[n++] = '$';
    n += writeUint(r / scale, out + n);
    if (decimals) {
        out[n++] = '.';
        n += writeUint(r % scale, out + n, decimals);
    }
    if (kUnits[unit]) out[n++] = kUnits[unit];
    return n;
}

int formatAge(uint32_t ms, char* out) {
    const uint32_t s = ms / 1000;
    int n = 0;
    if (s < 60) {
        n += writeUint(s, out);
        out[n++] = 's';
    } else if (s < 3600) {
        n += writeUint(s / 60, out);
        out[n++] = 'm';
        n += writeUint(s % 60, out + n, 2);
        out[n++] = 's';
    } else if (s < 100 * 3600) {
        n += writeUint(s / 3600, out);
        out[n++] = 'h';
        n += writeUint(s / 60 % 60, out + n, 2);
        out[n++] = 'm';
    } else {
        n = append(out, "99h+");
    }
    return n;
}

int formatFlickers(uint32_t count, char* out) {
    out[0] = 'x';
    return 1 + writeUint(count, out + 1);
}

LadderBadges::LadderBadges() {
    pills_.reserve(kMaxBadges + kEdgePills);
    glyphs_.reserve(kMaxBadgeGlyphs);
}

size_t LadderBadges::capacityBytes() const {
    return pills_.capacity() * sizeof(BadgePill) + glyphs_.capacity() * sizeof(ChartGlyphInstance);
}

bool LadderBadges::prepareAtlas(const ChartTextAtlas* atlas) {
    if (!atlas || !atlas->isBuilt() || atlas->fontPx() <= 0) return false;
    const qint64 key = atlas->image().cacheKey();
    if (atlas == atlas_ && key == atlasKey_) return true;
    const float top = atlas->glyphTopPx();
    for (int c = 32; c < int(unit_.size()); ++c) {
        const auto& g = atlas->glyph(QChar(c));
        unit_[size_t(c)] = {float(g.bounds.left()), float(g.bounds.top()) - top, float(g.bounds.width()),
                            float(g.bounds.height()), g.advance, g.uv, g.advance > 0 && !g.uv.isNull()};
    }
    scale_ = kBadgeFontPx / double(atlas->fontPx());
    boxH_ = double(atlas->glyphBottomPx() - top) * scale_;
    atlas_ = atlas;
    atlasKey_ = key;
    return true;
}

void LadderBadges::layout(const LadderSnapshot& s, const FrameContext& f, const ChartTextAtlas* atlas) {
    pills_.clear();
    glyphs_.clear();
    stats_ = {};
    if (!s.radar || !(f.raster.dpr > 0) || !prepareAtlas(atlas)) return;
    placeEdges(s, f);
    placeRows(s, f);
    for (const BadgePill& p : pills_) compose(p);
    stats_.glyphs = int(glyphs_.size());
}

double LadderBadges::textWidth(const BadgePill& p) const {
    double pen = 0;
    for (int i = 0; i < p.length; ++i) pen += unit_[size_t(p.text[i] & 0x7f)].advance;
    return pen * scale_;
}

// Right-aligned at `right` at `top` (both on device pixels: the callers snap them); the
// width rounds up to whole device pixels so the right edge stays where it was put.
void LadderBadges::finishPill(BadgePill& p, double right, double top, double dpr) {
    const double arrowW = p.arrow ? kArrowPx + kArrowGapPx : 0;
    const double width = std::ceil((2 * kBadgePadX + arrowW + textWidth(p)) * dpr) / dpr;
    p.rect = QRectF(right - width, top, width, kPillH);
    p.textLeft = snap(p.rect.left() + kBadgePadX + arrowW, dpr);
    p.textTop = snap(top + 0.5 * (kPillH - boxH_), dpr);
}

namespace {
// The largest found wall per [top|bottom edge][side], by real price against this
// frame's drawn window (the camera may have moved since the publish): on screen -> none.
EdgeSlots edgeSlots(const LadderSnapshot& s, const chart_raster::RasterCamera& cam) {
    EdgeSlots slot{};
    for (int side = 0; side < 2; ++side)
        for (const WallCandidate& c : s.walls[size_t(side)]) {
            const int edge = c.price >= cam.drawnMaxPrice ? 0 : c.price < cam.drawnMinPrice ? 1 : -1;
            if (!c.found || edge < 0) continue;
            const WallCandidate*& cell = slot[size_t(edge)][size_t(side)];
            if (!cell || c.size > cell->size) cell = &c;
        }
    return slot;
}

// Notional, then the distance from the drawn edge to the wall: $1.07M  +$61.0.
BadgePill edgePill(const WallCandidate& c, int side, bool top, const chart_raster::RasterCamera& cam) {
    BadgePill p;
    p.colour = side == LadderSnapshot::kAsk ? BadgePill::Ask : BadgePill::Bid;
    p.arrow = top ? 1 : -1;
    p.hollow = c.capped;
    p.length = formatMoney(double(c.size) * c.price, p.text);
    p.length += append(p.text + p.length, top ? "  +" : "  -");
    p.length += formatMoney(top ? c.price - cam.drawnMaxPrice : cam.drawnMinPrice - c.price, p.text + p.length);
    return p;
}

// Notional, seen-for, then the flicker count or "new".
void rowText(BadgePill& p, double usd, uint32_t seenMs, uint16_t flickers) {
    p.length = formatMoney(usd, p.text);
    p.length += append(p.text + p.length, "  ");
    p.length += formatAge(seenMs, p.text + p.length);
    if (flickers > 0) {
        p.length += append(p.text + p.length, "  ");
        p.length += formatFlickers(flickers, p.text + p.length);
    } else if (seenMs < kNewMs) {
        p.length += append(p.text + p.length, "  new");
    }
}

} // namespace

void LadderBadges::placeEdges(const LadderSnapshot& s, const FrameContext& f) {
    const auto& cam = f.raster;
    const EdgeSlots slot = edgeSlots(s, cam);
    const double right = snap(f.itemBounds.right() - kBadgeInsetPx, cam.dpr);
    for (int edge = 0; edge < 2; ++edge) {
        int stacked = 0; // the ask pill nearest the edge, the bid pill inward
        for (const int side : {LadderSnapshot::kAsk, LadderSnapshot::kBid}) {
            const WallCandidate* c = slot[size_t(edge)][size_t(side)];
            if (!c) continue;
            BadgePill p = edgePill(*c, side, edge == 0, cam);
            const double offset = kEdgeInsetPx + stacked * (kPillH + kPillGapPx);
            const double y = edge == 0 ? f.surfaceBounds.top() + offset : f.surfaceBounds.bottom() - offset - kPillH;
            finishPill(p, right, snap(y, cam.dpr), cam.dpr);
            pills_.push_back(p);
            ++stacked;
            ++stats_.edgePills;
        }
    }
}

void LadderBadges::placeRows(const LadderSnapshot& s, const FrameContext& f) {
    const auto& cam = f.raster;
    const auto window = ladderWindow(cam.drawnMinPrice, cam.drawnMaxPrice, s.binPrice());
    const int first = int(std::max<int64_t>(0, window.lo - s.binLo));
    const int last = int(std::min<int64_t>(s.rowCount - 1, window.hi - s.binLo));
    for (int i = last; i >= first; --i) { // from the top
        if (!(s.rows[size_t(i)].flags & (LadderRow::kBidLarge | LadderRow::kAskLarge))) continue;
        if (stats_.rowBadges >= kMaxBadges) {
            ++stats_.dropped;
            continue;
        }
        stats_.rowBadges += placeRow(s, f, i) ? 1 : 0;
    }
}

// One badge per row: the larger notional's side when both are walls (tie: ask). Only on
// a row at least kPillH tall, wholly inside the surface and clear of the edge pills.
bool LadderBadges::placeRow(const LadderSnapshot& s, const FrameContext& f, int index) {
    const auto& cam = f.raster;
    const LadderRow& row = s.rows[size_t(index)];
    const double bin = s.binPrice(), k = double(s.binLo + index);
    const double yTop = f.surfaceBounds.top() + cam.yDev((k + 1) * bin) / cam.dpr;
    const double yBottom = f.surfaceBounds.top() + cam.yDev(k * bin) / cam.dpr;
    if (yBottom - yTop < kPillH) return false;
    const double top = snap(0.5 * (yTop + yBottom - kPillH), cam.dpr);
    if (top < f.surfaceBounds.top() || top + kPillH > f.surfaceBounds.bottom()) return false;
    const double mid = (k + 0.5) * bin;
    const double bidUsd = (row.flags & LadderRow::kBidLarge) ? double(row.bid) * mid : -1;
    const double askUsd = (row.flags & LadderRow::kAskLarge) ? double(row.ask) * mid : -1;
    const bool ask = askUsd >= bidUsd;
    const bool hot = row.flags & (ask ? LadderRow::kAskHot : LadderRow::kBidHot);
    BadgePill p;
    p.colour = hot ? BadgePill::Hot : ask ? BadgePill::Ask : BadgePill::Bid;
    rowText(p, ask ? askUsd : bidUsd, ask ? row.askSeenMs : row.bidSeenMs, ask ? row.askFlickers : row.bidFlickers);
    finishPill(p, snap(f.itemBounds.right() - kBadgeInsetPx, cam.dpr), top, cam.dpr);
    for (int j = 0; j < stats_.edgePills; ++j)
        if (pills_[size_t(j)].rect.intersects(p.rect)) return false; // edge pills win
    pills_.push_back(p);
    return true;
}

// The pill's text origin is already on device pixels; glyphs keep their offsets from it.
void LadderBadges::compose(const BadgePill& p) {
    const QColor colour = QColor::fromRgba(textColour(p.colour));
    double pen = 0;
    for (int i = 0; i < p.length; ++i) {
        const UnitGlyph& u = unit_[size_t(p.text[i] & 0x7f)];
        if (u.ink) {
            ChartGlyphInstance glyph;
            glyph.rect = QRectF(p.textLeft + (pen + u.left) * scale_, p.textTop + u.top * scale_, u.width * scale_,
                                u.height * scale_);
            glyph.uv = u.uv;
            glyph.color = colour;
            glyph.debugAnchor = QPointF(p.textLeft, p.textTop);
            glyphs_.push_back(glyph);
        }
        pen += u.advance;
    }
}

int LadderBadges::writeGeometry(QSGGeometry::ColoredPoint2D* v) const {
    int n = 0;
    for (const BadgePill& p : pills_) {
        n += quad(v + n, p.rect, kPillBack);
        if (p.arrow) n += arrow(v + n, p);
    }
    return n;
}
} // namespace chart
