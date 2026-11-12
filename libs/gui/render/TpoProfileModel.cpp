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

int historyPageBudget(int sessionType, int64_t periodMs, int sessions) {
    if (periodMs <= 0 || sessions <= 0) return 0;
    const auto type = static_cast<SessionManager::SessionType>(sessionType);
    const int64_t duration = SessionManager::sessionDurationMs(type);
    const int64_t pagePeriods = std::min<int64_t>(kMaxPagePeriods, kMaxPageMs / periodMs);
    if (duration <= 0 || pagePeriods <= 0) return 0;
    const int64_t periods = (duration + periodMs - 1) / periodMs;
    const int64_t perSession = (periods + pagePeriods - 1) / pagePeriods;
    return static_cast<int>(std::min<int64_t>(kMaxHistoryPages, perSession * sessions));
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

int initialBalancePeriods(int64_t periodMs) {
    constexpr int64_t kHourMs = 3'600'000;
    if (periodMs <= 0) return 1;
    return static_cast<int>(std::max<int64_t>(1, kHourMs / periodMs));
}

void ProfileRows::build(const std::vector<std::vector<int>>& rowsByPeriod,
                        int64_t topAbs, int group, int ibPeriods) {
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
    m_ib = {};
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
            if (p < ibPeriods) {
                m_ib.low = m_ib.low < 0 ? r : std::min(m_ib.low, r);
                m_ib.high = std::max(m_ib.high, r);
            }
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

const char* levelName(Level level) {
    static constexpr const char* kNames[] = {"POC", "VAH", "VAL", "IBH", "IBL"};
    return kNames[static_cast<int>(level)];
}

const Element& levelElement(const StyleModel& style, Level level) {
    static constexpr Element StyleModel::* kFields[] = {
        &StyleModel::poc, &StyleModel::vah, &StyleModel::val, &StyleModel::ibh, &StyleModel::ibl};
    return style.*kFields[static_cast<int>(level)];
}

LevelLines levelLines(const ProfileRows& profile, const StyleModel& style, int64_t topAbs, double basePx) {
    LevelLines out;
    const double cellH = basePx * profile.group();
    auto rowTop = [&](int row) {
        return static_cast<double>(topAbs - (profile.minGroup() + row + 1) * profile.group()) * basePx;
    };
    auto add = [&](Level level, int row, double offset) {
        if (row < 0 || !levelElement(style, level).visible) return;
        out.lines[static_cast<size_t>(out.count++)] = {level, rowTop(row) + offset};
    };
    const ValueArea& va = profile.valueArea();
    const InitialBalance& ib = profile.initialBalance();
    if (va.valid()) {
        add(Level::Poc, va.poc, cellH * 0.5);
        add(Level::Vah, va.high, 0.0);
        add(Level::Val, va.low, cellH);
    }
    if (ib.valid()) {
        add(Level::Ibh, ib.high, 0.0);
        add(Level::Ibl, ib.low, cellH);
    }
    return out;
}

unsigned placeTags(const LevelLines& lines, double offsetY, double chipHeight, double top, double bottom) {
    unsigned kept = 0;
    std::array<double, 5> placed{};
    int placedCount = 0;
    for (int i = 0; i < lines.count; ++i) {
        const double y = lines.lines[static_cast<size_t>(i)].y + offsetY;
        if (!std::isfinite(y) || y - chipHeight * 0.5 < top || y + chipHeight * 0.5 > bottom) continue;
        bool clear = true;
        for (int k = 0; k < placedCount; ++k) clear = clear && std::abs(placed[static_cast<size_t>(k)] - y) >= chipHeight;
        if (!clear) continue;
        placed[static_cast<size_t>(placedCount++)] = y;
        kept |= 1u << i;
    }
    return kept;
}

} // namespace tpo
