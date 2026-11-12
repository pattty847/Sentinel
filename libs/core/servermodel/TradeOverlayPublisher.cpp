#include "TradeOverlayPublisher.hpp"
#include "../protocol/SentinelStreamProtocol.hpp"
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <map>

namespace trade_overlay {
bool Grid::valid() const {
    return rows > 0 && rows <= kMaxRows &&
        std::isfinite(tick) && tick > 0 && tick <= 1e6 &&
        std::isfinite(maxPrice) && maxPrice > 0 && maxPrice <= 1e12 && minPrice() >= 0;
}
SessionManager::SessionType volumeProfileSession(SessionManager::SessionType tpoSession) {
    const auto duration = SessionManager::sessionDurationMs(tpoSession);
    return (duration > 0 && duration <= 86400000) ? tpoSession : SessionManager::SessionType::H24;
}
const char* candleGranularityName(int64_t granularitySec) {
    switch (granularitySec) {
    case 60: return "ONE_MINUTE";
    case 300: return "FIVE_MINUTE";
    case 900: return "FIFTEEN_MINUTE";
    case 1800: return "THIRTY_MINUTE";
    case 3600: return "ONE_HOUR";
    case 7200: return "TWO_HOUR";
    case 21600: return "SIX_HOUR";
    case 86400: return "ONE_DAY";
    default: return nullptr;
    }
}
int64_t tpoCandleGranularityMs(int64_t periodMs, int64_t sessionStartMs) {
    int64_t best = 60000;
    for (const auto sec : kCandleGranularitiesSec) {
        const int64_t ms = sec * 1000;
        if (ms <= periodMs && periodMs % ms == 0 && sessionStartMs % ms == 0) best = ms;
    }
    return best;
}
Grid tpoGridFor(const Grid& base, SessionManager::SessionType session) {
    const int k = session == SessionManager::SessionType::M1 ? 10
                : session == SessionManager::SessionType::W1 ? 5 : 1;
    if (k == 1 || !base.valid()) return base;
    // Same row count, coarser tick, centred on the base band, aligned to its own tick.
    Grid grid = base;
    grid.tick = base.tick * k;
    const double centre = base.maxPrice - base.rows * base.tick / 2;
    grid.maxPrice = std::ceil((centre + base.rows * grid.tick / 2) / grid.tick) * grid.tick;
    if (grid.minPrice() < 0) grid.maxPrice = grid.rows * grid.tick;
    return grid.valid() ? grid : base;
}
std::vector<int64_t> liveBuckets(int64_t now, int64_t previous, int64_t tf, int64_t origin) {
    if (tf <= 0 || now < origin || now <= 0) return {};
    const auto bucket = origin + (now - origin) / tf * tf;
    std::vector<int64_t> result;
    if (previous > 0 && previous < bucket && bucket - tf >= origin)
        result.push_back(bucket - tf);
    result.push_back(bucket);
    return result;
}
namespace {
TimeWindow historyWindow(const Request& q) {
    const auto anchor = q.endMs > 0 ? std::min(q.endMs, q.nowMs) : q.nowMs;
    const auto session = SessionManager::sessionContaining(anchor - 1, q.session);
    const int64_t origin = session.startMs;
    const int64_t tf = q.tpoMs;
    if (tf <= 0 || !session.valid || anchor < origin || q.count <= 0) return {};
    const int64_t limit = std::min(anchor, session.endMs);
    const int64_t end = origin + (limit - origin) / tf * tf;
    const int64_t width = (session.endMs - origin) / tf;
    const auto count = std::min<int64_t>({q.count, width, kMaxColumns});
    return {std::max<int64_t>(origin, end - count * tf), end};
}
}
TimeWindow tradeWindow(const Request& q) {
    if (q.kind != Kind::Live) return historyWindow(q);
    // VP needs its session; live TPO needs only its last buckets.
    auto start = SessionManager::sessionContaining(q.nowMs, volumeProfileSession(q.session)).startMs;
    const auto tpoSession = SessionManager::sessionContaining(q.nowMs, q.session);
    if (tpoSession.valid)
        for (const auto bucket : liveBuckets(q.nowMs, q.previousMs, q.tpoMs, tpoSession.startMs))
            start = std::min(start, bucket);
    return {std::max<int64_t>(0, start), q.nowMs + 1};
}
CandleFetchResult fetchTpoCandles(const Request& q, int64_t retainedFromMs,
                                const CandleFetcher& fetch, const StopRequested& stopped) {
    CandleFetchResult result;
    result.ok = true;
    if (q.kind != Kind::TpoHistory) return result;
    auto window = historyWindow(q);
    const auto session = SessionManager::sessionContaining(window.startMs, q.session);
    const int64_t g = tpoCandleGranularityMs(q.tpoMs, session.valid ? session.startMs : 0);
    if (retainedFromMs > 0) {
        // Include the first partially retained candle, without widening the page.
        const auto candleEnd = retainedFromMs / g * g + (retainedFromMs % g ? g : 0);
        window.endMs = std::min(window.endMs, candleEnd);
    }
    if (window.empty()) return result;
    // Period boundaries are multiples of g from the session open, so these
    // roundings only matter for the one-minute fallback.
    window.startMs = window.startMs / g * g;
    window.endMs = (window.endMs + g - 1) / g * g;
    // Clients page long sessions (W1, M1) in windows of at most seven days.
    if (window.endMs - window.startMs > 7LL * 86400000) {
        result.ok = false; result.error = "TPO candle history exceeds input budget"; return result;
    }
    std::map<int64_t, OHLCVBar> minutes;
    constexpr int batch = 350;
    for (auto cursor = window.startMs; cursor < window.endMs;) {
        if (stopped && stopped()) {
            result.ok = false; result.error = "overlay history cancelled"; return result;
        }
        const auto end = std::min<int64_t>(window.endMs, cursor + batch * g);
        auto page = fetch(cursor / 1000, end / 1000, g / 1000, static_cast<int>((end - cursor) / g));
        if (stopped && stopped()) {
            result.ok = false; result.error = "overlay history cancelled"; return result;
        }
        if (!page.ok || page.candles.size() > batch) {
            result.ok = false;
            result.error = page.ok ? "TPO candle response exceeds page budget" : page.error;
            return result;
        }
        for (const auto& bar : page.candles) {
            if (bar.timestamp_ms >= cursor && bar.timestamp_ms < end && bar.timestamp_ms % g == 0)
                minutes[bar.timestamp_ms] = bar;
        }
        cursor = end;
    }
    result.candles.reserve(minutes.size());
    for (const auto& [_, bar] : minutes) result.candles.push_back(bar);
    return result;
}
namespace {
using Json = nlohmann::json;
using Trade = ServerDataModel::RetainedTradeSample;
// The only storage proportional to input size is the existing retained tape.
// Scan the retained tape so late trades participate; no sorted copy or timestamp cursor.
template<class Trades>
LiveSnapshot aggregateLiveTape(const Request& q, const Trades& trades) {
    LiveSnapshot out{q, q.grid, {}, {}, {}};
    if (q.kind != Kind::Live || q.symbol.empty() || q.nowMs <= 0 ||
        q.tpoMs < 60000 || q.tpoMs > 86400000 || q.count <= 0) {
        out.error = "invalid overlay request"; return out;
    }
    const auto window = tradeWindow(q);
    const auto before = [](const Trade& t, int64_t ms) { return t.timestampMs < ms; };
    const auto first = std::lower_bound(trades.begin(), trades.end(), window.startMs, before);
    const auto end = std::lower_bound(first, trades.end(), window.endMs, before);
    if (out.grid.maxPrice == 0) {
        // The tape is timestamp-sorted, with arrival order preserved for ties.
        const Trade* latest = first != end ? &*std::prev(end) : nullptr;
        if (latest && std::isfinite(latest->price) && latest->price > 0 &&
            std::isfinite(out.grid.tick) && out.grid.tick > 0) {
            const double bottom = std::max(0.0, std::floor(latest->price / out.grid.tick) - out.grid.rows / 2);
            out.grid.maxPrice = (bottom + out.grid.rows) * out.grid.tick;
        }
    }
    if (!out.grid.valid()) { out.error = "overlay grid unavailable or invalid"; return out; }
    const auto vp = SessionManager::sessionContaining(q.nowMs, volumeProfileSession(q.session));
    const auto tpo = SessionManager::sessionContaining(q.nowMs, q.session);
    if (!vp.valid || !tpo.valid || (tpo.endMs - tpo.startMs) % q.tpoMs != 0) {
        out.error = "invalid overlay session"; return out;
    }
    if ((tpo.endMs - tpo.startMs) / q.tpoMs > kMaxGridWidth) {
        out.error = "overlay timeframe exceeds session column budget"; return out;
    }
    const auto tpoGrid = tpoGridFor(out.grid, q.session);
    out.volumeBins.resize(out.grid.rows, 0);
    for (auto start : liveBuckets(q.nowMs, q.previousMs, q.tpoMs, tpo.startMs))
        if (start >= tpo.startMs && start + q.tpoMs <= tpo.endMs)
            out.columns.emplace_back(start, QByteArray(tpoGrid.rows, '\0'));
    for (auto it = first; it != end; ++it) {
        const auto& t = *it;
        if (!std::isfinite(t.price) || !std::isfinite(t.size) || t.size <= 0) continue;
        if (t.timestampMs >= vp.startMs && t.timestampMs < vp.endMs) {
            const double row = std::floor((out.grid.maxPrice - t.price) / out.grid.tick);
            if (row >= 0 && row < out.grid.rows) out.volumeBins[static_cast<int>(row)] += t.size;
        }
        for (auto& [start, letters] : out.columns) {
            if (t.timestampMs < start || t.timestampMs >= start + q.tpoMs) continue;
            const double row = std::floor((tpoGrid.maxPrice - t.price) / tpoGrid.tick);
            if (row >= 0 && row < tpoGrid.rows)
                letters[static_cast<int>(row)] = 'A' + ((start - tpo.startMs) / q.tpoMs) % 26;
        }
    }
    return out;
}
struct Builder {
    const Request& q;
    const std::vector<Trade>& trades;
    const std::vector<OHLCVBar>& candles;
    int64_t retainedFromMs;
    Result out;
    Grid tpoGrid;
    size_t bytes = 0;
    const LiveSnapshot* live = nullptr;
    void send(Json j) {
        auto data = j.dump();
        if (bytes + data.size() > kMaxBytes) { out.error = "overlay output budget exceeded"; return; }
        bytes += data.size(); out.messages.push_back(std::move(data));
    }
    template<class F> void scan(const Grid& grid, int64_t start, int64_t end, F fn) const {
        auto it = std::lower_bound(trades.begin(), trades.end(), start,
            [](const Trade& t, int64_t ms) { return t.timestampMs < ms; });
        for (; it != trades.end() && it->timestampMs < end; ++it) {
            if (!std::isfinite(it->price) || !std::isfinite(it->size) || it->size <= 0) continue;
            const double row = std::floor((grid.maxPrice - it->price) / grid.tick);
            if (row >= 0 && row < grid.rows) fn(static_cast<int>(row), *it);
        }
    }
    Json column(int64_t start, int64_t tf) const {
        const Grid& grid = tpoGrid;
        Json j = {{"time_start", start}, {"time_end", start + tf},
                  {"min_price", grid.minPrice()}, {"max_price", grid.maxPrice},
                  {"tick_size", grid.tick}};
        QByteArray letters(grid.rows, '\0');
        const auto session = SessionManager::sessionContaining(start, q.session);
        const auto origin = session.valid ? session.startMs : 0;
        const char letter = 'A' + ((start - origin) / tf) % 26;
        if (live) {
            for (const auto& [bucket, data] : live->columns)
                if (bucket == start) { letters = data; break; }
        } else scan(grid, start, start + tf, [&](int row, const Trade&) { letters[row] = letter; });
        if (q.kind == Kind::TpoHistory) {
            auto bar = std::lower_bound(candles.begin(), candles.end(), start,
                [](const OHLCVBar& b, int64_t time) { return b.timestamp_ms < time; });
            for (; bar != candles.end() && bar->timestamp_ms < start + tf; ++bar) {
                if (retainedFromMs > 0 && bar->timestamp_ms >= retainedFromMs) continue;
                if (!std::isfinite(bar->high) || !std::isfinite(bar->low) || bar->high < bar->low) continue;
                const double first = std::floor((grid.maxPrice - bar->high) / grid.tick);
                const double last = std::floor((grid.maxPrice - bar->low) / grid.tick);
                if (last < 0 || first >= grid.rows) continue;
                const int lo = static_cast<int>(std::max(0.0, first));
                const int hi = static_cast<int>(std::min(double(grid.rows - 1), last));
                for (int row = lo; row <= hi; ++row) letters[row] = letter;
            }
        }
        j["letters"] = letters.toBase64().toStdString(); j["format"] = "tpo_ascii";
        return j;
    }
    void columns(bool live) {
        const int64_t tf = q.tpoMs;
        const auto anchor = q.endMs > 0 ? std::min(q.endMs, q.nowMs) : q.nowMs;
        const auto session = SessionManager::sessionContaining(live ? q.nowMs : anchor - 1, q.session);
        if (!session.valid || (session.endMs - session.startMs) % tf != 0) { out.error = "invalid overlay session"; return; }
        const int64_t origin = session.startMs;
        const int width = static_cast<int>((session.endMs - session.startMs + tf - 1) / tf);
        if (width <= 0 || width > kMaxGridWidth) { out.error = "overlay timeframe exceeds session column budget"; return; }
        std::vector<int64_t> starts;
        if (live) {
            starts = liveBuckets(q.nowMs, q.previousMs, tf, origin);
            std::erase_if(starts, [&](int64_t start) {
                return start < session.startMs || start >= session.endMs || start + tf > session.endMs;
            });
        } else {
            const auto window = historyWindow(q);
            for (auto start = window.startMs; start < window.endMs; start += tf)
                if (start > 0) starts.push_back(start);
        }
        auto columns = Json::array();
        for (auto start : starts) columns.push_back(column(start, tf));
        Json j = {{"type", "tpo_history_chunk"},
            {"schema_version", protocol::SentinelProtocol::kTpoSchemaVersion},
            {"symbol", q.symbol}, {"timeframe_ms", tf}, {"session_type", static_cast<int>(q.session)},
            {"grid_width", width}, {"grid_height", out.grid.rows},
            {"format", "tpo_ascii"}, {"encoding", "base64"}, {"columns", std::move(columns)}};
        if (!live && !q.requestId.empty()) j["request_id"] = q.requestId;
        // Live uses the existing per-column messages, preserving consumer routing.
        if (live) {
            for (auto& col : j["columns"]) {
                auto one = j; one.erase("columns");
                one["type"] = "tpo_slice";
                one.update(col); send(std::move(one));
            }
        } else send(std::move(j));
    }
    void profile() {
        const auto vpSession = volumeProfileSession(q.session);
        const auto session = SessionManager::sessionContaining(q.nowMs, vpSession);
        if (!session.valid) return;
        const auto& bins = live->volumeBins;
        double total = 0; int poc = 0;
        for (int i = 0; i < out.grid.rows; ++i) { total += bins[i]; if (bins[i] > bins[poc]) poc = i; }
        int lo = poc, hi = poc; double covered = bins[poc];
        while (covered < total * .7) {
            const double above = lo > 0 ? bins[lo - 1] : -1;
            const double below = hi + 1 < out.grid.rows ? bins[hi + 1] : -1;
            if (above < 0 && below < 0) break;
            if (above >= below) covered += bins[--lo]; else covered += bins[++hi];
        }
        QByteArray data(out.grid.rows * 4, '\0');
        for (int i = 0; i < out.grid.rows; ++i) {
            float v = static_cast<float>(bins[i]); uint32_t bits; std::memcpy(&bits, &v, 4);
            qToLittleEndian(bits, reinterpret_cast<uchar*>(data.data()) + i * 4);
        }
        send({{"type", "volume_profile_slice"}, {"schema_version", protocol::SentinelProtocol::kVolumeProfileSchemaVersion},
            {"symbol", q.symbol}, {"session_start_ms", session.startMs}, {"session_end_ms", session.endMs},
            {"session_type", static_cast<int>(vpSession)}, {"grid_height", out.grid.rows},
            {"min_price", out.grid.minPrice()}, {"max_price", out.grid.maxPrice}, {"tick_size", out.grid.tick},
            {"total_volume", total}, {"poc_price", out.grid.maxPrice - (poc + .5) * out.grid.tick},
            {"vah_price", out.grid.maxPrice - lo * out.grid.tick},
            {"val_price", out.grid.maxPrice - (hi + 1) * out.grid.tick},
            {"format", "vp_f32"}, {"encoding", "base64"}, {"volume_bins", data.toBase64().toStdString()}});
    }
};
}
LiveSnapshot aggregateLive(const Request& q, const std::deque<Trade>& trades) {
    return aggregateLiveTape(q, trades);
}
Result buildLive(const LiveSnapshot& snapshot) {
    if (!snapshot.error.empty()) return {snapshot.grid, {}, snapshot.error};
    const std::vector<Trade> trades;
    const std::vector<OHLCVBar> candles;
    Builder b{snapshot.request, trades, candles, 0, {snapshot.grid, {}, {}}};
    b.live = &snapshot;
    b.tpoGrid = tpoGridFor(snapshot.grid, snapshot.request.session);
    b.columns(true);
    b.profile();
    if (!b.out.error.empty()) b.out.messages.clear();
    return b.out;
}
Result build(const Request& q, const std::vector<Trade>& trades,
             const std::vector<OHLCVBar>& candles, int64_t retainedFromMs) {
    if (q.kind == Kind::Live) return buildLive(aggregateLiveTape(q, trades));
    if (retainedFromMs == 0 && !trades.empty()) retainedFromMs = trades.front().timestampMs;
    Builder b{q, trades, candles, retainedFromMs, {q.grid, {}, {}}};
    if (q.symbol.empty() || q.nowMs <= 0 ||
        q.tpoMs < 60000 || q.tpoMs > 86400000 || q.count <= 0 || trades.size() > kMaxTrades || candles.size() > 7 * 1440) {
        b.out.error = "invalid overlay request or trade budget exceeded"; return b.out;
    }
    // Restart history can have candles before any retained trade has arrived.
    const double anchorPrice = !trades.empty() ? trades.back().price
        : (!candles.empty() ? candles.back().close : 0.0);
    if (b.out.grid.maxPrice == 0 && std::isfinite(anchorPrice) && anchorPrice > 0 &&
        b.out.grid.tick > 0 && std::isfinite(b.out.grid.tick)) {
        const double bottom = std::max(0.0, std::floor(anchorPrice / b.out.grid.tick) - b.out.grid.rows / 2);
        b.out.grid.maxPrice = (bottom + b.out.grid.rows) * b.out.grid.tick;
    }
    if (!b.out.grid.valid()) { b.out.error = "overlay grid unavailable or invalid"; return b.out; }
    b.tpoGrid = tpoGridFor(b.out.grid, q.session);
    b.columns(false);
    if (!b.out.error.empty()) b.out.messages.clear();
    return b.out;
}
} // namespace trade_overlay
