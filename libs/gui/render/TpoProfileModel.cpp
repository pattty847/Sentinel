#include "TpoProfileModel.hpp"

#include "../../core/servermodel/SessionManager.hpp"

#include <algorithm>
#include <cmath>

namespace tpo {

Layout parseLayout(std::string_view name, Layout fallback) {
    if (name == "split") return Layout::Split;
    if (name == "collapsed") return Layout::Collapsed;
    return fallback;
}

const char* layoutName(Layout layout) {
    return layout == Layout::Split ? "split" : "collapsed";
}

Theme parseTheme(std::string_view name, Theme fallback) {
    if (name == "rainbow") return Theme::Rainbow;
    if (name == "calm") return Theme::Calm;
    if (name == "sage") return Theme::Sage;
    return fallback;
}

const char* themeName(Theme theme) {
    switch (theme) {
    case Theme::Rainbow: return "rainbow";
    case Theme::Calm: return "calm";
    case Theme::Sage: return "sage";
    }
    return "rainbow";
}

int parseSessionType(std::string_view name, int fallback) {
    using SessionManager::SessionType;
    if (name == "ny") return static_cast<int>(SessionType::NY);
    if (name == "london") return static_cast<int>(SessionType::London);
    if (name == "asia") return static_cast<int>(SessionType::Asia);
    if (name == "australia") return static_cast<int>(SessionType::Australia);
    if (name == "h24") return static_cast<int>(SessionType::H24);
    if (name == "w1") return static_cast<int>(SessionType::W1);
    if (name == "m1") return static_cast<int>(SessionType::M1);
    return fallback;
}

const char* sessionTypeName(int sessionType) {
    static constexpr const char* kNames[] = {"ny", "london", "asia", "australia", "h24", "w1", "m1"};
    return (sessionType >= 0 && sessionType <= 6) ? kNames[sessionType] : "h24";
}

namespace {
constexpr int64_t kDayMs = 86'400'000;
constexpr int kMaxSessionPeriods = 2048;   // server kMaxGridWidth
constexpr int kMaxPagePeriods = 512;       // server kMaxColumns
constexpr int64_t kMaxPageMs = 7 * kDayMs; // server candle budget

bool validPeriod(int sessionType, int64_t periodMs) {
    const auto type = static_cast<SessionManager::SessionType>(sessionType);
    const int64_t duration = SessionManager::sessionDurationMs(type);
    if (periodMs < 60'000 || periodMs > kDayMs || duration <= 0) return false;
    if (duration % periodMs != 0 || duration / periodMs > kMaxSessionPeriods) return false;
    // Multi-day sessions must also split every day (M1 months differ in length).
    return duration <= kDayMs || kDayMs % periodMs == 0;
}
} // namespace

int64_t resolvePeriodMs(int sessionType, int64_t requestedMs) {
    if (validPeriod(sessionType, requestedMs)) return requestedMs;
    static constexpr int64_t kStandardMinutes[] = {15, 30, 60, 120, 240, 480, 1440};
    int64_t below = 0, smallest = 0;
    for (const int64_t minutes : kStandardMinutes) {
        const int64_t ms = minutes * 60'000;
        if (!validPeriod(sessionType, ms)) continue;
        if (smallest == 0) smallest = ms;
        if (ms <= requestedMs) below = ms;
    }
    return below > 0 ? below : (smallest > 0 ? smallest : 1'800'000);
}

std::vector<HistoryPage> historyPages(int sessionType, int64_t periodMs, int64_t nowMs,
                                      int sessions, int maxPages) {
    std::vector<HistoryPage> pages;
    if (periodMs <= 0 || nowMs <= 0 || sessions <= 0 || maxPages <= 0) return pages;
    const auto type = static_cast<SessionManager::SessionType>(sessionType);
    const int pagePeriods = static_cast<int>(std::min<int64_t>(kMaxPagePeriods, kMaxPageMs / periodMs));
    if (pagePeriods <= 0) return pages;
    auto session = SessionManager::sessionContaining(nowMs, type);
    std::vector<HistoryPage> sessionPages;
    for (int s = 0; s < sessions && session.valid; ++s) {
        const int64_t limit = s == 0 ? std::min(nowMs, session.endMs) : session.endMs;
        int64_t end = session.startMs + (limit - session.startMs) / periodMs * periodMs;
        sessionPages.clear();
        while (end > session.startMs) {
            const int count = static_cast<int>(std::min<int64_t>(pagePeriods, (end - session.startMs) / periodMs));
            if (count <= 0) break;
            sessionPages.push_back({end, count});
            end -= count * periodMs;
        }
        const int room = maxPages - static_cast<int>(pages.size());
        if (s == 0) {
            for (int i = 0; i < static_cast<int>(sessionPages.size()) && i < room; ++i) pages.push_back(sessionPages[i]);
        } else if (static_cast<int>(sessionPages.size()) <= room) {
            pages.insert(pages.end(), sessionPages.begin(), sessionPages.end());
        } else {
            break;
        }
        if (static_cast<int>(pages.size()) >= maxPages) break;
        session = SessionManager::sessionContaining(session.startMs - 1, type);
    }
    return pages;
}

char letterForPeriod(int period) {
    if (period < 0) return '?';
    const int idx = period % 52;
    return idx < 26 ? static_cast<char>('A' + idx) : static_cast<char>('a' + idx - 26);
}

int nextRowGroup(int group) {
    if (group < 1) return 1;
    int decade = 1;
    while (group >= decade * 10) decade *= 10;
    const int step = group / decade;  // 1, 2 (or 25 / 10 = 2 for 2.5), 5
    if (decade >= 10 && group == decade * 2) return decade * 5 / 2;  // 20 -> 25
    if (step == 1) return decade * 2;
    if (step == 2) return decade * 5;
    return decade * 10;
}

int rowGroupFor(double basePx, double targetRowPx) {
    if (!(basePx > 0.0) || !std::isfinite(basePx) || !(targetRowPx > 0.0)) return 1;
    int group = 1;
    while (basePx * group < targetRowPx && group < 100000000) group = nextRowGroup(group);
    return group;
}

ValueArea computeValueArea(const std::vector<int>& counts, double fraction) {
    ValueArea va;
    const int n = static_cast<int>(counts.size());
    long long total = 0;
    int first = -1, last = -1, best = 0;
    for (int i = 0; i < n; ++i) {
        if (counts[i] <= 0) continue;
        total += counts[i];
        if (first < 0) first = i;
        last = i;
        best = std::max(best, counts[i]);
    }
    if (total <= 0) return va;
    // POC: most TPOs; ties resolve to the row nearest the profile centre.
    const double centre = 0.5 * (first + last);
    for (int i = first; i <= last; ++i) {
        if (counts[i] != best) continue;
        if (va.poc < 0 || std::abs(i - centre) < std::abs(va.poc - centre)) va.poc = i;
    }
    const long long target = static_cast<long long>(std::ceil(static_cast<double>(total) * fraction));
    long long covered = counts[va.poc];
    int lo = va.poc, hi = va.poc;
    auto at = [&](int i) -> long long { return (i >= 0 && i < n) ? counts[i] : 0; };
    while (covered < target && (lo > first || hi < last)) {
        const bool canUp = hi < last;
        const bool canDown = lo > first;
        const long long up = canUp ? at(hi + 1) + at(hi + 2) : -1;
        const long long down = canDown ? at(lo - 1) + at(lo - 2) : -1;
        if (canUp && (!canDown || up >= down)) {
            // The convention compares and adds rows in pairs.
            covered += at(++hi);
            if (hi < last) covered += at(++hi);
        } else {
            covered += at(--lo);
            if (lo > first) covered += at(--lo);
        }
    }
    va.low = lo;
    va.high = hi;
    return va;
}

void ProfileRows::build(const std::vector<std::vector<int>>& rowsByPeriod,
                        int64_t topAbs, int group) {
    m_group = std::max(1, group);
    m_periods = static_cast<int>(rowsByPeriod.size());
    m_words = std::max(1, (m_periods + 63) / 64);
    int64_t minG = INT64_MAX, maxG = INT64_MIN;
    for (const auto& rows : rowsByPeriod) {
        for (const int row : rows) {
            const int64_t g = floorDiv(topAbs - row - 1, m_group);
            minG = std::min(minG, g);
            maxG = std::max(maxG, g);
        }
    }
    m_counts.clear();
    m_bits.clear();
    m_maxCount = 0;
    m_total = 0;
    m_va = {};
    if (minG > maxG) {
        m_rows = 0;
        m_minGroup = 0;
        return;
    }
    m_minGroup = minG;
    m_rows = static_cast<int>(maxG - minG + 1);
    m_bits.assign(static_cast<size_t>(m_rows) * m_words, 0);
    m_counts.assign(static_cast<size_t>(m_rows), 0);
    for (int p = 0; p < m_periods; ++p) {
        const uint64_t mask = uint64_t{1} << (p & 63);
        const size_t word = static_cast<size_t>(p >> 6);
        for (const int row : rowsByPeriod[static_cast<size_t>(p)]) {
            const int r = static_cast<int>(floorDiv(topAbs - row - 1, m_group) - minG);
            uint64_t& bits = m_bits[static_cast<size_t>(r) * m_words + word];
            if (!(bits & mask)) {
                bits |= mask;
                ++m_counts[static_cast<size_t>(r)];
            }
        }
    }
    for (const int c : m_counts) {
        m_total += c;
        m_maxCount = std::max(m_maxCount, c);
    }
    m_va = computeValueArea(m_counts);
}

namespace {
Rgba hsv(double hDeg, double s, double v, uint8_t a = 255) {
    const double h = std::fmod(std::fmod(hDeg, 360.0) + 360.0, 360.0) / 60.0;
    const double c = v * s;
    const double x = c * (1.0 - std::abs(std::fmod(h, 2.0) - 1.0));
    double r = 0, g = 0, b = 0;
    switch (static_cast<int>(h)) {
    case 0: r = c; g = x; break;
    case 1: r = x; g = c; break;
    case 2: g = c; b = x; break;
    case 3: g = x; b = c; break;
    case 4: r = x; b = c; break;
    default: r = c; b = x; break;
    }
    const double m = v - c;
    auto to8 = [](double f) { return static_cast<uint8_t>(std::clamp(std::lround(f * 255.0), 0L, 255L)); };
    return {to8(r + m), to8(g + m), to8(b + m), a};
}

Rgba mix(const Rgba& a, const Rgba& b, double t) {
    auto lerp = [t](uint8_t x, uint8_t y) {
        return static_cast<uint8_t>(std::lround(x + (static_cast<double>(y) - x) * t));
    };
    return {lerp(a.r, b.r), lerp(a.g, b.g), lerp(a.b, b.b), lerp(a.a, b.a)};
}

constexpr Rgba kPocCell{226, 229, 234, 255};
} // namespace

Rgba cellColor(Theme theme, int period, int periods, bool inValueArea, bool poc) {
    if (poc) return kPocCell;
    const double t = periods > 1 ? std::clamp(static_cast<double>(period) / (periods - 1), 0.0, 1.0) : 0.0;
    switch (theme) {
    case Theme::Rainbow: {
        // Red -> orange -> yellow -> green -> cyan -> blue -> violet across the session.
        const double hue = 280.0 * t;
        return inValueArea ? hsv(hue, 0.86, 0.92) : hsv(hue, 0.62, 0.52);
    }
    case Theme::Calm: {
        // Single blue hue; periods walk a gentle lightness ramp so letters stay distinct.
        const Rgba early = inValueArea ? Rgba{70, 112, 182, 255} : Rgba{40, 54, 80, 255};
        const Rgba late = inValueArea ? Rgba{98, 142, 210, 255} : Rgba{50, 66, 94, 255};
        return mix(early, late, t);
    }
    case Theme::Sage: {
        const Rgba early = inValueArea ? Rgba{104, 140, 100, 255} : Rgba{62, 80, 62, 255};
        const Rgba late = inValueArea ? Rgba{128, 160, 116, 255} : Rgba{72, 92, 70, 255};
        return mix(early, late, t);
    }
    }
    return kPocCell;
}

Rgba pocBarColor(Theme /*theme*/) {
    return {226, 229, 234, 70};
}

bool prefersDarkText(const Rgba& cell) {
    const double luma = 0.2126 * cell.r + 0.7152 * cell.g + 0.0722 * cell.b;
    return luma > 150.0;
}

} // namespace tpo
