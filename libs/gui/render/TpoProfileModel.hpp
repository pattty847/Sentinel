/*
 * Sentinel – TpoProfileModel
 *
 * Pure (Qt-free) market profile math for the TPO overlay:
 *   - session-relative period letters (A..Z, a..z, then repeat),
 *   - display row grouping by zoom (1-2-5 steps of the trade grid tick),
 *   - per display row period sets, TPO counts, POC and the 70% value area,
 *   - the period colour palettes.
 *
 * Row conventions: a trade grid base row i (0 = highest price) covers
 * [maxPrice - (i+1)*tick, maxPrice - i*tick). With topAbs = round(maxPrice/tick),
 * its absolute low index is L = topAbs - i - 1. A display row groups `group`
 * base rows: G = floor(L / group), covering [G*group*tick, (G+1)*group*tick).
 * Display rows are stored ascending in price (index 0 = lowest price).
 */
#pragma once

#include <bit>
#include <cstdint>
#include <string_view>
#include <vector>

namespace tpo {

enum class Layout : int { Split = 0, Collapsed = 1 };
enum class Theme : int { Rainbow = 0, Calm = 1, Sage = 2 };

Layout parseLayout(std::string_view name, Layout fallback);
const char* layoutName(Layout layout);
Theme parseTheme(std::string_view name, Theme fallback);
const char* themeName(Theme theme);

// Session names used by config: ny, london, asia, australia, h24, w1, m1.
// Returns a SessionManager::SessionType value, or fallback for unknown names.
int parseSessionType(std::string_view name, int fallback);
const char* sessionTypeName(int sessionType);

// Letter bracket for a session: requestedMs when it partitions the nominal
// session (and a UTC day, for multi-day sessions) into at most 2048 periods,
// otherwise the largest valid standard bracket below it (or the smallest valid).
int64_t resolvePeriodMs(int sessionType, int64_t requestedMs);

// One TPO history request: complete periods ending at endMs.
struct HistoryPage {
    int64_t endMs = 0;
    int count = 0;
};
// History requests, newest first: the current session (only completed periods;
// the forming one arrives live) and then whole earlier sessions, until `sessions`
// sessions or `maxPages` pages (see historyPageBudget). A page spans at most 7
// days and 512 periods (the server's candle and column budgets). An earlier
// session is included only when all of its pages fit.
std::vector<HistoryPage> historyPages(int sessionType, int64_t periodMs, int64_t nowMs,
                                      int sessions, int maxPages);
// Hard cap on pages per history run (pages are paced one at a time).
constexpr int kMaxHistoryPages = 64;
// Pages needed for `sessions` whole sessions, capped at kMaxHistoryPages.
int historyPageBudget(int sessionType, int64_t periodMs, int sessions);

// Session-relative letter: A..Z for periods 0..25, a..z for 26..51, then repeats.
char letterForPeriod(int period);

// Row group sequence: 1, 2, 5, 10, 20, 25, 50, 100, 200, 250, 500, ...
int nextRowGroup(int group);
// Smallest group in that sequence whose display row is at least targetRowPx
// tall. basePx = one base row (tick) in pixels.
int rowGroupFor(double basePx, double targetRowPx);

inline int64_t floorDiv(int64_t a, int64_t b) {
    const int64_t q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

struct ValueArea {
    int poc = -1;   // display row index with the most TPOs
    int low = -1;   // lowest display row inside the value area
    int high = -1;  // highest display row inside the value area
    bool valid() const { return poc >= 0; }
    bool contains(int row) const { return row >= low && row <= high; }
};

// Standard market profile value area: start at the POC (ties go to the row
// nearest the profile centre), then repeatedly add the next two rows above or
// below, whichever holds more TPOs, until `fraction` of all TPOs is covered.
ValueArea computeValueArea(const std::vector<int>& counts, double fraction = 0.70);

// Display rows of one session profile.
class ProfileRows {
public:
    // rowsByPeriod[p] lists the occupied base rows of period p.
    void build(const std::vector<std::vector<int>>& rowsByPeriod,
               int64_t topAbs, int group);

    int group() const { return m_group; }
    int rows() const { return m_rows; }
    int periods() const { return m_periods; }
    int64_t minGroup() const { return m_minGroup; }  // absolute G of display row 0
    int count(int row) const { return m_counts[static_cast<size_t>(row)]; }
    const std::vector<int>& counts() const { return m_counts; }
    int maxCount() const { return m_maxCount; }
    int totalTpos() const { return m_total; }
    const ValueArea& valueArea() const { return m_va; }
    bool has(int row, int period) const {
        const uint64_t word = m_bits[static_cast<size_t>(row) * m_words + static_cast<size_t>(period >> 6)];
        return (word >> (period & 63)) & 1u;
    }
    // Calls fn(period) for each period present in `row`, in period order.
    template <class F> void forEachPeriod(int row, F&& fn) const {
        const uint64_t* words = m_bits.data() + static_cast<size_t>(row) * m_words;
        for (int w = 0; w < m_words; ++w) {
            uint64_t bits = words[w];
            while (bits) {
                const int bit = std::countr_zero(bits);
                fn(w * 64 + bit);
                bits &= bits - 1;
            }
        }
    }

private:
    int m_group = 1;
    int m_rows = 0;
    int m_periods = 0;
    int m_words = 0;
    int64_t m_minGroup = 0;
    int m_maxCount = 0;
    int m_total = 0;
    std::vector<uint64_t> m_bits;
    std::vector<int> m_counts;
    ValueArea m_va;
};

struct Rgba {
    uint8_t r = 0, g = 0, b = 0, a = 255;
};

// Cell colour for one TPO. period/periods place the cell on the session gradient.
Rgba cellColor(Theme theme, int period, int periods, bool inValueArea, bool poc);
// Background bar drawn across the profile at the POC row.
Rgba pocBarColor(Theme theme);
// True when a letter on this cell should be dark rather than light.
bool prefersDarkText(const Rgba& cell);

} // namespace tpo
