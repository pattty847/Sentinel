#include "TradeOverlayPublisher.hpp"
#include "../protocol/SentinelStreamProtocol.hpp"
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

namespace trade_overlay {
bool Grid::valid() const {
    return width > 0 && width <= kMaxGridWidth && rows > 0 && rows <= kMaxRows &&
        std::isfinite(tick) && tick > 0 && tick <= 1e6 &&
        std::isfinite(maxPrice) && maxPrice > 0 && maxPrice <= 1e12 && minPrice() >= 0;
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
TimeWindow historyWindow(const Request& q, bool tpo) {
    const auto anchor = q.endMs > 0 ? std::min(q.endMs, q.nowMs) : q.nowMs;
    const auto session = SessionManager::sessionContaining(anchor - 1, q.session);
    const int64_t origin = tpo ? session.startMs : 0;
    const int64_t tf = tpo ? q.tpoMs : q.footprintMs;
    if (tf <= 0 || !session.valid || anchor < origin || q.count <= 0) return {};
    const int64_t limit = tpo ? std::min(anchor, session.endMs) : anchor;
    const int64_t end = origin + (limit - origin) / tf * tf;
    const int64_t width = tpo ? (session.endMs - origin) / tf : q.grid.width;
    const auto count = std::min<int64_t>({q.count, width, kMaxColumns});
    return {std::max<int64_t>(tpo ? origin : 0, end - count * tf), end};
}
}
TimeWindow tradeWindow(const Request& q) {
    if (q.kind != Kind::Live) return historyWindow(q, q.kind == Kind::TpoHistory);
    const auto session = SessionManager::sessionContaining(q.nowMs, q.session);
    auto start = session.startMs; // VP needs the session; footprint may precede its open.
    for (const auto bucket : liveBuckets(q.nowMs, q.previousMs, q.footprintMs))
        start = std::min(start, bucket);
    return {std::max<int64_t>(0, start), q.nowMs + 1};
}
CandleFetchResult fetchTpoCandles(const Request& q, int64_t retainedFromMs,
                                const CandleFetcher& fetch, const StopRequested& stopped) {
    CandleFetchResult result;
    result.ok = true;
    if (q.kind != Kind::TpoHistory) return result;
    auto window = historyWindow(q, true);
    if (retainedFromMs > 0) {
        // Include the first partially retained minute, without widening the page.
        const auto minuteEnd = retainedFromMs / 60000 * 60000 + (retainedFromMs % 60000 ? 60000 : 0);
        window.endMs = std::min(window.endMs, minuteEnd);
    }
    if (window.empty()) return result;
    // Requests may partition a session at non-minute boundaries; REST still
    // requires whole minute pages. Apply each candle to the periods it overlaps.
    window.startMs = window.startMs / 60000 * 60000;
    window.endMs = (window.endMs + 59999) / 60000 * 60000;
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
        const auto end = std::min<int64_t>(window.endMs, cursor + batch * 60000LL);
        auto page = fetch(cursor / 1000, end / 1000, static_cast<int>((end - cursor) / 60000));
        if (stopped && stopped()) {
            result.ok = false; result.error = "overlay history cancelled"; return result;
        }
        if (!page.ok || page.candles.size() > batch) {
            result.ok = false;
            result.error = page.ok ? "TPO candle response exceeds page budget" : page.error;
            return result;
        }
        for (const auto& bar : page.candles) {
            if (bar.timestamp_ms >= cursor && bar.timestamp_ms < end && bar.timestamp_ms % 60000 == 0)
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
using Trade = ServerDataModel::FootprintTradeSample;
struct Builder {
    const Request& q;
    const std::vector<Trade>& trades;
    const std::vector<OHLCVBar>& candles;
    int64_t retainedFromMs;
    Result out;
    size_t bytes = 0;
    void send(Json j) {
        auto data = j.dump();
        if (bytes + data.size() > kMaxBytes) { out.error = "overlay output budget exceeded"; return; }
        bytes += data.size(); out.messages.push_back(std::move(data));
    }
    template<class F> void scan(int64_t start, int64_t end, F fn) const {
        auto it = std::lower_bound(trades.begin(), trades.end(), start,
            [](const Trade& t, int64_t ms) { return t.timestampMs < ms; });
        for (; it != trades.end() && it->timestampMs < end; ++it) {
            if (!std::isfinite(it->price) || !std::isfinite(it->size) || it->size <= 0) continue;
            const double row = std::floor((out.grid.maxPrice - it->price) / out.grid.tick);
            if (row >= 0 && row < out.grid.rows) fn(static_cast<int>(row), *it);
        }
    }
    Json column(int64_t start, int64_t tf, bool tpo) const {
        Json j = {{"time_start", start}, {"time_end", start + tf},
                  {"min_price", out.grid.minPrice()}, {"max_price", out.grid.maxPrice},
                  {"tick_size", out.grid.tick}};
        if (tpo) {
            QByteArray letters(out.grid.rows, '\0');
            const auto session = SessionManager::sessionContaining(start, q.session);
            const auto origin = session.valid ? session.startMs : 0;
            const char letter = 'A' + ((start - origin) / tf) % 26;
            scan(start, start + tf, [&](int row, const Trade&) { letters[row] = letter; });
            if (q.kind == Kind::TpoHistory) {
                auto bar = std::lower_bound(candles.begin(), candles.end(), start - 59999,
                    [](const OHLCVBar& b, int64_t time) { return b.timestamp_ms < time; });
                for (; bar != candles.end() && bar->timestamp_ms < start + tf; ++bar) {
                    if (retainedFromMs > 0 && bar->timestamp_ms >= retainedFromMs) continue;
                    if (!std::isfinite(bar->high) || !std::isfinite(bar->low) || bar->high < bar->low) continue;
                    const double first = std::floor((out.grid.maxPrice - bar->high) / out.grid.tick);
                    const double last = std::floor((out.grid.maxPrice - bar->low) / out.grid.tick);
                    if (last < 0 || first >= out.grid.rows) continue;
                    const int lo = static_cast<int>(std::max(0.0, first));
                    const int hi = static_cast<int>(std::min(double(out.grid.rows - 1), last));
                    for (int row = lo; row <= hi; ++row) letters[row] = letter;
                }
            }
            j["letters"] = letters.toBase64().toStdString(); j["format"] = "tpo_ascii";
        } else {
            std::vector<double> delta(out.grid.rows, 0);
            scan(start, start + tf, [&](int row, const Trade& t) {
                if (t.side == AggressorSide::Buy) delta[row] += t.size;
                else if (t.side == AggressorSide::Sell) delta[row] -= t.size;
            });
            double peak = 0;
            for (double v : delta) peak = std::max(peak, std::abs(v));
            const double scale = peak > 0 ? std::max(1e-9, peak / 32767.0) : 1.0;
            QByteArray data(out.grid.rows * 2, '\0');
            for (int row = 0; row < out.grid.rows; ++row) {
                const auto code = static_cast<uint16_t>(32768 +
                    static_cast<int>(std::clamp(std::round(delta[row] / scale), -32768.0, 32767.0)));
                qToLittleEndian(code, reinterpret_cast<uchar*>(data.data()) + row * 2);
            }
            j["delta_levels_q16"] = data.toBase64().toStdString();
            j["quant_scale"] = scale; j["format"] = "q16_delta";
        }
        return j;
    }
    void columns(bool tpo, bool live) {
        const int64_t tf = tpo ? q.tpoMs : q.footprintMs;
        const auto anchor = q.endMs > 0 ? std::min(q.endMs, q.nowMs) : q.nowMs;
        const auto session = SessionManager::sessionContaining(live ? q.nowMs : anchor - 1, q.session);
        if (!session.valid || (tpo && (session.endMs - session.startMs) % tf != 0)) { out.error = "invalid overlay session"; return; }
        const int64_t origin = tpo ? session.startMs : 0;
        const int width = tpo ? static_cast<int>((session.endMs - session.startMs + tf - 1) / tf) : out.grid.width;
        if (width <= 0 || width > kMaxGridWidth) { out.error = "overlay timeframe exceeds session column budget"; return; }
        std::vector<int64_t> starts;
        if (live) {
            starts = liveBuckets(q.nowMs, q.previousMs, tf, origin);
            if (tpo) {
                std::erase_if(starts, [&](int64_t start) {
                    return start < session.startMs || start >= session.endMs || start + tf > session.endMs;
                });
            }
        } else {
            const auto window = historyWindow(q, tpo);
            for (auto start = window.startMs; start < window.endMs; start += tf)
                if (start > 0) starts.push_back(start);
        }
        auto columns = Json::array();
        for (auto start : starts) columns.push_back(column(start, tf, tpo));
        Json j = {{"type", tpo ? "tpo_history_chunk" : "footprint_history_chunk"},
            {"schema_version", tpo ? protocol::SentinelProtocol::kTpoSchemaVersion : protocol::SentinelProtocol::kFootprintSchemaVersion},
            {"symbol", q.symbol}, {"timeframe_ms", tf}, {"session_type", static_cast<int>(q.session)},
            {"grid_width", width}, {"grid_height", out.grid.rows},
            {"format", tpo ? "tpo_ascii" : "q16_delta"}, {"encoding", "base64"}, {"columns", std::move(columns)}};
        // Live uses the existing per-column messages, preserving consumer routing.
        if (live) {
            for (auto& col : j["columns"]) {
                auto one = j; one.erase("columns");
                one["type"] = tpo ? "tpo_slice" : "footprint_slice";
                one.update(col); send(std::move(one));
            }
        } else send(std::move(j));
    }
    void profile() {
        const auto session = SessionManager::sessionContaining(q.nowMs, q.session);
        if (!session.valid) return;
        std::vector<double> bins(out.grid.rows, 0);
        scan(session.startMs, session.endMs, [&](int row, const Trade& t) { bins[row] += t.size; });
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
            {"session_type", static_cast<int>(q.session)}, {"grid_height", out.grid.rows},
            {"min_price", out.grid.minPrice()}, {"max_price", out.grid.maxPrice}, {"tick_size", out.grid.tick},
            {"total_volume", total}, {"poc_price", out.grid.maxPrice - (poc + .5) * out.grid.tick},
            {"vah_price", out.grid.maxPrice - lo * out.grid.tick},
            {"val_price", out.grid.maxPrice - (hi + 1) * out.grid.tick},
            {"format", "vp_f32"}, {"encoding", "base64"}, {"volume_bins", data.toBase64().toStdString()}});
    }
};
}
Result build(const Request& q, const std::vector<Trade>& trades,
             const std::vector<OHLCVBar>& candles, int64_t retainedFromMs) {
    if (retainedFromMs == 0 && !trades.empty()) retainedFromMs = trades.front().timestampMs;
    Builder b{q, trades, candles, retainedFromMs, {q.grid, {}, {}}};
    if (q.symbol.empty() || q.nowMs <= 0 || q.footprintMs < 1000 || q.footprintMs > 86400000 ||
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
    if (q.kind != Kind::TpoHistory) b.columns(false, q.kind == Kind::Live);
    if (q.kind != Kind::FootprintHistory) b.columns(true, q.kind == Kind::Live);
    if (q.kind == Kind::Live) b.profile();
    if (!b.out.error.empty()) b.out.messages.clear();
    return b.out;
}
} // namespace trade_overlay
