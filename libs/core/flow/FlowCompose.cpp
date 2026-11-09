#include "FlowCompose.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>

namespace sentinel::flow {
FlowBar composeFlowBars(std::span<const FlowBar> bars, int64_t timeframeMs) {
    if (bars.empty() || timeframeMs <= 0) throw std::invalid_argument("empty flow compose");
    FlowBar out;
    out.startMs = bars.front().startMs;
    out.timeframeMs = timeframeMs;
    out.nativeTick = bars.front().nativeTick;
    out.lowRow = bars.front().lowRow;
    out.open = bars.front().open;
    out.close = bars.back().close;
    out.high = bars.front().high;
    out.low = bars.front().low;
    std::map<int64_t, Row> rows;
    int64_t previous = out.startMs - 1;
    for (const auto& bar : bars) {
        if (bar.startMs <= previous || bar.startMs < out.startMs ||
            bar.startMs >= out.startMs + timeframeMs || bar.nativeTick != out.nativeTick)
            throw std::invalid_argument("incompatible flow bars");
        previous = bar.startMs;
        out.lowRow = std::min(out.lowRow, bar.lowRow);
        out.high = std::max(out.high, bar.high);
        out.low = std::min(out.low, bar.low);
        out.baseVolume += bar.baseVolume;
        out.quoteVolume += bar.quoteVolume;
        out.count += bar.count;
        out.buyVol += bar.buyVol;
        out.sellVol += bar.sellVol;
        out.revision = std::max(out.revision, bar.revision);
        for (const auto& row : bar.rows) {
            auto& dest = rows[bar.lowRow + row.offset];
            dest.bidVol += row.bidVol; dest.askVol += row.askVol;
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
