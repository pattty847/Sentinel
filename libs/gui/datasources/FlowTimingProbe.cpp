#include "FlowTimingProbe.hpp"
#include "SentinelLogging.hpp"
#include "../../core/marketdata/model/TradeData.h"
#include <algorithm>

namespace {
using Status = SentinelStreamClient::FlowStatus;
struct ViewBars { size_t bars = 0, building = 0; };
// Bars held and buckets still building/partial inside [from, to) of one series.
ViewBars viewBars(const FlowSeriesBuffer& buffer, int64_t tf, int64_t from, int64_t to) {
    ViewBars out;
    const auto* series = buffer.series(tf);
    if (!series || to <= from) return out;
    for (auto it = series->bars.lower_bound(from); it != series->bars.end() && it->first < to; ++it) ++out.bars;
    for (auto it = series->coverage.lower_bound(from); it != series->coverage.end() && it->first < to; ++it)
        out.building += it->second.status == Status::Building || it->second.status == Status::Partial;
    return out;
}
}

FlowTimingProbe::FlowTimingProbe() : m_on(sentinel::logging::probeEnabled("flow.timing")) {}

void FlowTimingProbe::reset() {
    m_symbol.clear();
    m_tf = m_startMs = m_endMs = 0;
    m_sentId = 0;
    m_sentAtMs = 0;
    m_loggedHold = FlowBackfillState::Hold::Sent;
    m_fill.reset();
}

FlowTimingProbe::Range FlowTimingProbe::fillRange(int64_t startMs, int64_t endMs, int64_t tf, int64_t windowStartMs,
                                                  int64_t windowEndMs, int64_t wallNowMs) {
    if (tf <= 0 || endMs <= startMs || startMs < 0) return {};
    Range out{startMs / tf * tf, ((endMs - 1) / tf + 1) * tf};
    out.from = std::max(out.from, windowStartMs);
    out.to = std::min({out.to, windowEndMs, (wallNowMs / tf + 1) * tf});
    if (out.to <= out.from) return {};
    return out;
}

bool FlowTimingProbe::covered(const FlowBackfillState& state, int64_t wallNowMs) const {
    const auto range = fillRange(m_startMs, m_endMs, m_tf, state.windowStartMs(), state.windowEndMs(), wallNowMs);
    return state.covers(range.from, range.to);
}

void FlowTimingProbe::sent(const FlowBackfillState::Request& request, int64_t nowMs) {
    if (!m_on) return;
    m_sentId = request.id;
    m_sentAtMs = nowMs;
    m_loggedHold = FlowBackfillState::Hold::Sent;
    sLog_Probe("flow.timing", "send id=" << request.id << " symbol=" << request.symbol << " tfMs="
               << request.timeframeMs << " endMs=" << request.endMs << " limit=" << request.limit
               << " refresh=" << request.refresh << " probe=" << request.probe << " atMs=" << nowMs);
}

void FlowTimingProbe::held(const FlowBackfillState& state, int64_t nowMs) {
    if (!m_on) return;
    const auto hold = state.lastHold();
    if (hold == FlowBackfillState::Hold::Sent || hold == FlowBackfillState::Hold::Idle ||
        hold == FlowBackfillState::Hold::NoSelection) {
        m_loggedHold = hold;
        return;
    }
    if (hold == m_loggedHold || !state.workDue(nowMs)) return;
    m_loggedHold = hold;
    const int64_t waitMs = hold == FlowBackfillState::Hold::EdgeThrottle ? state.nextEdgePageMs() - nowMs
        : hold == FlowBackfillState::Hold::InFlight ? nowMs - m_sentAtMs
        : state.retryAtMs() - nowMs;
    sLog_Probe("flow.timing", "held reason=" << FlowBackfillState::holdName(hold) << " symbol=" << m_symbol
               << " tfMs=" << m_tf << (hold == FlowBackfillState::Hold::InFlight ? " inFlightMs=" : " dueInMs=")
               << waitMs << " id=" << m_sentId);
}

void FlowTimingProbe::failed(quint64 id, int64_t nowMs) {
    if (!m_on) return;
    sLog_Probe("flow.timing", "failed id=" << id << " rttMs=" << (id == m_sentId ? nowMs - m_sentAtMs : -1));
}

void FlowTimingProbe::replied(const SentinelStreamClient::FlowBatch& batch, bool accepted, int64_t nowMs,
                              double applyMs, double consumersMs) {
    if (!m_on) return;
    size_t rows = 0;
    for (const auto& record : batch.bars) rows += record.bar.rows.size();
    sLog_Probe("flow.timing", "reply id=" << batch.id << " symbol=" << batch.symbol << " tfMs=" << batch.timeframeMs
               << " accepted=" << accepted << " rttMs=" << (batch.id == m_sentId ? nowMs - m_sentAtMs : -1)
               << " wireBytes=" << batch.wireBytes << " decodeMs=" << batch.decodeMs << " bars=" << batch.bars.size()
               << " rows=" << rows << " ranges=" << batch.ranges.size() << " applyMs=" << applyMs
               << " consumersMs=" << consumersMs);
}

void FlowTimingProbe::logFilled(const FlowFillTracker::Filled& filled, const FlowSeriesBuffer& buffer) const {
    const auto inView = viewBars(buffer, m_tf, m_startMs / m_tf * m_tf, m_endMs);
    sLog_Probe("flow.timing", "view filled symbol=" << m_symbol << " tfMs=" << m_tf << " view=[" << m_startMs << ".."
               << m_endMs << ") sinceLastChangeMs=" << filled.sinceLastChangeMs << " sinceFirstChangeMs="
               << filled.sinceFirstChangeMs << " pages=" << filled.pages << " changes=" << filled.changes
               << " barsInView=" << inView.bars << " buildingInView=" << inView.building);
}

void FlowTimingProbe::viewChanged(const QString& symbol, int64_t tf, int64_t startMs, int64_t endMs,
                                  const FlowBackfillState& state, const FlowSeriesBuffer& buffer, int64_t nowMs,
                                  int64_t wallNowMs) {
    if (!m_on) return;
    if (symbol != m_symbol || tf != m_tf) {
        reset();
        m_symbol = symbol;
        m_tf = tf;
    }
    m_startMs = startMs;
    m_endMs = endMs;
    if (const auto filled = m_fill.viewChanged(startMs, endMs, covered(state, wallNowMs), nowMs))
        logFilled(*filled, buffer);
}

void FlowTimingProbe::pageApplied(const FlowBackfillState& state, const FlowSeriesBuffer& buffer, int64_t nowMs,
                                  int64_t wallNowMs) {
    if (!m_on || m_tf <= 0) return;
    if (const auto filled = m_fill.pageApplied(covered(state, wallNowMs), nowMs)) logFilled(*filled, buffer);
}

size_t approxBookBytes(const LiveOrderBook& book) {
    const auto side = [](size_t slotCount) {
        const size_t words = (slotCount + 63) / 64, blocks = (slotCount + LiveOrderBook::SumBlockSlots - 1) /
            LiveOrderBook::SumBlockSlots;
        return slotCount * sizeof(double) + words * sizeof(uint64_t) + blocks * (sizeof(double) + sizeof(uint8_t));
    };
    return side(book.getBids().capacity()) + side(book.getAsks().capacity());
}

void logClientMemory(const CandleSeriesBuffer* candles, const FlowSeriesBuffer& flow,
                     const std::unordered_map<std::string, std::unique_ptr<LiveOrderBook>>& books) {
    const auto c = candles ? candles->approxMemory() : CandleSeriesBuffer::MemoryApprox{};
    const auto f = flow.approxMemory();
    size_t bookBytes = 0, bookSlots = 0;
    for (const auto& [symbol, book] : books) {
        if (!book) continue;
        bookBytes += approxBookBytes(*book);
        bookSlots += book->getBids().capacity() + book->getAsks().capacity();
    }
    sLog_Probe("client.mem", "candles series=" << c.series << " bars=" << c.bars << " ringSlots=" << c.ringSlots
               << " approxRingBytes=" << c.ringBytes
               << " | flow symbol=" << flow.symbol() << " series=" << f.series << " bars=" << f.bars << " rows="
               << f.rows << " approxRowBytes=" << f.rowBytes << " approxNotableBytes=" << f.notableBytes
               << " approxNodeBytes=" << f.nodeBytes << " approxTotalBytes=" << f.totalBytes()
               << " coverageRanges=" << f.coverageRanges << " coverage=[" << f.coveredFromMs << ".."
               << f.coveredToMs << ")"
               << " | books count=" << books.size() << " slots=" << bookSlots << " approxBytes=" << bookBytes);
}
