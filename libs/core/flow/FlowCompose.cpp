#include "FlowCompose.hpp"
#include "servermodel/RecordingCodec.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>

namespace sentinel::flow {
FlowBar composeFlowBars(std::span<const FlowBar> bars, int64_t timeframeMs) {
    if (bars.empty() || timeframeMs <= 0 || bars.front().timeframeMs <= 0 ||
        timeframeMs % bars.front().timeframeMs != 0)
        throw std::invalid_argument("invalid flow compose timeframe");
    FlowBar out;
    out.startMs = recording::floorDiv(bars.front().startMs, timeframeMs) * timeframeMs;
    out.timeframeMs = timeframeMs;
    out.nativeTick = bars.front().nativeTick;
    out.priceScale = bars.front().priceScale;
    out.baseScale = bars.front().baseScale;
    out.quoteScale = bars.front().quoteScale;
    out.lowRow = bars.front().lowRow;
    out.open = bars.front().open;
    out.close = bars.back().close;
    out.high = bars.front().high;
    out.low = bars.front().low;
    std::map<int64_t, Row> rows;
    int64_t previous = out.startMs - 1;
    const auto add = [](int64_t& to, int64_t from) {
        if (__builtin_add_overflow(to, from, &to)) throw std::overflow_error("flow compose volume overflow");
    };
    for (const auto& bar : bars) {
        if (bar.startMs <= previous || bar.startMs < out.startMs ||
            bar.startMs >= out.startMs + timeframeMs ||
            recording::floorDiv(bar.startMs, bar.timeframeMs) * bar.timeframeMs != bar.startMs ||
            bar.timeframeMs != bars.front().timeframeMs ||
            bar.nativeTick != out.nativeTick || bar.priceScale != out.priceScale ||
            bar.baseScale != out.baseScale || bar.quoteScale != out.quoteScale)
            throw std::invalid_argument("incompatible flow bars");
        previous = bar.startMs;
        out.lowRow = std::min(out.lowRow, bar.lowRow);
        out.high = std::max(out.high, bar.high);
        out.low = std::min(out.low, bar.low);
        add(out.baseVolume, bar.baseVolume);
        add(out.quoteVolume, bar.quoteVolume);
        out.count += bar.count;
        add(out.buyVol, bar.buyVol);
        add(out.sellVol, bar.sellVol);
        out.revision = std::max(out.revision, bar.revision);
        for (const auto& row : bar.rows) {
            auto& dest = rows[bar.lowRow + row.offset];
            add(dest.bidVol, row.bidVol); add(dest.askVol, row.askVol);
            dest.bidCount += row.bidCount; dest.askCount += row.askCount;
        }
        out.notable.insert(out.notable.end(), bar.notable.begin(), bar.notable.end());
    }
    for (auto& [r, row] : rows) {
        const auto offset = r - out.lowRow;
        if (offset < 0 || offset > INT16_MAX) throw std::out_of_range("flow row offset exceeds int16");
        row.offset = static_cast<int16_t>(offset);
        out.maxRowVol = std::max(out.maxRowVol, row.bidVol + row.askVol);
        out.rows.push_back(row);
    }
    out.delta = out.buyVol - out.sellVol;
    std::sort(out.notable.begin(), out.notable.end(), [](const Notable& a, const Notable& b) {
        const auto an = a.notional, bn = b.notional;
        if (an != bn) return an > bn;
        if (a.exchangeMs != b.exchangeMs) return a.exchangeMs < b.exchangeMs;
        return a.tradeId < b.tradeId;
    });
    if (out.notable.size() > NotableCapacity) out.notable.resize(NotableCapacity);
    return out;
}
} // namespace sentinel::flow
