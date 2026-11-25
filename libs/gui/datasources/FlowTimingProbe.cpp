#include "FlowTimingProbe.hpp"
#include "SentinelLogging.hpp"
#include "../../core/marketdata/model/TradeData.h"
#include <algorithm>
FlowTimingProbe::FlowTimingProbe() : m_on(sentinel::logging::probeEnabled("flow.timing")) {}
void FlowTimingProbe::reset() {
    m_waiting = m_fetched = false; m_symbol.clear(); m_tf = 0;
}
void FlowTimingProbe::viewChanged(const QString& symbol, int64_t tf, int64_t from, int64_t to, int64_t now) {
    if (!m_on) return;
    if (symbol != m_symbol || tf != m_tf) reset();
    if (!m_waiting) m_first = now;
    m_symbol = symbol; m_tf = tf; m_from = from; m_to = to; m_last = now;
    m_waiting = true; m_fetched = false;
}
std::optional<FlowTimingProbe::Filled> FlowTimingProbe::applied(const FlowSeriesBuffer& buffer,
                                                              int64_t now, int64_t wallNow) {
    if (!m_on || !m_waiting || m_tf <= 0) return {};
    const auto* series = buffer.series(m_tf);
    if (!series || buffer.symbol() != m_symbol) return {};
    Filled result{now - m_last, now - m_first, 0, 0, m_fetched};
    const auto end = std::min(m_to, wallNow / m_tf * m_tf + m_tf);
    for (auto time = m_from / m_tf * m_tf; time < end; time += m_tf) {
        const auto c = series->coverage.upper_bound(time);
        if (c == series->coverage.begin() || std::prev(c)->second.toMs <= time ||
            std::prev(c)->second.status == SentinelStreamClient::FlowStatus::Building) return {};
        if (const auto b = series->bars.find(time); b != series->bars.end()) {
            if (b->second.source == SentinelStreamClient::FlowSource::Store) ++result.sealed;
            else ++result.live;
        }
    }
    m_waiting = false;
    sLog_Probe("flow.timing", "view filled symbol=" << m_symbol << " tfMs=" << m_tf << " view=[" << m_from << ".."
               << m_to << ") source=" << (result.fetched ? "fetch" : result.live ? "live" : "cache")
               << " sinceLastChangeMs=" << result.sinceLastChangeMs << " sinceFirstChangeMs=" << result.sinceFirstChangeMs
               << " sealedBars=" << result.sealed << " liveBars=" << result.live);
    return result;
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
