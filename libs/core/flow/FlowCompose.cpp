#include "FlowCompose.hpp"
#include "FlowRowStep.hpp"
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
    int64_t finestTick = INT64_MAX, coarsestTick = 0;
    double finestNativeTick = 0;
    for (const auto& bar : bars) {
        if (bar.priceScale != out.priceScale || bar.baseScale != out.baseScale || bar.quoteScale != out.quoteScale)
            throw std::invalid_argument("incompatible flow scales");
        const auto tick = chunks::tickUnits(bar.nativeTick, bar.priceScale);
        if (tick < finestTick) { finestTick = tick; finestNativeTick = bar.nativeTick; }
        coarsestTick = std::max(coarsestTick, tick);
        out.low = std::min(out.low, bar.low); out.high = std::max(out.high, bar.high);
    }
    for (const auto& bar : bars) {
        const auto tick = chunks::tickUnits(bar.nativeTick, bar.priceScale);
        if (tick % finestTick || !chunks::validStep(uint64_t(tick / finestTick)))
            throw std::invalid_argument("incompatible flow ticks");
    }
    const auto low = recording::priceUnits(out.low, double(out.priceScale));
    const auto high = recording::priceUnits(out.high, double(out.priceScale));
    if (!low || !high) throw std::invalid_argument("invalid flow prices");
    const auto step = chunks::stepFor(*low, *high, finestTick, uint64_t(coarsestTick / finestTick));
    int64_t outputTick;
    if (__builtin_mul_overflow(finestTick, int64_t(step), &outputTick))
        throw std::overflow_error("flow tick overflow");
    out.nativeTick = finestNativeTick * double(step);
    out.lowRow = recording::floorDiv(*low, outputTick);
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
            bar.priceScale != out.priceScale ||
            bar.baseScale != out.baseScale || bar.quoteScale != out.quoteScale)
            throw std::invalid_argument("incompatible flow bars");
        previous = bar.startMs;
        out.high = std::max(out.high, bar.high);
        out.low = std::min(out.low, bar.low);
        add(out.baseVolume, bar.baseVolume);
        add(out.quoteVolume, bar.quoteVolume);
        out.count += bar.count;
        add(out.buyVol, bar.buyVol);
        add(out.sellVol, bar.sellVol);
        const auto tick = chunks::tickUnits(bar.nativeTick, bar.priceScale);
        for (const auto& row : bar.rows) {
            auto& dest = rows[recording::floorDiv(bar.lowRow + row.offset, outputTick / tick)];
            add(dest.bidVol, row.bidVol); add(dest.askVol, row.askVol);
            dest.bidCount += row.bidCount; dest.askCount += row.askCount;
        }
        out.notable.insert(out.notable.end(), bar.notable.begin(), bar.notable.end());
    }
    for (auto& [r, row] : rows) {
        const auto offset = r - out.lowRow;
        if (offset < 0 || offset > chunks::MaxOffset) throw std::out_of_range("flow row offset exceeds u16");
        row.offset = static_cast<uint16_t>(offset);
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
